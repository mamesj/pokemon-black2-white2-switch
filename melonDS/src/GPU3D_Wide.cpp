// ssrecomp: widescreen, full-resolution rendering of the 3D scene. See GPU3D_Wide.h.
#include "GPU3D_Wide.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <type_traits>
#include <mutex>
#include <thread>

#include "NDS.h"
#include "GPU.h"
#include "GPU3D.h"
#include "GPU3D_Soft.h"
#include "SsrThreads.h"
#include "SsrProf.h"
#include "GPU3D_GLThread.h"
#include "GPU3D_WideGpu.h"

namespace melonDS
{
WideState Wide;

bool YSort(Polygon* a, Polygon* b);

namespace
{
// Runs one job on several threads; the calling thread takes a share too.
class Workers
{
public:
    explicit Workers(int n)
    {
        for (int i = 0; i < n; i++) Threads.emplace_back([this, i] { SsrHelperThreadStart(i + 1); Loop(i); });
    }
    ~Workers()
    {
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Quit = true;
            Generation++;
        }
        Wake.notify_all();
        for (auto& t : Threads) t.join();
    }
    int Count() const { return (int)Threads.size(); }
    // Starts the job on all threads and returns; the job must stay valid until Wait().
    void Start(const std::function<void(int)>& job)
    {
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Job = &job;
            Remaining = (int)Threads.size();
            Generation++;
        }
        Wake.notify_all();
    }
    void Wait()
    {
        std::unique_lock<std::mutex> lock(Mutex);
        Done.wait(lock, [this] { return Remaining == 0; });
    }

private:
    void Loop(int index)
    {
        u64 seen = 0;
        for (;;)
        {
            const std::function<void(int)>* job;
            {
                std::unique_lock<std::mutex> lock(Mutex);
                Wake.wait(lock, [&] { return Generation != seen; });
                seen = Generation;
                if (Quit) return;
                job = Job;
            }
            SsrNoteCore(false);
            (*job)(index);
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Remaining--;
            }
            Done.notify_all();
        }
    }

    std::vector<std::thread> Threads;
    std::mutex Mutex;
    std::condition_variable Wake, Done;
    const std::function<void(int)>* Job = nullptr;
    u64 Generation = 0;
    int Remaining = 0;
    bool Quit = false;
};

// The widescreen rasteriser works on groups of pixels with the compiler's vector types
// (SSE/AVX on x86, NEON on ARM): 8 at a time where AVX2 is on, otherwise 4.
#if defined(__AVX2__)
constexpr int Lanes = 8;
#else
constexpr int Lanes = 4;
#endif
typedef float VF __attribute__((vector_size(Lanes * 4)));
typedef s32 VI __attribute__((vector_size(Lanes * 4)));
static inline VF Select(VI mask, VF a, VF b) { return (VF)(((VI)a & mask) | ((VI)b & ~mask)); }
static inline VI Select(VI mask, VI a, VI b) { return (a & mask) | (b & ~mask); }
static inline bool Any(VI mask)
{
    s32 r = 0;
    for (int i = 0; i < Lanes; i++) r |= mask[i];
    return r != 0;
}
static inline VI LaneIndex()
{
    VI v;
    for (int i = 0; i < Lanes; i++) v[i] = i;
    return v;
}
static inline VI Clamp63(VI v) { return Select(v < 0, VI{}, Select(v > 63, VI{} + 63, v)); }
static inline VF LaneIndexF() { return __builtin_convertvector(LaneIndex(), VF); }
// A texture coordinate (whole texels) wrapped, mirrored or clamped to the texture's size.
static inline VI WrapCoord(VI t, int size, bool repeat, bool mirror)
{
    if (!repeat) return Select(t < 0, VI{}, Select(t >= size, VI{} + (size - 1), t));
    const VI m = t & (size - 1);
    if (!mirror) return m;
    return Select((t & size) != 0, (size - 1) - m, m);
}

// One of the three pictures that take turns (see WideState::Front).
struct WideBuffer
{
    std::vector<u32> Own;
    u32* Pixels = nullptr; // Own's, or the graphics chip renderer's memory
    int W = 0, H = 0;
    bool OnGpu = false;
};

struct WideVertex
{
    float X, Y;      // output pixels
    float InvW;      // 1 / normalised W
    float Z;         // depth as the DS stores it (Z or W, see Polygon::WBuffer)
    float R, G, B;   // 6-bit colour
    float S, T;      // texture coordinates, 1/16 texel
};
}

struct WideAccess
{
    static inline std::vector<Vertex> VtxReal, VtxInterp;
    static inline std::vector<Polygon> PolyReal, PolyInterp;
    // The wide view shows about a third more of the scene than the hardware has room for.
    static constexpr u32 MaxPolygons = 4096, MaxVertices = 12288;
    static inline std::vector<Polygon*> ListReal, ListInterp;
    static inline u32 NumReal = 0, NumInterp = 0;
    static inline bool Dirty = false, NeedReal = false, PendingPublish = false;
    // The three pictures that take turns (see WideState::Front): drawn into Bufs[Next], which is
    // then published, and Next moves on.
    static inline WideBuffer Bufs[3];
    static inline int Next = 0;
    static inline u32* BackPx = nullptr; // Bufs[Next].Pixels while it is drawn
    // The buffer for the next picture, in memory of our own or the graphics chip renderer's.
    static u32* BackFor(int W, int H, bool gpu)
    {
        WideBuffer& b = Bufs[Next];
        b.Pixels = gpu ? WideGpuCurrent->Target(Next, W, H) : nullptr;
        b.OnGpu = b.Pixels != nullptr;
        if (!b.Pixels)
        {
            b.Own.resize((size_t)W * H);
            b.Pixels = b.Own.data();
        }
        b.W = W;
        b.H = H;
        return BackPx = b.Pixels;
    }
    static void Publish()
    {
        const WideBuffer& b = Bufs[Next];
        Wide.Front = b.Pixels;
        Wide.FrontW = b.W;
        Wide.FrontH = b.H;
        Wide.FrontSerial++;
        Next = (Next + 1) % 3;
    }
    static void ReleaseGpu()
    {
        Wait();
        for (WideBuffer& b : Bufs)
        {
            if (!b.OnGpu) continue;
            const u32* old = b.Pixels;
            b.Own.assign(old, old + (size_t)b.W * b.H);
            b.Pixels = b.Own.data();
            b.OnGpu = false;
            if (Wide.Front == old) Wide.Front = b.Pixels;
        }
    }
    // Per drawing thread, for the strip of rows it is drawing (see Draw): small enough to
    // stay in the processor's cache, unlike buffers the size of the picture.
    struct StripBuffers
    {
        std::vector<s32> Depth;
        std::vector<u8> Attr;    // bit 7 fog, bits 0-6 translucent polygon ID + 1
        std::vector<u16> Writer; // which opaque triangle (Tri::Order) drew the pixel
    };
    static inline std::unique_ptr<Workers> Pool;
    static inline bool Drawing = false;
    static inline bool DrawingOnGL = false; // by the graphics chip (GPU3D_GLThread.h)
    static inline bool DrawingOnWideGpu = false; // by the graphics chip (GPU3D_WideGpu.h)
    static inline int PendingSlot = 0;
    static inline bool SentOk = false; // the graphics chip took the picture (set by its job, see Draw)
    static inline u64 LateFrames = 0; // frames that went on without the graphics chip's picture (see OnRender)

    // Ends the drawing threads (at the end of the program; a new pool is made if needed).
    static void Shutdown()
    {
        Wait();
        Pool.reset();
        ListHelper::Stop();
        WideGpuSendStop();
    }

    // Blocks until the picture being drawn in the background is finished.
    static void Wait()
    {
        if (Drawing && DrawingOnWideGpu)
        {
            SsrWaitTimer t(SsrStats().WaitWideNs);
            static const bool prof = getenv("SSR_WIDE_PROF") != nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            // (On the sending thread the picture is fetched by its job, see Draw.)
            // (Not slot 3: the DS-resolution picture just sent, at the same line 215.)
            if (WideGpuSendOnThread())
            {
                WideGpuSendJoin(1);
                WideGpuSendJoin(2);
            }
            else if (WideGpuCurrent) WideGpuCurrent->Fetch(PendingSlot);
            if (prof)
            {
                static double total = 0;
                static u64 n = 0;
                total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (++n % 200 == 0) fprintf(stderr, "[wide] waiting for and fetching the graphics chip's picture: %.2f ms\n", total / n);
            }
        }
        else if (Drawing)
        {
#ifdef OGLRENDERER_ENABLED
            if (DrawingOnGL)
            {
                // (the renderer counts its own waiting)
                if (GLThreadRenderer::Current) GLThreadRenderer::Current->WaitIdle();
            }
            else
#endif
            {
                SsrWaitTimer t(SsrStats().WaitWideNs);
                Pool->Wait();
            }
        }
        Drawing = false;
        DrawingOnGL = false;
        DrawingOnWideGpu = false;
    }

    // Textures are decoded texel by texel as they are first used in a frame: an entry is
    // 0 until then, afterwards bit 31 is set and it holds red, green, blue (6 bits each,
    // from bit 0, 8 and 16) and alpha (5 bits from bit 24). Threads may decode the same
    // texel at once; they write the same value.
    struct Texture
    {
        u32 Param, Palette;   // Param without the wrapping bits
        int W, H;
        std::vector<u32> Texels;
    };
    static inline std::vector<std::unique_ptr<Texture>> Textures;

    // Which polygon of the previous frame each of this frame's polygons is paired with (-1 for
    // none), and whether it may share two corners with the polygon before it (see Replay).
    // (No initialisers: one is a static object of ListHelper, zeroed.)
    struct Pairing
    {
        std::vector<s32> Partner;
        std::vector<u8> KeepStrip;
        u32 Matched;
    };

    // Pairs the polygons of rawCur with those of rawPrev, for an in-between frame. The same for
    // the DS-resolution in-between frame and the wide view's, so it is worked out once where
    // the wide view's lists are built on a helper thread (ListHelper). `debug`: the pairs are
    // written to the log with SSR_PAIR_DEBUG.
    static void Pair(const std::vector<GPU3D::RawPolygon>& rawCur, const std::vector<GPU3D::RawPolygon>& rawPrev, bool debug,
        Pairing& out)
    {
        typedef GPU3D::RawPolygon RawPolygon;
        // Polygons are matched in submission order. When the two frames do not line up (an
        // object entered or left), the search skips ahead a little; an unmatched polygon is
        // drawn at its new position.
        auto sameKind = [](const RawPolygon& a, const RawPolygon& b) {
            return a.PolygonMode == b.PolygonMode && a.PolygonAttr == b.PolygonAttr &&
                a.TexParam == b.TexParam && a.TexPalette == b.TexPalette;
        };
        // How a polygon moved between the two frames: the mean movement of its vertices in
        // clip space relative to depth (1024 = the full screen width). Returns false if any
        // vertex moved more than an eighth of the screen: that is a different polygon or a
        // camera cut, not motion to smooth out.
        auto movement = [](const RawPolygon& a, const RawPolygon& b, int nverts, s64& mx, s64& my) -> bool {
            s64 sum[2] = { 0, 0 };
            for (int i = 0; i < nverts; i++)
            {
                s64 wa = a.Vertices[i].Position[3], wb = b.Vertices[i].Position[3];
                if (wa < 0) wa = -wa;
                if (wb < 0) wb = -wb;
                s64 w = (wa > wb ? wa : wb);
                if (w < 4096) w = 4096;
                for (int c = 0; c < 2; c++)
                {
                    s64 d = ((s64)b.Vertices[i].Position[c] - a.Vertices[i].Position[c]) * 512 / w;
                    if (d > 128 || d < -128) return false;
                    sum[c] += d;
                }
            }
            mx = sum[0] / nverts;
            my = sum[1] / nverts;
            return true;
        };
        // How far a pairing is from what the rest of the picture does: the scene as a whole
        // moves with the camera, so the right partner of a polygon is the one that moved
        // like its neighbours, not the nearest one. (Taking the nearest paired a polygon
        // with a different one of the same model whenever the picture moved by more than
        // half the distance between them: running, and most at the bottom of the screen.)
        auto unlike = [](const RawPolygon& a, const RawPolygon& b, int nverts, s64 rx, s64 ry) -> s64 {
            s64 total = 0;
            for (int i = 0; i < nverts; i++)
            {
                s64 wa = a.Vertices[i].Position[3], wb = b.Vertices[i].Position[3];
                if (wa < 0) wa = -wa;
                if (wb < 0) wb = -wb;
                s64 w = (wa > wb ? wa : wb);
                if (w < 4096) w = 4096;
                s64 dx = ((s64)b.Vertices[i].Position[0] - a.Vertices[i].Position[0]) * 512 / w - rx;
                s64 dy = ((s64)b.Vertices[i].Position[1] - a.Vertices[i].Position[1]) * 512 / w - ry;
                total += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            }
            return total;
        };

        // The movement most pairings agree on: every polygon votes with each candidate it
        // could be paired with; the true pairings all say the same, the false ones scatter.
        s64 sceneX = 0, sceneY = 0;
        if (!rawPrev.empty())
        {
            // (Per thread: the DS-resolution in-between frame and the wide one are built at once.)
            static thread_local std::vector<u16> votes;
            votes.assign(129 * 129, 0);
            const size_t np = rawPrev.size(), nc = rawCur.size();
            const size_t step = nc > 600 ? nc / 600 : 1;
            int best = 0;
            for (size_t i = 0; i < nc; i += step)
            {
                const RawPolygon& cur = rawCur[i];
                const int nverts = (cur.PolygonMode & 0x1) ? 4 : 3;
                // The lists differ by whole objects that came or went, so the partner is
                // near the same place counted from the front of the list or from its end.
                for (int pass = 0; pass < 2; pass++)
                {
                    const s64 centre = pass == 0 ? (s64)i : (s64)i + (s64)np - (s64)nc;
                    if (pass == 1 && np == nc) break;
                    const s64 lo = std::max<s64>(0, centre - 48), hi = std::min<s64>((s64)np, centre + 48);
                    for (s64 k = lo; k < hi; k++)
                    {
                        s64 mx, my;
                        if (!sameKind(rawPrev[k], cur) || !movement(rawPrev[k], cur, nverts, mx, my)) continue;
                        const int bx = (int)(mx + 128) / 2, by = (int)(my + 128) / 2;
                        const int n = ++votes[by * 129 + bx];
                        if (n > best)
                        {
                            best = n;
                            sceneX = bx * 2 - 128;
                            sceneY = by * 2 - 128;
                        }
                    }
                }
            }
        }

        out.Partner.assign(rawCur.size(), -1);
        out.KeepStrip.assign(rawCur.size(), 1);
        out.Matched = 0;
        size_t ia = 0;
        s64 lastPair = -1;
        static const bool dbgPairOn = getenv("SSR_PAIR_DEBUG") != nullptr;
        const bool dbgPair = dbgPairOn && debug;
        s64 dbgOff = 0;
        if (dbgPair) fprintf(stderr, "[pair] --- frame, scene motion %lld,%lld\n", (long long)sceneX, (long long)sceneY);
        for (size_t ci = 0; ci < rawCur.size(); ci++)
        {
            const RawPolygon& cur = rawCur[ci];
            const int nverts = (cur.PolygonMode & 0x1) ? 4 : 3;

            // Polygons of a strip share two corners with the one before, and the DS takes
            // those from the polygon already built. That is only right here if both were
            // moved the same way: both paired, with neighbours in the previous frame as
            // well, or both left where they are. Otherwise this one is built on its own;
            // sharing would give it two corners from the in-between position and the rest
            // from the new one, which tore faces apart (seen where a model meets the edge
            // of the picture and some of its polygons no longer pair).
            // Scenes are full of identical tiles and models, so of the candidates the one
            // that moved the way the scene did is taken.
            const RawPolygon* prev = nullptr;
            s64 bestMove = -1;
            size_t bestIndex = 0;
            // A partner has to have moved roughly as the scene did (within a few DS
            // pixels per vertex, more when the scene moves fast; that leaves room for things
            // that move by themselves).
            // Without that limit, when a model leaves the picture the next model of the
            // same kind was paired with it and drawn halfway towards where it had been.
            static const bool loose = getenv("SSR_PAIR_LOOSE") != nullptr;
            const s64 speed = (sceneX < 0 ? -sceneX : sceneX) + (sceneY < 0 ? -sceneY : sceneY);
            const s64 limit = loose ? INT64_MAX : nverts * (12 + speed / 2);
            // First the polygon that follows the last partner: within an object the two
            // lists run in step, and that also pairs what moves by itself (a windmill's
            // blades, a walking person) and what is so close to the camera that it moves
            // unlike the rest. A neighbouring tile or model is further off than this allows.
            if (ia < rawPrev.size() && !loose)
            {
                s64 mx, my;
                if (sameKind(rawPrev[ia], cur) && movement(rawPrev[ia], cur, nverts, mx, my) &&
                    unlike(rawPrev[ia], cur, nverts, sceneX, sceneY) <= nverts * 40)
                {
                    bestMove = 0;
                    bestIndex = ia;
                }
            }
            for (size_t k = ia; bestMove < 0 && k < rawPrev.size() && k < ia + 512; k++)
            {
                s64 mx, my;
                if (!sameKind(rawPrev[k], cur) || !movement(rawPrev[k], cur, nverts, mx, my)) continue;
                s64 m = unlike(rawPrev[k], cur, nverts, sceneX, sceneY);
                if (m > limit) continue;
                if (bestMove < 0 || m < bestMove)
                {
                    bestMove = m;
                    bestIndex = k;
                    if (m <= nverts * 6) break; // within a pixel or so per vertex of the scene's movement
                }
            }
            if (bestMove >= 0)
            {
                prev = &rawPrev[bestIndex];
                ia = bestIndex + 1;
            }
            const s64 pairIndex = prev ? (s64)bestIndex : -1;
            const bool keepStrip = (pairIndex < 0 && lastPair < 0) || (pairIndex >= 0 && lastPair >= 0 && pairIndex == lastPair + 1);
            lastPair = pairIndex;
            if (dbgPair)
            {
                const s64 off = pairIndex < 0 ? INT64_MIN : pairIndex - (s64)ci;
                if (off != dbgOff)
                    fprintf(stderr, "[pair] cur %zu/%zu -> prev %lld/%zu (mode %u, unlike %lld)\n", ci, rawCur.size(),
                        (long long)pairIndex, rawPrev.size(), cur.PolygonMode, (long long)bestMove);
                dbgOff = off;
            }
            out.Partner[ci] = (s32)pairIndex;
            out.KeepStrip[ci] = keepStrip ? 1 : 0;
            if (prev) out.Matched++;
        }
    }

    // Submits the recorded frame again into the given buffers. With `interpolate`, every
    // vertex is first moved halfway back towards where the same polygon was in the
    // previous frame; `matched` then counts the polygons found in both frames. The
    // horizontal clip coordinate is multiplied by xnum/xden.
    // The frame is rawCur, the one before it rawPrev (g's own RawCurrent and RawPrevious, or
    // copies of them when g is a helper's GPU3D, see ListHelper).
    static void Replay(GPU3D& g, bool interpolate, s64 xnum, s64 xden, Vertex* vram, Polygon* pram,
        Polygon** list, u32& numPolys, u32& matched, const std::vector<GPU3D::RawPolygon>& rawCur,
        const std::vector<GPU3D::RawPolygon>& rawPrev, u32 polyLimit = 2048, u32 vertLimit = 6144,
        const Pairing* pairing = nullptr)
    {
        const u32 savedPolyLimit = g.PolygonLimit, savedVertLimit = g.VertexLimit;
        g.PolygonLimit = polyLimit;
        g.VertexLimit = vertLimit;
        typedef GPU3D::RawPolygon RawPolygon;

        // Everything SubmitPolygon touches is saved and put back afterwards.
        Vertex savedTemp[4];
        memcpy(savedTemp, g.TempVertexBuffer, sizeof(savedTemp));
        u32 savedViewport[6];
        memcpy(savedViewport, g.Viewport, sizeof(savedViewport));
        const u32 savedPolygonMode = g.PolygonMode, savedAttr = g.CurPolygonAttr, savedTexParam = g.TexParam,
            savedTexPalette = g.TexPalette, savedConsecutive = g.NumConsecutivePolygons, savedDispCnt = g.DispCnt,
            savedNumVertices = g.NumVertices, savedNumPolygons = g.NumPolygons, savedNumOpaque = g.NumOpaquePolygons,
            savedSlotsFree = g.VertexSlotsFree;
        const s32 savedPipeline = g.PolygonPipeline, savedSlotCounter = g.VertexSlotCounter;
        Polygon* const savedLastStrip = g.LastStripPolygon;
        Vertex* const savedVertexRAM = g.CurVertexRAM;
        Polygon* const savedPolygonRAM = g.CurPolygonRAM;

        g.CurVertexRAM = vram;
        g.CurPolygonRAM = pram;
        g.NumVertices = g.NumPolygons = g.NumOpaquePolygons = 0;
        g.LastStripPolygon = nullptr;
        g.InReplay = true;

        // The pairs: worked out here, or already (see Pair).
        static thread_local Pairing local;
        const Pairing* pr = pairing;
        if (interpolate && !pr)
        {
            Pair(rawCur, rawPrev, xnum == xden, local);
            pr = &local;
        }
        matched = interpolate ? pr->Matched : 0;
        for (size_t ci = 0; ci < rawCur.size(); ci++)
        {
            const RawPolygon& cur = rawCur[ci];
            const int nverts = (cur.PolygonMode & 0x1) ? 4 : 3;
            memcpy(g.TempVertexBuffer, cur.Vertices, sizeof(cur.Vertices));

            // Polygons of a strip share two corners with the one before, and the DS takes
            // those from the polygon already built. That is only right here if both were
            // moved the same way: both paired, with neighbours in the previous frame as
            // well, or both left where they are. Otherwise this one is built on its own;
            // sharing would give it two corners from the in-between position and the rest
            // from the new one, which tore faces apart (seen where a model meets the edge
            // of the picture and some of its polygons no longer pair).
            bool keepStrip = true;
            if (interpolate)
            {
                keepStrip = pr->KeepStrip[ci] != 0;
                if (pr->Partner[ci] >= 0)
                {
                    const RawPolygon* prev = &rawPrev[pr->Partner[ci]];
                    for (int i = 0; i < nverts; i++)
                    {
                        Vertex& v = g.TempVertexBuffer[i];
                        const Vertex& p = prev->Vertices[i];
                        for (int c = 0; c < 4; c++) v.Position[c] = (s32)(((s64)v.Position[c] + p.Position[c]) / 2);
                        for (int c = 0; c < 3; c++) v.Color[c] = (v.Color[c] + p.Color[c]) / 2;
                        for (int c = 0; c < 2; c++)
                        {
                            // Texture coordinates that jump (wrapping animations) are left at their new value.
                            int d = v.TexCoords[c] - p.TexCoords[c];
                            if (d > -512 && d < 512) v.TexCoords[c] = (s16)((v.TexCoords[c] + p.TexCoords[c]) / 2);
                        }
                    }
                }
            }

            if (xnum != xden)
            {
                // A quad that exactly fills the DS's view (its corners on the corners of the
                // view, at one depth) is a layer drawn in front of the camera on purpose: the
                // weathers' scrolling backgrounds (Black 2's sandstorm haze, blizzard, rain;
                // pokebw2's Field3DBG_DrawBillboard) and full-screen covers. Squeezed like the
                // world it would only cover the middle of the wide view, so it keeps its size,
                // filling the whole view, and its texture goes on instead of stretching where
                // it repeats.
                bool fillsView = nverts == 4;
                bool left = false, right = false, top = false, bottom = false;
                for (int i = 0; fillsView && i < 4; i++)
                {
                    const s32* p = g.TempVertexBuffer[i].Position;
                    const s64 w = p[3], tol = std::max<s64>(16, std::abs(w) / 32);
                    if (w <= 0 || std::abs(std::abs((s64)p[0]) - w) > tol || std::abs(std::abs((s64)p[1]) - w) > tol ||
                        std::abs((s64)g.TempVertexBuffer[0].Position[3] - w) > tol)
                        fillsView = false;
                    left |= p[0] < 0;
                    right |= p[0] > 0;
                    top |= p[1] > 0;
                    bottom |= p[1] < 0;
                }
                fillsView = fillsView && left && right && top && bottom;
                if (fillsView)
                {
                    if (cur.TexParam & (1 << 16)) // repeats across: more of the texture, not a wider one
                    {
                        s32 centre = 0;
                        for (int i = 0; i < 4; i++) centre += g.TempVertexBuffer[i].TexCoords[0];
                        centre /= 4;
                        for (int i = 0; i < 4; i++)
                        {
                            s16& s = g.TempVertexBuffer[i].TexCoords[0];
                            s = (s16)std::clamp<s64>(centre + (s64)(s - centre) * xden / xnum, -32768, 32767);
                        }
                    }
                }
                else
                    for (int i = 0; i < nverts; i++)
                        g.TempVertexBuffer[i].Position[0] = (s32)((s64)g.TempVertexBuffer[i].Position[0] * xnum / xden);
            }

            g.PolygonMode = cur.PolygonMode;
            g.CurPolygonAttr = cur.PolygonAttr;
            g.TexParam = cur.TexParam;
            g.TexPalette = cur.TexPalette;
            memcpy(g.Viewport, cur.Viewport, sizeof(g.Viewport));
            g.NumConsecutivePolygons = cur.NumConsecutivePolygons;
            if (cur.StripStart || !keepStrip) g.LastStripPolygon = nullptr;
            g.SubmitPolygon();
        }

        numPolys = g.NumPolygons;
        if (g.NumPolygons)
        {
            u32 io = 0, it = g.NumOpaquePolygons;
            for (u32 i = 0; i < g.NumPolygons; i++)
            {
                Polygon* poly = &g.CurPolygonRAM[i];
                if (poly->Translucent)
                    list[it++] = poly;
                else
                    list[io++] = poly;
            }
            std::stable_sort(list, list + ((g.FlushAttributes & 0x1) ? g.NumOpaquePolygons : g.NumPolygons), YSort);
        }
        g.PolygonLimit = savedPolyLimit;
        g.VertexLimit = savedVertLimit;

        g.InReplay = false;
        memcpy(g.TempVertexBuffer, savedTemp, sizeof(savedTemp));
        memcpy(g.Viewport, savedViewport, sizeof(savedViewport));
        g.PolygonMode = savedPolygonMode;
        g.CurPolygonAttr = savedAttr;
        g.TexParam = savedTexParam;
        g.TexPalette = savedTexPalette;
        g.NumConsecutivePolygons = savedConsecutive;
        g.DispCnt = savedDispCnt;
        g.NumVertices = savedNumVertices;
        g.NumPolygons = savedNumPolygons;
        g.NumOpaquePolygons = savedNumOpaque;
        g.VertexSlotsFree = savedSlotsFree;
        g.PolygonPipeline = savedPipeline;
        g.VertexSlotCounter = savedSlotCounter;
        g.LastStripPolygon = savedLastStrip;
        g.CurVertexRAM = savedVertexRAM;
        g.CurPolygonRAM = savedPolygonRAM;
    }

    // The wide view's polygon lists built on a thread of their own (on the Switch, core 1,
    // where the DS-resolution 3D thread has little to do while the graphics chip draws the 3D),
    // instead of by the emulation as the frame is flushed. Measured in White 2's Aspertia City
    // on the Switch they took 5 ms of every frame the game drew its 3D (every other frame
    // there), the in-between frame's 3 ms more. The in-between lists are needed when the
    // in-between frame is drawn, 23 lines later, and are built first, while the emulation builds
    // the DS-resolution in-between frame itself; the real frame's lists are needed only a frame
    // later. Each list is built by a GPU3D of its own (only its SubmitPolygon is used), from
    // copies of the frame's raw polygons, so nothing the emulation goes on with is touched.
    // The in-between frame's pairs (Pair, most of its cost) are worked out here first, for the
    // wide view's lists and for the DS-resolution in-between frame, which the emulation then
    // builds from them when it is drawn (WideReplayPaired, GPU3D::VCount215): so the emulation
    // goes on through the 23 lines in between instead of pairing them itself.
    struct ListHelper
    {
        // A GPU3D that only builds polygon lists: no renderer.
        struct NoRenderer : Renderer3D
        {
            NoRenderer() : Renderer3D(false) {}
            void Reset(GPU&) override {}
            void RenderFrame(GPU&) override {}
            u32* GetLine(int) override { return Line; }
            u32 Line[256] {};
        };
        struct Job
        {
            // (No initialisers: a static object is zeroed, and they could not be used here.)
            std::unique_ptr<GPU3D> Engine;
            bool Interpolate;
            bool Busy, Started; // queued (or being built) / taken by the thread
        };
        // 0: the in-between frame's pairs, 1: its lists, 2: the real frame's lists (in that order).
        static inline Job Jobs[3];
        static inline Pairing Shared;
        static inline bool Paired; // pairs are being worked out for this frame
        static inline std::vector<GPU3D::RawPolygon> Cur, Prev;
        static inline s64 XNum = 1, XDen = 1;
        static inline float View = 256;
        static inline std::thread Thread;
        static inline std::mutex Mutex;
        static inline std::condition_variable Wake, Done;
        static inline bool Quit = false;

        static void Loop()
        {
            SsrHelperThreadStart(-1);
            std::unique_lock<std::mutex> lock(Mutex);
            for (;;)
            {
                auto next = []() -> int {
                    for (int i = 0; i < 3; i++)
                        if (Jobs[i].Busy && !Jobs[i].Started) return i;
                    return -1;
                };
                Wake.wait(lock, [&next] { return Quit || next() >= 0; });
                if (Quit) return;
                const int which = next();
                Job& job = Jobs[which];
                job.Started = true;
                lock.unlock();
                Build(which);
                lock.lock();
                job.Busy = false;
                Done.notify_all();
            }
        }
        static void Build(int which)
        {
            u32 matched;
            if (which == 0)
                Pair(Cur, Prev, true, Shared);
            else if (which == 1)
            {
                Replay(*Jobs[1].Engine, true, XNum, XDen, VtxInterp.data(), PolyInterp.data(), ListInterp.data(), NumInterp, matched,
                    Cur, Prev, MaxPolygons, MaxVertices, &Shared);
                ExtendToEdges(ListInterp.data(), NumInterp, View, true);
            }
            else
            {
                Replay(*Jobs[2].Engine, false, XNum, XDen, VtxReal.data(), PolyReal.data(), ListReal.data(), NumReal, matched,
                    Cur, Prev, MaxPolygons, MaxVertices);
                ExtendToEdges(ListReal.data(), NumReal, View, false);
            }
        }
        // Waits for the in-between frame's pairs (0), also its lists (1), or everything (2).
        static void Join(int which)
        {
            if (!Thread.joinable()) return;
            SsrProf::Scope prof(SsrProf::HelperWait); // (only the emulation thread joins)
            std::unique_lock<std::mutex> lock(Mutex);
            Done.wait(lock, [which] {
                for (int i = 0; i <= which; i++)
                    if (Jobs[i].Busy) return false;
                return true;
            });
        }
        static void Start(GPU3D& g, bool interpolate, s64 xnum, s64 xden, float view)
        {
            Join(2);
            WideGpuSendJoin(1); // (still sending a picture from the lists about to be rebuilt)
            if (!Thread.joinable())
            {
                for (Job& j : Jobs) j.Engine = std::make_unique<GPU3D>(g.GetNDS(), std::make_unique<NoRenderer>());
                Quit = false;
                Thread = std::thread(Loop);
            }
            Cur = g.RawCurrent;
            Prev = g.RawPrevious;
            XNum = xnum;
            XDen = xden;
            View = view;
            NumInterp = 0;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Paired = interpolate && !Prev.empty();
                for (int i = 0; i < 3; i++)
                {
                    Job& j = Jobs[i];
                    // (What SubmitPolygon reads besides the polygon itself.)
                    j.Engine->FlushAttributes = g.FlushAttributes;
                    j.Engine->ZeroDotWLimit = g.ZeroDotWLimit;
                    j.Interpolate = i < 2;
                    j.Busy = i == 2 || Paired;
                    j.Started = false;
                }
            }
            Wake.notify_all();
        }
        static void Stop()
        {
            if (!Thread.joinable()) return;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Quit = true;
            }
            Wake.notify_all();
            Thread.join();
            for (Job& j : Jobs)
            {
                j.Engine.reset();
                j.Busy = j.Started = false;
            }
            Paired = false;
        }
    };

    // The DS-resolution in-between frame, from the pairs the helper thread worked out for this
    // frame (false if there are none: no frame before it).
    static bool ReplayPaired(GPU3D& g, Vertex* vram, Polygon* pram, Polygon** list, u32& numPolys, u32& matched,
        size_t& frameSize)
    {
        ListHelper::Join(0);
        if (!ListHelper::Paired) return false;
        Replay(g, true, 1, 1, vram, pram, list, numPolys, matched, ListHelper::Cur, ListHelper::Prev, 2048, 6144,
            &ListHelper::Shared);
        frameSize = ListHelper::Cur.size();
        return true;
    }

    // Where the wide lists are built: on the helper thread on the Switch (SSR_WIDE_LISTS=0
    // keeps them with the emulation), elsewhere only with SSR_WIDE_LISTS=1; never for the
    // OpenGL 3D renderer, which is handed the lists themselves rather than a copy.
    static bool ListsOnHelper()
    {
        static const int env = [] {
            const char* v = getenv("SSR_WIDE_LISTS");
            return v ? (v[0] == '1' ? 1 : 0) : -1;
        }();
#ifdef __SWITCH__
        const bool on = env != 0;
#else
        const bool on = env == 1;
#endif
        if (!on) return false;
#ifdef OGLRENDERER_ENABLED
        if (GLThreadRenderer::Current) return false;
#endif
        return true;
    }

    // A frame has been flushed: builds the wide polygon lists for it, with the in-between
    // frame's if `interpolate`.
    static void OnFlush(GPU3D& g, bool interpolate)
    {
        if (VtxReal.empty())
        {
            VtxReal.resize(MaxVertices);
            VtxInterp.resize(MaxVertices);
            PolyReal.resize(MaxPolygons);
            PolyInterp.resize(MaxPolygons);
            ListReal.resize(MaxPolygons);
            ListInterp.resize(MaxPolygons);
        }
        if (getenv("SSR_WIDE_COUNT"))
        {
            // Development aid: how close the frame came to the DS's polygon and vertex memory.
            static u32 maxP = 0, maxV = 0;
            if (g.NumPolygons > maxP + 50 || g.NumVertices > maxV + 150)
            {
                maxP = std::max(maxP, g.NumPolygons);
                maxV = std::max(maxV, g.NumVertices);
                fprintf(stderr, "[wide] DS lists: %u of 2048 polygons, %u of 6144 vertices (raw %zu)\n", g.NumPolygons, g.NumVertices, g.RawCurrent.size());
            }
        }
        float view = Wide.ViewWidth < 256 ? 256 : Wide.ViewWidth;
        const s64 xden = 4096, xnum = (s64)(4096.0f * 256.0f / view + 0.5f);
        Dirty = true;
        if (ListsOnHelper())
        {
            ListHelper::Start(g, interpolate, xnum, xden, view);
            return;
        }
        ListHelper::Stop();
        u32 matched;
        Replay(g, false, xnum, xden, VtxReal.data(), PolyReal.data(), ListReal.data(), NumReal, matched, g.RawCurrent,
            g.RawPrevious, MaxPolygons, MaxVertices);
        ExtendToEdges(ListReal.data(), NumReal, view, false);
        NumInterp = 0;
        if (interpolate)
        {
            Replay(g, true, xnum, xden, VtxInterp.data(), PolyInterp.data(), ListInterp.data(), NumInterp, matched,
                g.RawCurrent, g.RawPrevious, MaxPolygons, MaxVertices);
            ExtendToEdges(ListInterp.data(), NumInterp, view, true);
        }
    }

    // A layer of the distance: as far as the furthest thing in the picture, within a tenth (a
    // sky's panels, its mountains, a far line of trees), or a haze: see-through, at least half
    // as far, its picture spread thin (no more than a texel for every two DS pixels, across
    // and down). At Aspertia's lookout the haze in front of the sky is a dome of 16 such panels,
    // from a little over half as far as the sky to four fifths, whose last column on the right
    // ended 110 pixels of 1280 short, so the sky and mountains beyond were not hazed over: a
    // darker block in the corner, and the mountains continued there in their own colours.
    // (Spread thin: the leaves of Castelia City's trees are see-through and as far, and came
    // apart in slivers continued.)
    // (A far one is also about as far at every corner, within a sixth: a panel of the sky or a
    // line of mountains faces the camera. Looking down from above, as on the stairs to
    // Aspertia's lookout, everything is about as far as the furthest thing, and the fence round a
    // planter there, running from near to far, counted as a layer of the distance: continued
    // to the view's edge, it ran on down it as a long rail.)
    static bool IsLayer(const Polygon* poly, s32 w0, s32 w1, s32 farW)
    {
        if ((s64)w1 * 10 >= (s64)farW * 9) return (s64)w0 * 6 >= (s64)w1 * 5;
        if (!poly->Translucent || (s64)w1 * 2 < farW) return false;
        if (!((poly->TexParam >> 26) & 7)) return true;
        s32 x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN, s0 = INT32_MAX, s1 = INT32_MIN, t0 = INT32_MAX, t1 = INT32_MIN;
        for (u32 v = 0; v < poly->NumVertices; v++)
        {
            const Vertex* p = poly->Vertices[v];
            x0 = std::min(x0, p->HiresPosition[0]); x1 = std::max(x1, p->HiresPosition[0]);
            y0 = std::min(y0, p->HiresPosition[1]); y1 = std::max(y1, p->HiresPosition[1]);
            s0 = std::min(s0, (s32)p->TexCoords[0]); s1 = std::max(s1, (s32)p->TexCoords[0]);
            t0 = std::min(t0, (s32)p->TexCoords[1]); t1 = std::max(t1, (s32)p->TexCoords[1]);
        }
        // (HiresPosition is in sixteenths of a DS pixel, as texture coordinates are of a texel.)
        return (s64)(s1 - s0) * 2 <= (s64)(x1 - x0) && (s64)(t1 - t0) * 2 <= (s64)(y1 - y0);
    }

    // The game leaves out what its own screen cannot show, so the wide view can come to the end
    // of a polygon a little short of its edge where the game drew nothing further: a sky made
    // of panels around the camera (Castelia zoomed out by the Pokemon Center: the last panel on
    // each side ended 20-40 pixels of 1280 short, the rear plane's flat colour beyond), or the
    // layers of a view from a lookout (sky, mountains, haze and a far line of trees, each ending
    // 30-90 pixels short). A polygon at least a tenth of the view wide that reaches past the DS
    // screen's edge, ends within a twelfth of the view's edge, and has nothing else beside it
    // there (no other polygon's extent reaches into the strip between its end and the edge, over
    // more than a third of its rows) is continued to that edge: its outermost corners moved out
    // along its top and bottom edges, its texture, colours and depth carried on with them, as if
    // the game had drawn it wider. (Only where nothing else is: continuing a polygon that has
    // neighbours there, a shadow on the street, a floor, changed it for nothing. Others ending in
    // the same strip, a sky's other layers, do not count as neighbours, nor does a see-through
    // layer that reaches the edge itself: the haze over a sky, which the sky behind should fill.)
    // A polygon whose picture has see-through parts: a texture with a transparent colour 0 or
    // alpha (A3I5, A5I3), or a translucent polygon.
    static bool SeeThrough(const Polygon* poly)
    {
        const u32 fmt = (poly->TexParam >> 26) & 7;
        return poly->Translucent || fmt == 1 || fmt == 6 || (fmt >= 2 && fmt <= 4 && (poly->TexParam & (1 << 29)));
    }

    // What was decided for each polygon reaching past the DS screen's edge in the last real
    // frame: continued (shown) or not, and for how many frames in a row since the rules have
    // said otherwise (against). The rules below each have a threshold (something beside it over
    // a third of its rows, a twelfth of the view from its edge, thin, continued too far), and as
    // the camera moves a polygon can sit on one, continued in one frame and not the next: on
    // the stairs to Aspertia's lookout the ground, a tree and a wall at the view's right edge
    // flickered in and out as James walked up them, each by a different rule. So a polygon's
    // decision only changes once the rules have said otherwise for HoldFrames real frames in a
    // row, and an in-between frame follows the last real frame's.
    struct Held { u32 tex; u8 n, side; bool shown; u8 against; s32 x0, x1, y0, y1; };
    static inline std::vector<Held> HeldPrev, HeldNow;
    static constexpr u8 HoldFrames = 4;
    static bool Continued(const char* result) { return result[0] == 'c' || result[0] == 's'; }

    static void ExtendToEdges(Polygon** list, u32 count, float view, bool interp)
    {
        static const bool off = getenv("SSR_WIDE_NO_EXTEND") != nullptr; // (to compare)
        // Only the layers of the distance are continued, not what is nearer: on the stairs to
        // Aspertia's lookout the ground, a tree and a wall at the view's right edge, continued in
        // some frames and not others as the camera moved, flickered (James, on his PC; what the
        // game draws beyond them, Black 2's wider map, frontend/bw2map.h, is better than a
        // stretched cliff in any case). (To compare: SSR_WIDE_EXTEND_ONLY=all continues both, as
        // before, =near only the nearer ones. SSR_WIDE_EXTEND_TRACE=1 writes each frame's
        // decisions, one line a polygon, to find one continued in some frames and not others.)
        static const char only = getenv("SSR_WIDE_EXTEND_ONLY") ? getenv("SSR_WIDE_EXTEND_ONLY")[0] : 'f';
        static const bool trace = getenv("SSR_WIDE_EXTEND_TRACE") != nullptr;
        static u32 traceCall = 0;
        if (trace) traceCall++;
        static const bool log = getenv("SSR_WIDE_EXTEND_LOG") != nullptr; // (development aid)
        static const bool hold = !getenv("SSR_WIDE_EXTEND_NO_HOLD"); // (to compare)
        if (off || !Wide.ExtendEdges) { HeldPrev.clear(); return; }
        if (!interp) HeldNow.clear();
        static thread_local std::vector<u8> heldTaken;
        heldTaken.assign(HeldPrev.size(), 0);
        // Whether polygon i is shown continued on `side`, the rules wanting it or not (`want`).
        auto decide = [&](u32 i, int side, bool want, s32 x0, s32 x1, s32 y0, s32 y1) {
            const Polygon* poly = list[i];
            s32 best = -1, bestD = 256 * 16 / 8;
            for (u32 k = 0; k < HeldPrev.size(); k++)
            {
                const Held& h = HeldPrev[k];
                if (heldTaken[k] || h.tex != poly->TexParam || h.n != poly->NumVertices || h.side != side) continue;
                const s32 d = std::abs(h.x0 - x0) + std::abs(h.x1 - x1) + std::abs(h.y0 - y0) + std::abs(h.y1 - y1);
                if (d < bestD) { bestD = d; best = (s32)k; }
            }
            bool shown = want;
            u8 against = 0;
            if (best >= 0)
            {
                heldTaken[best] = 1;
                const Held& h = HeldPrev[best];
                shown = h.shown;
                if (!interp && want != shown && (against = h.against + 1) >= HoldFrames) { shown = want; against = 0; }
            }
            if (!interp) HeldNow.push_back({ poly->TexParam, (u8)poly->NumVertices, (u8)side, shown, against, x0, x1, y0, y1 });
            return shown;
        };
        constexpr s32 full = 256 * 16;      // the view's width in HiresPosition's units
        const s32 dsLeft = (s32)(full / 2 - (full / 2) * 256.0f / view), dsRight = full - dsLeft;
        const s32 gap = full / 12;
        // side: 1 ends in the left strip, 2 the right; w0, w1: its nearest and furthest W (clip
        // space, comparable between polygons, unlike the W normalised for each polygon)
        // tl, bl, tr, br: its top and bottom corners' x, the outermost on each side
        struct Box { s32 x0, x1, y0, y1, w0, w1; u8 side; s32 tl, bl, tr, br; };
        static thread_local std::vector<Box> boxes;
        boxes.assign(count, Box { 0, -1, 0, -1, 0, 0, 0, 0, 0, 0, 0 });
        for (u32 i = 0; i < count; i++)
        {
            const Polygon* poly = list[i];
            if (!poly || poly->Degenerate || poly->Type != 0 || poly->NumVertices < 3) continue;
            Box& b = boxes[i];
            b.x0 = b.y0 = b.w0 = INT32_MAX;
            b.x1 = b.y1 = b.w1 = INT32_MIN;
            for (u32 v = 0; v < poly->NumVertices; v++)
            {
                const s32* p = poly->Vertices[v]->HiresPosition;
                b.x0 = std::min(b.x0, p[0]); b.x1 = std::max(b.x1, p[0]);
                b.y0 = std::min(b.y0, p[1]); b.y1 = std::max(b.y1, p[1]);
                const s32 w = poly->Vertices[v]->Position[3];
                b.w0 = std::min(b.w0, w); b.w1 = std::max(b.w1, w);
            }
            b.tl = b.bl = INT32_MAX;
            b.tr = b.br = INT32_MIN;
            for (u32 v = 0; v < poly->NumVertices; v++)
            {
                const s32* p = poly->Vertices[v]->HiresPosition;
                if (p[1] == b.y0) { b.tl = std::min(b.tl, p[0]); b.tr = std::max(b.tr, p[0]); }
                if (p[1] == b.y1) { b.bl = std::min(b.bl, p[0]); b.br = std::max(b.br, p[0]); }
            }
        }
        // The furthest W in the picture: a sky's layers are about as far as anything.
        s32 farW = 0;
        for (const Box& b : boxes)
            if (b.x1 >= b.x0) farW = std::max(farW, b.w1);
        bool any = false;
        for (u32 i = 0; i < count; i++)
        {
            Box& b = boxes[i];
            if (b.x1 < b.x0 || b.x1 - b.x0 < full / 10) continue;
            // A layer of the backdrop (as far as the furthest thing, within a tenth: the sky, its
            // mountains, haze and far line of trees) is continued from wherever past the DS
            // screen's edge it ends: the game cuts those off at its own screen's edge plus a
            // margin that varies with the camera. At Aspertia's lookout the mountains ended 125
            // pixels of 1280 short (more than a twelfth), so they and the sky's plain colour
            // behind them stopped in a hard edge while the haze and clouds in front, ending
            // nearer the edge, were continued over nothing: a darker block in the corner.
            const bool backdrop = IsLayer(list[i], b.w0, b.w1, farW);
            if ((only == 'f' && !backdrop) || (only == 'n' && backdrop)) continue;
            const s32 reach = backdrop ? full / 2 : gap;
            // (A layer whose end is slanted, reaching the view's edge at its top and ending short of
            // it at its bottom, or the other way round, counts as ending at its shorter corner:
            // at Aspertia's lookout the sky's left panel came down from the top left corner to a
            // tenth of the way across at the horizon, its next panel to the left left out, so the
            // rear plane's paler colour showed in a wedge.)
            const bool slanted = backdrop && !list[i]->Translucent && b.y0 <= 0 && (b.x0 <= 0) != (b.x1 >= full);
            const s32 x0 = b.x0 > 0 || !slanted ? b.x0 : std::max(b.tl, b.bl);
            const s32 x1 = b.x1 < full || !slanted ? b.x1 : std::min(b.tr, b.br);
            if (x0 > 0 && x0 <= reach && x0 < dsLeft) b.side |= 1;
            if (x1 < full && x1 >= full - reach && x1 > dsRight) b.side |= 2;
            any |= b.side != 0;
        }
        if (log)
            for (u32 i = 0; i < count; i++)
            {
                const Box& b = boxes[i];
                if (b.x1 < b.x0 || !IsLayer(list[i], b.w0, b.w1, farW)) continue;
                fprintf(stderr, "[extend] backdrop %u: x %d-%d y %d-%d w %d-%d side %d translucent %d alpha %u tex %08x verts %u\n", i,
                    b.x0, b.x1, b.y0, b.y1, b.w0, b.w1, b.side, list[i]->Translucent ? 1 : 0, (list[i]->Attr >> 16) & 31,
                    list[i]->TexParam, list[i]->NumVertices);
            }
        for (u32 i = 0; i < count && (any || hold); i++)
        {
            const Box b = boxes[i];
            if (!b.side && !hold) continue;
            if (b.x1 < b.x0) continue;
            const bool layer = IsLayer(list[i], b.w0, b.w1, farW);
            const bool farBox = (s64)b.w1 * 10 >= (s64)farW * 9, distant = (s64)b.w1 * 10 >= (s64)farW * 6;
            // (A layer of the backdrop is not held back by anything small beside it, less than a
            // tenth of the view wide, or anything with see-through parts (a picture with a
            // transparent colour or alpha), that shares none of its corners: something standing
            // in front of the distance, a far tree on the horizon, which the depth keeps in front
            // of what is continued. At Aspertia's lookout the trees on the hill to the east held
            // back the mountains, which then ended in a hard edge; those trees are about a tenth
            // of the view wide, so as the camera rose to the view they held the sky back in some
            // frames and not others, a flicker. Anything solid and larger still does: the wall
            // beside the back wall of Route 19's gate.)
            auto sharesCorner = [&](u32 j) {
                const Polygon* p = list[i];
                const Polygon* q = list[j];
                for (u32 u = 0; u < p->NumVertices; u++)
                    for (u32 v = 0; v < q->NumVertices; v++)
                    {
                        if (p->Vertices[u] == q->Vertices[v]) return true;
                        // (The same place counts too, the game having made separate corners
                        // for its neighbours, but not on the view's edges, where the clipping
                        // makes corners of everything that crosses them: the clouds at the top
                        // left corner of the lookout's view, "sharing" one with the sky, held
                        // it back in one frame in two, a flicker.)
                        const s32* a = p->Vertices[u]->HiresPosition;
                        const s32* c = q->Vertices[v]->HiresPosition;
                        if (a[0] == c[0] && a[1] == c[1] && a[0] > 0 && a[0] < full && a[1] > 0 && a[1] < 192 * 16) return true;
                    }
                return false;
            };
            for (int side = 1; side <= 2; side++)
            {
                const s32 nearX = (b.x0 + b.x1) / 2, limit = side == 1 ? dsLeft : dsRight, edge = side == 1 ? 0 : full;
                if (!(b.side & side))
                {
                    // (Not one the rules would continue now: past the DS screen's edge and short
                    // of the view's, it may have been continued in the last frames.)
                    const bool strip = side == 1 ? b.x0 > 0 && b.x0 < dsLeft : b.x1 < full && b.x1 > dsRight;
                    if (hold && strip && decide(i, side, false, b.x0, b.x1, b.y0, b.y1))
                    {
                        const char* r = Continue(list[i], side, nearX, limit, edge, layer, 2);
                        if (trace)
                            fprintf(stderr, "[xt] %u %c %c %c tex %08x n %u x %d-%d y %d-%d w %d-%d far %d held-out-%s %d\n", traceCall, interp ? 'i' : 'r',
                                side == 1 ? 'L' : 'R', layer ? 'F' : 'N', list[i]->TexParam, list[i]->NumVertices, b.x0, b.x1, b.y0, b.y1,
                                b.w0, b.w1, farW, r, i);
                    }
                    continue;
                }
                // The strip between the polygon's end and the view's edge, over its rows.
                const s32 sx0 = side == 1 ? 0 : b.x1 < full ? b.x1 : std::min(b.tr, b.br),
                          sx1 = side == 1 ? (b.x0 > 0 ? b.x0 : std::max(b.tl, b.bl)) : full;
                s32 blocker = -1;
                for (u32 j = 0; j < count && blocker < 0; j++)
                {
                    const Box& o = boxes[j];
                    if (j == i || o.x1 < o.x0 || (o.side & side)) continue;
                    // (A neighbour sharing a corner always holds it back, even a see-through one: the
                    // haze at Aspertia's lookout is a dome of panels sharing their corners, whose
                    // column at the view's left edge would otherwise have been folded up by the
                    // next column in being continued over it.)
                    const bool shares = sharesCorner(j);
                    if (!shares && list[j]->Translucent && (side == 1 ? o.x0 <= 0 : o.x1 >= full)) continue;
                    if (farBox && !shares && (o.x1 - o.x0 < full / 10 || SeeThrough(list[j]))) continue;
                    // (Nor anything wholly nearer, by more than a twentieth: it stays in front of
                    // what is continued behind it; the trees before a far line of trees at Aspertia's
                    // lookout.)
                    if ((s64)o.w1 * 21 < (s64)b.w0 * 20) continue;
                    // (For a layer of the distance, a sky's mountains or haze: nor, if it hangs from
                    // the top of the view, what stands below its top, nor anything wholly further,
                    // which it covers, as it would on the DS: the land beside the lookout that
                    // Black 2 draws once it loads more of its map, frontend/bw2map.h.)
                    if (farBox && b.y0 <= 0 && o.y0 > 0) continue;
                    if ((distant || layer) && o.w0 > b.w1) continue;

                    // (Touching counts: a neighbour sharing the corner to be moved would be bent.
                    // One beside only a third of its rows or less does not: the sea below the
                    // sky's haze at the horizon.)
                    const s32 rows = std::min(o.y1, b.y1) - std::max(o.y0, b.y0);
                    if (o.x0 <= sx1 && o.x1 >= sx0 && rows > 0 && rows * 3 > b.y1 - b.y0) blocker = (s32)j;
                }
                if (log)
                    fprintf(stderr, "[extend] polygon %u (x %d-%d, y %d-%d of %d, w %d-%d of %d) %s on the %s\n", i, b.x0, b.x1, b.y0, b.y1, full, b.w0, b.w1, farW,
                        blocker < 0 ? "continued" : "left (something beside it)", side == 1 ? "left" : "right");
                if (log && blocker >= 0)
                    fprintf(stderr, "[extend]   beside it: polygon %d (x %d-%d, y %d-%d, w %d-%d)\n", blocker, boxes[blocker].x0, boxes[blocker].x1,
                        boxes[blocker].y0, boxes[blocker].y1, boxes[blocker].w0, boxes[blocker].w1);
                const char* result = blocker >= 0 ? "blocked" : Continue(list[i], side, nearX, limit, edge, layer, hold ? 0 : 1);
                if (hold)
                {
                    const bool want = Continued(result);
                    if (decide(i, side, want, b.x0, b.x1, b.y0, b.y1))
                    {
                        const char* r = Continue(list[i], side, nearX, limit, edge, layer, want ? 1 : 2);
                        if (!want) result = r[0] == 'c' ? "held-carried" : "held-stretched";
                    }
                    else if (want)
                        result = "held-back";
                }
                if (trace)
                    fprintf(stderr, "[xt] %u %c %c %c tex %08x n %u x %d-%d y %d-%d w %d-%d far %d %s%s%d\n", traceCall, interp ? 'i' : 'r',
                        side == 1 ? 'L' : 'R', layer ? 'F' : 'N', list[i]->TexParam, list[i]->NumVertices, b.x0, b.x1, b.y0, b.y1,
                        b.w0, b.w1, farW, result, blocker >= 0 ? " by " : " ", blocker >= 0 ? blocker : (s32)i);
            }
        }
        if (!interp) HeldPrev.swap(HeldNow);
    }

    // Moves a polygon's corners at one end (in its outer half, `nearX` being its middle, and
    // beyond `limit`, the DS screen's edge: a slanted end's both corners) out to x = `edge`,
    // each along the edge of the polygon running inwards from it, with what is drawn carried
    // on: depth and 1/W (which change linearly across the screen) and the texture coordinates
    // and colours (linearly once divided by W, as the renderers interpolate them). Where that
    // cannot be done, the corners are only moved, stretching the polygon: a corner without such
    // an edge (its neighbours as far out as it is, or the edge so short it would be continued
    // many times over), a texture that does not repeat and would be continued past its end
    // (its last column drawn over and over, a band of one colour: the clouds by the pier), or
    // one that ends exactly at the corner (below).
    // mode 0: only says what would be done; 1: does it; 2: does it whatever the checks below say,
    // the polygon having been continued in the last frames (HeldPrev).
    static const char* Continue(Polygon* poly, int side, s32 nearX, s32 limit, s32 edge, bool layer, int mode)
    {
        const u32 n = poly->NumVertices;
        // A layer of the distance whose end the DS screen shows part of really ends there, the
        // game having cut it off nowhere: one of its end's corners past the screen's edge and the
        // next, also in the outer half, on the screen by more than two DS pixels (a corner a few
        // sixteenths inside the edge is where the DS's own clipping cut it: the fence running
        // down beside the street below the lookout). Moving the first alone would make a wedge
        // (the far hillside at Aspertia's lookout, its end running from inside the DS screen's
        // right edge to past it: grass drawn over the cliff in a thin wedge to the view's edge,
        // and only in the frames where the hillside counted as a layer, a flicker). So for a
        // layer the corners of its end within a twelfth of the view inside the DS screen's edge
        // are moved out too, the whole end: the game left out the layer's next panel, which
        // would have filled that corner of the DS screen as well. (Left where they were, the
        // white mountain at the lookout, its end's lower corner a little inside the edge,
        // stopped in a slanted cut near the view's right edge as the camera moved, the sky
        // beyond.) A corner further inside means the layer really ends there.
        const s32 inside = layer ? 256 * 16 / 12 : 0;
        auto isOut = [&](s32 x) {
            return side == 1 ? x <= nearX && x < limit + inside : x >= nearX && x > limit - inside;
        };
        auto beyond = [&](u32 v) {
            const s32 x = poly->Vertices[v]->HiresPosition[0];
            return side == 1 ? x <= nearX && x < limit : x >= nearX && x > limit;
        };
        auto deepInside = [&](u32 v) {
            const s32 x = poly->Vertices[v]->HiresPosition[0];
            return side == 1 ? x <= nearX && x >= limit + inside : x >= nearX && x <= limit - inside;
        };
        for (u32 v = 0; v < n && layer; v++)
        {
            const u32 w = (v + 1) % n;
            if ((beyond(v) && deepInside(w)) || (deepInside(v) && beyond(w))) if (mode != 2) return "ends-on-screen";
        }
        struct Corner { s32 x, y, z; float invW, s, t, r, g, b; s32 rs, rt; };
        Corner c[10];
        for (u32 v = 0; v < n; v++)
        {
            const Vertex* p = poly->Vertices[v];
            const float invW = poly->FinalW[v] > 0 ? 1.0f / (float)poly->FinalW[v] : 1.0f;
            c[v] = { p->HiresPosition[0], p->HiresPosition[1], poly->FinalZ[v], invW,
                p->TexCoords[0] * invW, p->TexCoords[1] * invW,
                p->FinalColor[0] * invW, p->FinalColor[1] * invW, p->FinalColor[2] * invW,
                p->TexCoords[0], p->TexCoords[1] };
        }
        // The new corners, worked out first: all of them are continued, or none.
        struct Moved { bool out; s32 y, z, w; s16 s, t; s32 r, g, b; };
        Moved m[10];
        const u32 tp = poly->TexParam;
        const bool textured = (tp >> 26) & 7;
        const float sMax = (float)((8 << ((tp >> 20) & 7)) * 16), tMax = (float)((8 << ((tp >> 23) & 7)) * 16);
        static const bool stretchOnly = getenv("SSR_WIDE_EXTEND_STRETCH") != nullptr; // (to compare)
        bool carry = !stretchOnly, steep = false;
        for (u32 v = 0; v < n && carry; v++)
        {
            const Corner& o = c[v];
            m[v].out = isOut(o.x);
            if (!m[v].out) continue;
            // The neighbour furthest inwards: the other end of the polygon's top or bottom edge.
            const Corner& a = c[(v + n - 1) % n];
            const Corner& b = c[(v + 1) % n];
            const Corner& in = side == 1 ? (a.x > b.x ? a : b) : (a.x < b.x ? a : b);
            const s32 dx = o.x - in.x;
            const float t = dx != 0 ? (float)(edge - in.x) / (float)dx : 0.0f;
            auto at = [&](float from, float to) { return from + (to - from) * t; };
            const float invW = at(in.invW, o.invW);
            if ((side == 1 ? dx > -16 : dx < 16) || t < 1.0f || t > 4.0f || invW <= 0)
            {
                carry = false;
                break;
            }
            // (Along an edge steeper than it is long across, a corner goes further down or up the
            // view than out to its edge: that is the side of something running down the view, not
            // the top or bottom edge of something cut off at the side. Such a polygon is held to the
            // limit for anything near, below, even if it counts as a layer of the distance: from
            // above, the fence round a planter by the stairs to Aspertia's lookout did, and its
            // thin side ran on down across the street to the view's corner as one long rail. The
            // street's paving beside it, as steep but wide, is still continued.)
            if (std::abs(o.y - in.y) > std::abs(dx)) steep = true;
            // A texture that ends exactly at the corner, at a whole number of its repeats (Castelia's
            // sky: each panel one repeat of its own picture), goes on in the panel beside it,
            // which the game left out and which may show another picture: carried on, it starts
            // again with a seam where the polygon ended. Stretched, it has none. (Unless the
            // polygon itself shows the picture over more than once: then it repeats without a
            // seam, and stretched it would change shape as the camera moved, the mountains on
            // the left of the view from Aspertia's lookout.)
            if (textured)
            {
                const bool alongS = std::abs(o.rs - in.rs) >= std::abs(o.rt - in.rt);
                const s32 size = alongS ? (s32)sMax : (s32)tMax, at0 = alongS ? o.rs : o.rt;
                const s32 phase = ((at0 % size) + size) % size;
                s32 lo = INT32_MAX, hi = INT32_MIN;
                for (u32 u = 0; u < n; u++)
                {
                    lo = std::min(lo, alongS ? c[u].rs : c[u].rt);
                    hi = std::max(hi, alongS ? c[u].rs : c[u].rt);
                }
                const bool repeats = (tp & (1 << (alongS ? 16 : 17))) && hi - lo > size + 16;
                if (!repeats && (phase < 16 || phase > size - 16))
                {
                    carry = false;
                    break;
                }
            }
            const float s = at(in.s, o.s) / invW, tt = at(in.t, o.t) / invW;
            // (Half a texel of slack for rounding. Only past where the corner already was: a
            // texture that the polygon itself already shows past its end, its edge row drawn on,
            // goes on as it was, the haze at Aspertia's lookout fading out in its first row.)
            auto past = [](float to, float from, float end) {
                return (to < -8 && to < from - 8) || (to > end + 8 && to > from + 8);
            };
            if (textured && ((!(tp & (1 << 16)) && past(s, (float)o.rs, sMax)) || (!(tp & (1 << 17)) && past(tt, (float)o.rt, tMax))))
            {
                carry = false;
                break;
            }
            m[v].y = (s32)std::lround(at((float)in.y, (float)o.y));
            m[v].z = (s32)std::clamp(at((float)in.z, (float)o.z), 0.0f, 16777215.0f);
            m[v].w = (s32)std::clamp(1.0f / invW, 1.0f, 65535.0f);
            m[v].s = (s16)std::clamp(s, -32768.0f, 32767.0f);
            m[v].t = (s16)std::clamp(tt, -32768.0f, 32767.0f);
            m[v].r = (s32)std::clamp(at(in.r, o.r) / invW, 0.0f, 511.0f);
            m[v].g = (s32)std::clamp(at(in.g, o.g) / invW, 0.0f, 511.0f);
            m[v].b = (s32)std::clamp(at(in.b, o.b) / invW, 0.0f, 511.0f);
        }
        // Stretched by more than a fifth of its width, a polygon shows it: a tree or a house at the
        // end of the view grown wide (SoulSilver and HeartGold, Route 29 by New Bark Town); and
        // carried on by more than half of it, a piece of a model turns into long slivers (a tree
        // in Castelia City's plaza, out of the game's own view). Only the layers that end a
        // little short (a sky's panels, the haze from a lookout) are continued; anything else
        // stays as the game drew it.
        // And a thin strip, a rail or a curb seen from above, is not continued at all when it
        // runs down the view (its end, moved out along its sides, drew a rail on down the view),
        // nor across it when it is nearer than the distance (the bar along its foot, without
        // its sides, drew a rail across the street): the fence round the planter by the stairs to
        // Aspertia's lookout, walking up them. Its thickness: twice its area over its longest
        // stretch between two corners; thin: under a twelfth of the view running down it (the
        // paving beside that planter is about an eighth), a twenty-fourth across it.
        {
            double area2 = 0, longest = 0;
            for (u32 v = 0; v < n; v++)
            {
                const Corner& a = c[v];
                const Corner& b = c[(v + 1) % n];
                area2 += (double)a.x * b.y - (double)b.x * a.y;
                for (u32 u = v + 1; u < n; u++)
                    longest = std::max(longest, std::hypot((double)(c[u].x - a.x), (double)(c[u].y - a.y)));
            }
            const double thick = longest > 0 ? std::abs(area2) / longest : 0;
            if (mode != 2 && steep && thick < 256 * 16 / 12.0) return "thin-steep";
            if (mode != 2 && !layer && thick < 256 * 16 / 24.0) return "thin";
        }
        {
            s32 x0 = INT32_MAX, x1 = INT32_MIN;
            for (u32 v = 0; v < n; v++)
            {
                x0 = std::min(x0, c[v].x);
                x1 = std::max(x1, c[v].x);
            }
            const s32 added = side == 1 ? x0 - edge : edge - x1;
            static const bool logC = getenv("SSR_WIDE_EXTEND_LOG") != nullptr;
            if (logC)
            {
                fprintf(stderr, "[extend]   corners:");
                for (u32 v = 0; v < n; v++) fprintf(stderr, " (%d,%d st %d,%d)", c[v].x, c[v].y, c[v].rs, c[v].rt);
                fprintf(stderr, " carry %d added %d width %d%s\n", carry ? 1 : 0, added, x1 - x0,
                    (s64)added * (carry ? (layer && !steep ? 1 : 2) : 5) > (s64)(x1 - x0) ? " TOO FAR: left" : "");
            }
            if (mode != 2 && (s64)added * (carry ? (layer && !steep ? 1 : 2) : 5) > (s64)(x1 - x0)) return "too-far";
        }
        if (mode == 0) return carry ? (steep ? "carried-steep" : "carried") : "stretched";
        for (u32 v = 0; v < n; v++)
        {
            const bool out = isOut(c[v].x);
            if (!out) continue;
            Vertex* p = poly->Vertices[v];
            p->HiresPosition[0] = edge;
            if (!carry) continue;
            p->HiresPosition[1] = m[v].y;
            poly->FinalZ[v] = m[v].z;
            poly->FinalW[v] = m[v].w;
            p->TexCoords[0] = m[v].s;
            p->TexCoords[1] = m[v].t;
            p->FinalColor[0] = m[v].r;
            p->FinalColor[1] = m[v].g;
            p->FinalColor[2] = m[v].b;
        }
        return carry ? (steep ? "carried-steep" : "carried") : "stretched";
    }

    // Called where the 3D renderer draws a frame. The image drawn here belongs to the
    // display frame after this one, like the 3D renderer's own, so it is published one
    // call later.
    static void OnRender(GPU& gpu, bool showingInterp)
    {
        // A picture the graphics chip has not finished, in a frame with nothing new to draw (a
        // game that draws its 3D every other frame, as Black 2 in its cities): rather than the
        // emulation waiting for it, the last picture stays on show for one more frame, and this
        // one is shown at the next call, by when it has had two frames. (Where the graphics chip
        // keeps up nothing changes. SSR_WIDE_NO_LATE=1 always waits.)
        static const bool noLate = getenv("SSR_WIDE_NO_LATE") != nullptr;
        if (!noLate && Drawing && DrawingOnWideGpu && WideGpuCurrent && !showingInterp && !Dirty && !NeedReal &&
            Wide.FrontW == Wide.Width && Wide.FrontH == Wide.Height &&
            (WideGpuSendOnThread() ? WideGpuSendBusy(1) || WideGpuSendBusy(2) : !WideGpuCurrent->Done(PendingSlot)))
        {
            if (++LateFrames % 300 == 0)
                fprintf(stderr, "[wide] %llu frames went on with the last picture while the graphics chip finished the next\n",
                    (unsigned long long)LateFrames);
            return;
        }
        Wait();
        if (PendingPublish)
        {
            Publish();
            PendingPublish = false;
        }
        // The lists built on the helper thread (ListHelper): the in-between frame's for it, or
        // both.
        ListHelper::Join(showingInterp ? 1 : 2);
        if (Wide.Width <= 0 || Wide.Height <= 0) return;
        if (showingInterp && NumInterp)
        {
            Draw(gpu, ListInterp.data(), NumInterp);
            NeedReal = true;
            Dirty = false;
            PendingPublish = true;
        }
        else if (Dirty || NeedReal || Wide.FrontW != Wide.Width || Wide.FrontH != Wide.Height)
        {
            Draw(gpu, ListReal.data(), NumReal);
            NeedReal = false;
            Dirty = false;
            PendingPublish = true;
        }
    }

    static void Draw(GPU& gpu, Polygon** list, u32 npolys)
    {
        const int W = Wide.Width, H = Wide.Height;
        const GPU3D& g = gpu.GPU3D;
#ifdef OGLRENDERER_ENABLED
        // With the 3D drawn by the graphics chip, so is this picture (same thread, same frame).
        if (GLThreadRenderer* gl = GLThreadRenderer::Current; gl && &g.GetCurrentRenderer() == gl)
        {
            gl->QueueWide(list, npolys, W, H, BackFor(W, H, false));
            Drawing = true;
            DrawingOnGL = true;
            return;
        }
#endif
        if (WideGpuCurrent)
        {
            // Worked out here, drawn by the graphics chip while the emulation goes on; Wait()
            // fetches the picture.
            // (On a thread of its own with deko3D, WideGpuSend: from a copy of the list, whose
            // polygons are not touched before Wait or the next frame's lists, ListHelper::Start,
            // which wait for it. The registers and texture memory it reads stay until the next
            // frame is flushed, GPU3D::VBlank, which waits for it too.)
            static WideGpuFrame frame;
            static std::vector<Polygon*> listCopy;
            static const bool prof = getenv("SSR_WIDE_PROF") != nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            BackFor(W, H, true);
            listCopy.assign(list, list + npolys);
            SentOk = false;
            const int slot = Next;
            WideGpuSend(1, [&gpu, W, H, slot] {
                WideGpuBuild(gpu, listCopy.data(), (u32)listCopy.size(), W, H, frame, WideGpuCurrent->TakesAtlas());
                SentOk = WideGpuCurrent->Submit(frame, slot);
                // (Waited for on the other sending thread: the emulation then only waits for that
                // job, in Wait.)
                if (SentOk && WideGpuSendOnThread()) WideGpuSend(2, [slot] { WideGpuCurrent->Fetch(slot); });
            });
            Drawing = DrawingOnWideGpu = WideGpuSendOnThread() || SentOk;
            PendingSlot = Next;
            if (prof)
            {
                WideGpuSendJoin(1);
                static double total = 0;
                static u64 frames = 0, vertices = 0, draws = 0;
                total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                vertices += frame.Vertices.size();
                draws += frame.Draws.size();
                if (++frames % 200 == 0)
                    fprintf(stderr, "[wide] %dx%d on the graphics chip (%s): %.2f ms to set up and send, %llu triangles, %llu draws\n",
                        W, H, WideGpuCurrent->Name(), total / frames, (unsigned long long)(vertices / 3 / frames),
                        (unsigned long long)(draws / frames));
            }
            return;
        }
        const SoftRenderer& soft = static_cast<const SoftRenderer&>(g.GetCurrentRenderer());

        BackFor(W, H, false);

        // Rear plane.
        const s32 clearZ = (s32)(((g.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF);
        const u32 clearAlpha = (g.RenderClearAttr1 >> 16) & 0x1F;
        u32 clearColor = 0;
        if (clearAlpha)
        {
            u32 r = (g.RenderClearAttr1 << 3) & 0xF8, gr = (g.RenderClearAttr1 >> 2) & 0xF8, b = (g.RenderClearAttr1 >> 7) & 0xF8;
            clearColor = 0xFF000000 | ((r | (r >> 5)) << 16) | ((gr | (gr >> 5)) << 8) | (b | (b >> 5));
        }
        const u8 clearAttr = (g.RenderClearAttr1 & (1 << 15)) ? 0x80 : 0;

        // Screen-space vertices of every polygon, as fans of triangles.
        struct Tri
        {
            u32 Attr, TexParam;
            bool WBuffer;
            WideVertex V[3];
            int Y0, Y1; // rows touched
            Texture* Tex;
            bool FrontFirst; // drawn out of order: on equal depth it wins if it came first
            u16 Order;       // its place among the opaque triangles, in the game's order
            float NearZ;
        };
        // Opaque polygons arrive in no particular order of depth (the hardware sorts them by
        // screen position). Drawn like that, a forest shades every pixel many times over, so
        // they are drawn near to far instead and the depth test rejects what is hidden
        // before any shading. To keep the outcome the same, on equal depth the one earlier in
        // the original order wins (Writer records who drew each pixel). Polygons that test
        // for equal depth (decals) need what is under them drawn first, and translucent ones
        // are blended in order, so those follow in their original order.
        static std::vector<Tri> tris, opaqueTris, laterTris;
        tris.clear();
        opaqueTris.clear();
        laterTris.clear();
        const float sx = W / (256.0f * 16), sy = H / (192.0f * 16);
        const bool texturing = g.RenderDispCnt & 1;
        size_t texturesUsed = 0;
        auto findTexture = [&](u32 param, u32 palette) -> Texture* {
            param &= ~0x000F0000u; // wrapping is applied when sampling
            for (size_t i = 0; i < texturesUsed; i++)
                if (Textures[i]->Param == param && Textures[i]->Palette == palette) return Textures[i].get();
            if (texturesUsed == Textures.size()) Textures.push_back(std::make_unique<Texture>());
            Texture* t = Textures[texturesUsed++].get();
            t->Param = param;
            t->Palette = palette;
            t->W = 8 << ((param >> 20) & 0x7);
            t->H = 8 << ((param >> 23) & 0x7);
            t->Texels.assign((size_t)t->W * t->H, 0);
            return t;
        };
        for (u32 p = 0; p < npolys; p++)
        {
            const Polygon* poly = list[p];
            if (!poly || poly->Degenerate || poly->IsShadowMask || poly->IsShadow || poly->Type != 0) continue;
            if (poly->NumVertices < 3) continue;
            Texture* tex = texturing && ((poly->TexParam >> 26) & 0x7) ? findTexture(poly->TexParam, poly->TexPalette) : nullptr;
            WideVertex v[10];
            for (u32 i = 0; i < poly->NumVertices; i++)
            {
                const Vertex* vtx = poly->Vertices[i];
                v[i].X = vtx->HiresPosition[0] * sx;
                v[i].Y = vtx->HiresPosition[1] * sy;
                v[i].InvW = poly->FinalW[i] > 0 ? 1.0f / poly->FinalW[i] : 1.0f;
                v[i].Z = (float)poly->FinalZ[i];
                v[i].R = (float)(vtx->FinalColor[0] >> 3);
                v[i].G = (float)(vtx->FinalColor[1] >> 3);
                v[i].B = (float)(vtx->FinalColor[2] >> 3);
                v[i].S = (float)vtx->TexCoords[0];
                v[i].T = (float)vtx->TexCoords[1];
            }
            for (u32 i = 1; i + 1 < poly->NumVertices; i++)
            {
                Tri t;
                t.Attr = poly->Attr;
                t.TexParam = poly->TexParam;
                t.WBuffer = poly->WBuffer;
                t.Tex = tex;
                t.V[0] = v[0];
                t.V[1] = v[i];
                t.V[2] = v[i + 1];
                float ymin = std::min({ t.V[0].Y, t.V[1].Y, t.V[2].Y }), ymax = std::max({ t.V[0].Y, t.V[1].Y, t.V[2].Y });
                // Spans are widened by a hair so that neighbouring polygons leave no gaps.
                t.Y0 = std::max(0, (int)std::ceil(ymin - 0.5f - 0.4f));
                t.Y1 = std::min(H - 1, (int)std::floor(ymax - 0.5f + 0.4f));
                if (t.Y1 < t.Y0) continue;
                t.FrontFirst = !poly->Translucent && !(poly->Attr & (1 << 14));
                t.NearZ = std::min({ t.V[0].Z, t.V[1].Z, t.V[2].Z });
                if (t.FrontFirst) t.Order = (u16)std::min<size_t>(opaqueTris.size(), 0xFFFE);
                (t.FrontFirst ? opaqueTris : laterTris).push_back(t);
            }
        }
        // Nearest first (by its nearest corner), so that the depth test rejects as much as
        // possible; on equal depth the one first in the game's order wins (Writer).
        std::stable_sort(opaqueTris.begin(), opaqueTris.end(), [](const Tri& a, const Tri& b) { return a.NearZ < b.NearZ; });
        tris.assign(opaqueTris.begin(), opaqueTris.end());
        tris.insert(tris.end(), laterTris.begin(), laterTris.end());

        // The drawing itself runs on other threads while emulation goes on, so everything it
        // reads is copied here first. (Texture memory stays as it is until the next frame is
        // rendered, and WideWait() is called before that.)
        static u16 toonCopy[32];
        static u8 fogDensity[34];
        memcpy(toonCopy, g.RenderToonTable, sizeof(toonCopy));
        memcpy(fogDensity, g.RenderFogDensityTable, sizeof(fogDensity));
        const u32 fogOffset = g.RenderFogOffset, fogShift = g.RenderFogShift;
        const GPU* const gpuP = &gpu;
        const SoftRenderer* const softP = &soft;
        const u32 dispCnt = g.RenderDispCnt;
        const bool blendEnabled = dispCnt & (1 << 3);
        const u32 alphaRef = g.RenderAlphaRef;
        const bool fogEnabled = dispCnt & (1 << 7);
        const bool fogColorOn = !(dispCnt & (1 << 6));
        const u32 fogR = (g.RenderFogColor << 3) & 0xF8, fogG = (g.RenderFogColor >> 2) & 0xF8, fogB = (g.RenderFogColor >> 7) & 0xF8;

        if (!Pool)
        {
            int n = (int)std::thread::hardware_concurrency() - 2;
#if defined(__x86_64__) || defined(__i386__)
            // x86 processors mostly run two threads per core. Drawing threads on the second
            // thread of a core slow down the emulation running on the first, and measured
            // (6 cores, 12 threads) 3-5 threads were quicker overall than 10.
            n = (int)std::thread::hardware_concurrency() / 2 - 1;
#endif
#ifdef __SWITCH__
            // Games get three of the Switch's four cores; the emulation keeps the first, so
            // the drawing threads share the other two with the DS-resolution render thread,
            // and a third takes core 0 while the emulation waits (SsrThreads.h).
            n = SsrWideCore0() ? 3 : 2;
#endif
            if (const char* forced = getenv("SSR_WIDE_THREADS")) n = atoi(forced);
            Pool = std::make_unique<Workers>(std::clamp(n, 1, 16));
        }

        // The picture is drawn in strips of rows that the threads take one after another, so
        // that a thread whose strip is quick (sky, a plain floor) goes on to help elsewhere.
        const int stripRows = 16, strips = (H + stripRows - 1) / stripRows;
        static std::atomic<int> nextStrip;
        nextStrip = 0;
        auto strip = [=](int index) {
            const int rowA = index * stripRows, rowB = std::min(H, rowA + stripRows);
            const size_t stripPixels = (size_t)(rowB - rowA) * W;
            static thread_local StripBuffers buffers;
            // (a group of pixels at the end of the last row reads a little past it)
            buffers.Depth.resize((size_t)stripRows * W + Lanes);
            buffers.Attr.resize((size_t)stripRows * W);
            buffers.Writer.resize((size_t)stripRows * W + Lanes);
            std::fill(BackPx + (size_t)rowA * W, BackPx + (size_t)rowB * W, clearColor);
            std::fill(buffers.Depth.begin(), buffers.Depth.begin() + stripPixels, clearZ);
            std::fill(buffers.Attr.begin(), buffers.Attr.begin() + stripPixels, clearAttr);
            std::fill(buffers.Writer.begin(), buffers.Writer.begin() + stripPixels, 0xFFFF);

            // One copy of the loop per kind of triangle (textured or not, Z- or W-buffered,
            // one colour or shaded), so that each pixel tests and keeps live only what its
            // kind needs.
            auto raster = [&](const Tri& t, auto texC, auto wbC, auto flatC) {
                    const WideVertex& a = t.V[0];
                    const WideVertex& b = t.V[1];
                    const WideVertex& c = t.V[2];
                    double area = (double)(b.X - a.X) * (c.Y - a.Y) - (double)(c.X - a.X) * (b.Y - a.Y);
                    if (area == 0) return;
                    const double inv = 1.0 / area;
                    // Barycentric weight i at (x, y) = E[i][0] * x + E[i][1] * y + E[i][2].
                    const double E[3][3] = {
                        { (b.Y - c.Y) * inv, (c.X - b.X) * inv, ((double)b.X * c.Y - (double)c.X * b.Y) * inv },
                        { (c.Y - a.Y) * inv, (a.X - c.X) * inv, ((double)c.X * a.Y - (double)a.X * c.Y) * inv },
                        { (a.Y - b.Y) * inv, (b.X - a.X) * inv, ((double)a.X * b.Y - (double)b.X * a.Y) * inv },
                    };

                    const u32 attr = t.Attr;
                    const u32 polyAlpha = (attr >> 16) & 0x1F;
                    const bool depthEqual = attr & (1 << 14);
                    constexpr bool wbuffer = decltype(wbC)::value;
                    const s32 equalTolerance = wbuffer ? 0xFF : 0x200;
                    const u8 polyId = (u8)(((attr >> 24) & 0x3F) + 1);
                    const u8 fogBit = (attr & (1 << 15)) ? 0x80 : 0;
                    // In W-buffer mode the stored depth is W; the vertices carry it scaled.
                    const float wScale = wbuffer ? a.Z * a.InvW : 0;
                    constexpr bool textured = decltype(texC)::value;
                    Texture* const tex = t.Tex;
                    const u32 texParam = t.TexParam;
                    const int texW = tex ? tex->W : 0, texH = tex ? tex->H : 0;
                    const u32 blendMode = (attr >> 4) & 0x3;
                    const bool highlight = blendMode == 2 && (dispCnt & (1 << 1));
                    const bool toon = blendMode == 2 && !highlight;
                    // Most polygons have one colour at all three corners.
                    constexpr bool flat = decltype(flatC)::value;
                    const int flatR = (int)a.R, flatG = (int)a.G, flatB = (int)a.B;

                    // Local copies: the loop below writes through byte pointers, which would
                    // otherwise make the compiler read all of this from memory again per pixel.
                    const float aIW = a.InvW, bIW = b.InvW, cIW = c.InvW, aZ = a.Z, bZ = b.Z, cZ = c.Z;
                    const float aR = a.R, bR = b.R, cR = c.R, aG = a.G, bG = b.G, cG = c.G, aB = a.B, bB = b.B, cB = c.B;
                    const float aS = a.S, bS = b.S, cS = c.S, aT = a.T, bT = b.T, cT = c.T;
                    u32* const texels = tex ? tex->Texels.data() : nullptr;
                    // A texel, decoded the first time it is used in this frame.
                    auto fetchTexel = [&](int ts, int tv) -> u32 {
                        u32 texel = texels[(size_t)tv * texW + ts];
                        if (!texel)
                        {
                            u16 tcolor;
                            u8 talpha;
                            softP->TextureLookup(*gpuP, tex->Param, tex->Palette, (s16)(ts << 4), (s16)(tv << 4), &tcolor, &talpha);
                            u32 tr = (tcolor << 1) & 0x3E; if (tr) tr++;
                            u32 tg = (tcolor >> 4) & 0x3E; if (tg) tg++;
                            u32 tb = (tcolor >> 9) & 0x3E; if (tb) tb++;
                            texel = 0x80000000 | tr | (tg << 8) | (tb << 16) | ((u32)talpha << 24);
                            texels[(size_t)tv * texW + ts] = texel;
                        }
                        return texel;
                    };
                    const bool frontFirst = t.FrontFirst;
                    const u16 order = t.Order;
                    const int Wl = W;
                    u32* const backData = BackPx;
                    // (the strip's buffers start at its first row)
                    s32* const depthData = buffers.Depth.data() - (ptrdiff_t)rowA * W;
                    u8* const attrData = buffers.Attr.data() - (ptrdiff_t)rowA * W;
                    u16* const writerData = buffers.Writer.data() - (ptrdiff_t)rowA * W;
                    const u16* const toonTable = toonCopy;
                    // Vertices are placed to a sixteenth of a DS pixel, which is a visible fraction
                    // of an output pixel, so where three polygons meet along an edge a sliver can
                    // stay open. Each polygon is grown by a third of an output pixel to close them.
                    const double margin[3] = {
                        0.34 * std::sqrt(E[0][0] * E[0][0] + E[0][1] * E[0][1]),
                        0.34 * std::sqrt(E[1][0] * E[1][0] + E[1][1] * E[1][1]),
                        0.34 * std::sqrt(E[2][0] * E[2][0] + E[2][1] * E[2][1]),
                    };

                    const double xmin = std::min({ (double)a.X, (double)b.X, (double)c.X });
                    const double xmax = std::max({ (double)a.X, (double)b.X, (double)c.X });
                    const int y0 = std::max(t.Y0, rowA), y1 = std::min(t.Y1, rowB - 1);
                    for (int y = y0; y <= y1; y++)
                    {
                        const double py = y + 0.5;
                        // The span of this row: where all three weights are non-negative.
                        double lo = 0, hi = Wl;
                        bool empty = false;
                        for (int i = 0; i < 3; i++)
                        {
                            const double k = E[i][1] * py + E[i][2] + margin[i];
                            if (E[i][0] > 1e-12)
                                lo = std::max(lo, -k / E[i][0]);
                            else if (E[i][0] < -1e-12)
                                hi = std::min(hi, -k / E[i][0]);
                            else if (k < -1e-6)
                                empty = true;
                        }
                        if (empty) continue;
                        // At a sharp corner the widened edges meet well beyond the vertex, which
                        // would draw a streak along that row; the span ends where the triangle does.
                        lo = std::max(lo, xmin - 0.4);
                        hi = std::min(hi, xmax + 0.4);
                        int x0 = (int)std::ceil(lo - 0.5 - 0.02), x1 = (int)std::floor(hi - 0.5 + 0.02);
                        if (x0 < 0) x0 = 0;
                        if (x1 >= Wl) x1 = Wl - 1;
                        if (x1 < x0) continue;

                        u32* color = backData + (size_t)y * Wl;
                        s32* depth = depthData + (size_t)y * Wl;
                        u8* pattr = attrData + (size_t)y * Wl;
                        u16* writer = writerData + (size_t)y * Wl;

                        const float l0 = (float)(E[0][0] * (x0 + 0.5) + E[0][1] * py + E[0][2]);
                        const float l1 = (float)(E[1][0] * (x0 + 0.5) + E[1][1] * py + E[1][2]);
                        const float s0 = (float)E[0][0], s1 = (float)E[1][0];
                        // Up to the depth test and the texel's place, a group of pixels at a time (SIMD);
                        // what passes is then textured, shaded and written one pixel at a time.
                        for (int xb = x0; xb <= x1; xb += Lanes)
                        {
                            const int n = std::min(Lanes, x1 - xb + 1);
                            const VF k = (float)(xb - x0) + LaneIndexF();
                            const VF f0 = l0 + k * s0, f1 = l1 + k * s1;
                            // Samples a hair outside the triangle (the widened spans) are pulled
                            // back onto it, and all of them slightly towards its middle, so that
                            // texture coordinates never reach past the polygon's own texels.
                            VF w0 = Select(f0 < 0, VF{}, f0), w1 = Select(f1 < 0, VF{}, f1), w2 = 1.0f - w0 - w1;
                            const VI outside = w2 < 0;
                            if (Any(outside))
                            {
                                const VF kk = 1.0f / (w0 + w1);
                                w0 = Select(outside, w0 * kk, w0);
                                w1 = Select(outside, w1 * kk, w1);
                                w2 = Select(outside, VF{}, w2);
                            }
                            w0 = w0 * 0.9994f + 0.0002f;
                            w1 = w1 * 0.9994f + 0.0002f;
                            w2 = w2 * 0.9994f + 0.0002f;
                            // Perspective-correct weights.
                            VF q0 = w0 * aIW, q1 = w1 * bIW, q2 = w2 * cIW;
                            const VF qs = q0 + q1 + q2;
                            VI ok = (qs > 0) & (LaneIndex() < n);
                            const VF iq = 1.0f / qs;
                            // In W-buffer mode the stored depth is W; the vertices carry it scaled.
                            const VI z = wbuffer ? __builtin_convertvector(wScale * iq, VI) : __builtin_convertvector(w0 * aZ + w1 * bZ + w2 * cZ, VI);
                            VI dz;
                            memcpy(&dz, depth + xb, sizeof(dz));
                            if (depthEqual)
                            {
                                const VI diff = dz - z;
                                ok &= (diff >= -equalTolerance) & (diff <= equalTolerance);
                            }
                            else if (frontFirst)
                            {
                                // On equal depth the one first in the game's order wins.
                                VI wr;
                                for (int i = 0; i < Lanes; i++) wr[i] = writer[xb + i];
                                ok &= (z < dz) | ((z == dz) & ((s32)order < wr));
                            }
                            else
                                ok &= z < dz;
                            if (!Any(ok)) continue;
                            q0 *= iq;
                            q1 *= iq;
                            q2 *= iq;
                            VI vts{}, vtv{};
                            if (textured)
                            {
                                // (+32768 keeps the value positive so the conversion rounds down)
                                vts = (__builtin_convertvector(q0 * aS + q1 * bS + q2 * cS + 32768.0f, VI) - 32768) >> 4;
                                vtv = (__builtin_convertvector(q0 * aT + q1 * bT + q2 * cT + 32768.0f, VI) - 32768) >> 4;
                                vts = WrapCoord(vts, texW, texParam & (1 << 16), texParam & (1 << 18));
                                vtv = WrapCoord(vtv, texH, texParam & (1 << 17), texParam & (1 << 19));
                            }
                            if (!highlight && !toon)
                            {
                                // The usual case, on the whole group: the texel and alpha test, the colour, and
                                // what goes into the picture; only the writes are made one pixel at a time.
                                VI alphaV = VI{} + (s32)polyAlpha, texel{};
                                if (textured)
                                {
                                    for (int i = 0; i < n; i++)
                                        if (ok[i]) texel[i] = (s32)fetchTexel(vts[i], vtv[i]);
                                    if (!(blendMode & 1))
                                        alphaV = ((((texel >> 24) & 0x1F) + 1) * (s32)(polyAlpha + 1) - 1) >> 5;
                                }
                                if (polyAlpha == 0) alphaV = VI{} + 31; // wireframe polygons are drawn filled here
                                ok &= alphaV > (s32)alphaRef;
                                if (!Any(ok)) continue;
                                VI vr = VI{} + flatR, vg = VI{} + flatG, vb = VI{} + flatB;
                                if (!flat)
                                {
                                    vr = Clamp63(__builtin_convertvector(q0 * aR + q1 * bR + q2 * cR + 0.5f, VI));
                                    vg = Clamp63(__builtin_convertvector(q0 * aG + q1 * bG + q2 * cG + 0.5f, VI));
                                    vb = Clamp63(__builtin_convertvector(q0 * aB + q1 * bB + q2 * cB + 0.5f, VI));
                                }
                                VI vR = vr, vG = vg, vB = vb;
                                if (textured)
                                {
                                    const VI tr = texel & 0x3F, tg = (texel >> 8) & 0x3F, tb = (texel >> 16) & 0x3F, ta = (texel >> 24) & 0x1F;
                                    if (blendMode & 1)
                                    {
                                        // decal
                                        const VI none = ta == 0, full = ta == 31;
                                        vR = Select(none, vr, Select(full, tr, (tr * ta + vr * (31 - ta)) >> 5));
                                        vG = Select(none, vg, Select(full, tg, (tg * ta + vg * (31 - ta)) >> 5));
                                        vB = Select(none, vb, Select(full, tb, (tb * ta + vb * (31 - ta)) >> 5));
                                    }
                                    else
                                    {
                                        // modulate
                                        vR = ((tr + 1) * (vr + 1) - 1) >> 6;
                                        vG = ((tg + 1) * (vg + 1) - 1) >> 6;
                                        vB = ((tb + 1) * (vb + 1) - 1) >> 6;
                                    }
                                }
                                vR = (vR << 2) | (vR >> 4);
                                vG = (vG << 2) | (vG >> 4);
                                vB = (vB << 2) | (vB >> 4);
                                for (int i = 0; i < n; i++)
                                {
                                    if (!ok[i]) continue;
                                    const int x = xb + i;
                                    if (alphaV[i] == 31)
                                    {
                                        color[x] = 0xFF000000 | ((u32)vR[i] << 16) | ((u32)vG[i] << 8) | (u32)vB[i];
                                        depth[x] = z[i];
                                        pattr[x] = fogBit;
                                        if (frontFirst) writer[x] = order;
                                        continue;
                                    }
                                    const s32 zl = z[i];
                                    const u32 alpha = (u32)alphaV[i];
                                    u32 r = vR[i], gr = vG[i], bl = vB[i];
                                    // Translucent pixels of one polygon ID are not drawn over each other.
                                    if ((pattr[x] & 0x7F) == polyId) continue;
                                    const u32 dst = color[x];
                                    u32 da = dst >> 24;
                                    if (da && blendEnabled)
                                    {
                                        const u32 f = alpha + 1;
                                        r = (r * f + ((dst >> 16) & 0xFF) * (32 - f)) >> 5;
                                        gr = (gr * f + ((dst >> 8) & 0xFF) * (32 - f)) >> 5;
                                        bl = (bl * f + (dst & 0xFF) * (32 - f)) >> 5;
                                    }
                                    da = std::max(da, (alpha << 3) | (alpha >> 2));
                                    color[x] = (da << 24) | (r << 16) | (gr << 8) | bl;
                                    if (attr & (1 << 11))
                                    {
                                        depth[x] = zl;
                                        if (frontFirst) writer[x] = order;
                                    }
                                    pattr[x] = (u8)(polyId | (pattr[x] & fogBit));
                                }
                            }
                            else
                                for (int i = 0; i < n; i++)
                                {
                                    if (!ok[i]) continue;
                                    const int x = xb + i;
                                    const s32 zl = z[i];
                                    const float pq0 = q0[i], pq1 = q1[i], pq2 = q2[i];
                                    // The alpha test first: it needs only the texel, and a quarter of the
                                    // pixels (the clear parts of billboards) stop there.
                                    u32 r, gr, bl, alpha, texel = 0;
                                    if (textured)
                                    {
                                        const int ts = vts[i], tv = vtv[i];
                                        texel = fetchTexel(ts, tv);
                                        const u32 ta = (texel >> 24) & 0x1F;
                                        alpha = (blendMode & 1) ? polyAlpha : ((ta + 1) * (polyAlpha + 1) - 1) >> 5;
                                    }
                                    else
                                        alpha = polyAlpha;
                                    if (polyAlpha == 0) alpha = 31; // wireframe polygons are drawn filled here
                                    if (alpha <= alphaRef) continue;

                                    int vr = flatR, vg = flatG, vb = flatB;
                                    if (!flat)
                                    {
                                        vr = std::clamp((int)(pq0 * aR + pq1 * bR + pq2 * cR + 0.5f), 0, 63);
                                        vg = std::clamp((int)(pq0 * aG + pq1 * bG + pq2 * cG + 0.5f), 0, 63);
                                        vb = std::clamp((int)(pq0 * aB + pq1 * bB + pq2 * cB + 0.5f), 0, 63);
                                    }
                                    const int rawR = vr;
                                    if (highlight)
                                        vg = vb = vr;
                                    else if (toon)
                                    {
                                        u16 tc = toonTable[vr >> 1];
                                        vr = (tc << 1) & 0x3E; if (vr) vr++;
                                        vg = (tc >> 4) & 0x3E; if (vg) vg++;
                                        vb = (tc >> 9) & 0x3E; if (vb) vb++;
                                    }

                                    if (textured)
                                    {
                                        const u32 tr = texel & 0x3F, tg = (texel >> 8) & 0x3F, tb = (texel >> 16) & 0x3F, ta = (texel >> 24) & 0x1F;
                                        if (blendMode & 1)
                                        {
                                            // decal
                                            if (ta == 0) { r = vr; gr = vg; bl = vb; }
                                            else if (ta == 31) { r = tr; gr = tg; bl = tb; }
                                            else
                                            {
                                                r = ((tr * ta) + (vr * (31 - ta))) >> 5;
                                                gr = ((tg * ta) + (vg * (31 - ta))) >> 5;
                                                bl = ((tb * ta) + (vb * (31 - ta))) >> 5;
                                            }
                                        }
                                        else
                                        {
                                            // modulate
                                            r = ((tr + 1) * (vr + 1) - 1) >> 6;
                                            gr = ((tg + 1) * (vg + 1) - 1) >> 6;
                                            bl = ((tb + 1) * (vb + 1) - 1) >> 6;
                                        }
                                    }
                                    else
                                    {
                                        r = vr;
                                        gr = vg;
                                        bl = vb;
                                    }
                                    if (highlight)
                                    {
                                        u16 tc = toonTable[rawR >> 1];
                                        u32 hr = (tc << 1) & 0x3E; if (hr) hr++;
                                        u32 hg = (tc >> 4) & 0x3E; if (hg) hg++;
                                        u32 hb = (tc >> 9) & 0x3E; if (hb) hb++;
                                        r = std::min<u32>(63, r + hr);
                                        gr = std::min<u32>(63, gr + hg);
                                        bl = std::min<u32>(63, bl + hb);
                                    }

                                    r = (r << 2) | (r >> 4);
                                    gr = (gr << 2) | (gr >> 4);
                                    bl = (bl << 2) | (bl >> 4);

                                    if (alpha == 31)
                                    {
                                        color[x] = 0xFF000000 | (r << 16) | (gr << 8) | bl;
                                        depth[x] = zl;
                                        pattr[x] = fogBit;
                                        if (frontFirst) writer[x] = order;
                                    }
                                    else
                                    {
                                        // Translucent pixels of one polygon ID are not drawn over each other.
                                        if ((pattr[x] & 0x7F) == polyId) continue;
                                        const u32 dst = color[x];
                                        u32 da = dst >> 24;
                                        if (da && blendEnabled)
                                        {
                                            const u32 f = alpha + 1;
                                            r = (r * f + ((dst >> 16) & 0xFF) * (32 - f)) >> 5;
                                            gr = (gr * f + ((dst >> 8) & 0xFF) * (32 - f)) >> 5;
                                            bl = (bl * f + (dst & 0xFF) * (32 - f)) >> 5;
                                        }
                                        da = std::max(da, (alpha << 3) | (alpha >> 2));
                                        color[x] = (da << 24) | (r << 16) | (gr << 8) | bl;
                                        if (attr & (1 << 11))
                                        {
                                            depth[x] = zl;
                                            if (frontFirst) writer[x] = order;
                                        }
                                        pattr[x] = (u8)(polyId | (pattr[x] & fogBit));
                                    }
                                }
                        }
                    }

            };
            using T = std::true_type;
            using F = std::false_type;
            for (const Tri& t : tris)
            {
                if (t.Y1 < rowA || t.Y0 >= rowB) continue;
                const WideVertex& a = t.V[0];
                const WideVertex& b = t.V[1];
                const WideVertex& c = t.V[2];
                const bool flat = a.R == b.R && a.R == c.R && a.G == b.G && a.G == c.G && a.B == b.B && a.B == c.B;
                switch ((t.Tex ? 4 : 0) | (t.WBuffer ? 2 : 0) | (flat ? 1 : 0))
                {
                case 0: raster(t, F(), F(), F()); break;
                case 1: raster(t, F(), F(), T()); break;
                case 2: raster(t, F(), T(), F()); break;
                case 3: raster(t, F(), T(), T()); break;
                case 4: raster(t, T(), F(), F()); break;
                case 5: raster(t, T(), F(), T()); break;
                case 6: raster(t, T(), T(), F()); break;
                case 7: raster(t, T(), T(), T()); break;
                }
            }

            if (fogEnabled)
            {
                for (int y = rowA; y < rowB; y++)
                {
                    u32* color = &BackPx[(size_t)y * W];
                    const s32* depth = &buffers.Depth[(size_t)(y - rowA) * W];
                    const u8* pattr = &buffers.Attr[(size_t)(y - rowA) * W];
                    for (int x = 0; x < W; x++)
                    {
                        if (!(pattr[x] & 0x80) || !(color[x] >> 24)) continue;
                        u32 z = (u32)depth[x], id, frac;
                        if (z < fogOffset)
                        {
                            id = 0;
                            frac = 0;
                        }
                        else
                        {
                            z -= fogOffset;
                            z = (z >> 2) << fogShift;
                            id = z >> 17;
                            if (id >= 32)
                            {
                                id = 32;
                                frac = 0;
                            }
                            else
                                frac = z & 0x1FFFF;
                        }
                        u32 density = ((fogDensity[id] * (0x20000 - frac)) +
                            (fogDensity[id + 1] * frac)) >> 17;
                        if (density >= 127) density = 128;
                        if (!density || !fogColorOn) continue;
                        const u32 c = color[x];
                        u32 r = (fogR * density + ((c >> 16) & 0xFF) * (128 - density)) >> 7;
                        u32 gr = (fogG * density + ((c >> 8) & 0xFF) * (128 - density)) >> 7;
                        u32 b = (fogB * density + (c & 0xFF) * (128 - density)) >> 7;
                        color[x] = (c & 0xFF000000) | (r << 16) | (gr << 8) | b;
                    }
                }
            }
        };
        static std::function<void(int)> job;
        job = [=](int) {
            for (int i; (i = nextStrip.fetch_add(1)) < strips;) strip(i);
        };
        static const bool prof = getenv("SSR_WIDE_PROF") != nullptr;
        auto t0 = std::chrono::steady_clock::now();
        Pool->Start(job);
        Drawing = true;
        if (prof)
        {
            Wait();
            static double total = 0;
            static u64 frames = 0, triCount = 0;
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            triCount += tris.size();
            if (++frames % 200 == 0)
                fprintf(stderr, "[wide] %dx%d: %.2f ms per draw, %llu triangles, %d threads\n", W, H, total / frames,
                    (unsigned long long)(triCount / frames), Pool->Count());
        }
    }
};

void WideWait() { WideAccess::Wait(); }
void WideWaitForTextures()
{
    if (WideAccess::Drawing && !WideAccess::DrawingOnWideGpu) WideAccess::Wait();
}
void WideShutdown() { WideAccess::Shutdown(); }
void WideReleaseGpu() { WideAccess::ReleaseGpu(); }
void WideOnFlush(GPU3D& g, bool interpolate) { WideAccess::OnFlush(g, interpolate); }
bool WideListsOnHelper() { return WideAccess::ListsOnHelper(); }
bool WideReplayPaired(GPU3D& g, Vertex* vram, Polygon* pram, Polygon** list, u32& numPolys, u32& matched, size_t& frameSize)
{
    return WideAccess::ReplayPaired(g, vram, pram, list, numPolys, matched, frameSize);
}
void WideOnRender(GPU& gpu, bool showingInterp) { WideAccess::OnRender(gpu, showingInterp); }
void WideReplay(GPU3D& g, bool interpolate, Vertex* vram, Polygon* pram, Polygon** list,
    u32& numPolys, u32& matched)
{
    WideAccess::Replay(g, interpolate, 1, 1, vram, pram, list, numPolys, matched, g.RawCurrent, g.RawPrevious);
}
}
