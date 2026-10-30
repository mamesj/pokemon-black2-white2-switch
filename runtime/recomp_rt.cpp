// Dispatcher for recompiled chunks.
//
// Several overlays are linked at the same addresses, so an address alone does not
// identify code. A chunk is only entered after its bytes have been compared with
// what is in emulated memory, and every recompiled instruction re-checks its own
// opcode, so a stale chunk can never execute the wrong instruction.
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "NDS.h"
#include "ARM.h"
#include "ARMInterpreter.h"
#include "SsrWatch.h"
#include "recomp_rt.h"
#include "recomp_api.h"

using namespace melonDS;

bool rc_single = false;
RcStats rc_stats;
u32 rc_frame = 1;
u32 rc_section_last_frame[512];

namespace
{
const u32 kMainBase = 0x02000000, kMainEnd = 0x02400000;
const u32 kItcmBase = 0x01FF8000, kItcmEnd = 0x02000000;
const u32 kBiosBase = 0xFFFF0000, kBiosEnd = 0xFFFF1000;

// Chunk index + 1 for every halfword of executable address space; 0 means nothing stamped.
std::vector<u32> viewMain((kMainEnd - kMainBase) >> 1);
std::vector<u32> viewItcm((kItcmEnd - kItcmBase) >> 1);
std::vector<u32> viewBios((kBiosEnd - kBiosBase) >> 1);

// After a failed byte comparison, skip this many lookups before comparing again.
std::vector<u8> retryDelay;
std::vector<u32> chunkLastFrame;

struct Probe
{
    u32 addr;
    u32 section;
    u32 lastFrame;
    u32 next; // next probe at the same address (index + 1), for overlays that share addresses
    u32 chunk; // the chunk the address is in (~0u: none): a function is not always a chunk's start
    RcHook hook = nullptr; // called on every entry (rc_add_hook), not only once a frame
    void* hookUser = nullptr;
};
std::vector<Probe> probes;
std::vector<u32> probeHead; // per halfword of main RAM: first probe index + 1
std::vector<u8> chunkHasProbe; // lets the dispatcher skip that table for most chunks
u64 stamps = 0, evictions = 0;

// Addresses where Miss found no chunk, so that code run there by the interpreter (a
// section changed at run time, such as the copy-protection code that decrypts itself)
// does not search every section again for each instruction. An entry lasts a few
// frames, after which the address is looked up again (code may have been loaded there).
struct NoChunk
{
    u32 addr; // address | thumb, 0 = empty
    u32 until; // rc_frame up to which it holds
};
NoChunk noChunk[4096];
const u32 kNoChunkFrames = 4;
inline NoChunk& NoChunkSlot(u32 key) { return noChunk[(key ^ (key >> 12)) & 4095]; }

inline u32* ViewSlot(u32 addr)
{
    if (addr >= kMainBase && addr < kMainEnd) return &viewMain[(addr - kMainBase) >> 1];
    if (addr >= kItcmBase && addr < kItcmEnd) return &viewItcm[(addr - kItcmBase) >> 1];
    if (addr >= kBiosBase && addr < kBiosEnd) return &viewBios[(addr - kBiosBase) >> 1];
    return nullptr;
}

const u8* GuestPtr(ARMv5* cpu, u32 addr)
{
    if (addr >= kBiosBase) return cpu->NDS.GetARM9BIOS().data() + (addr - kBiosBase);
    if (addr >= kMainBase) return cpu->NDS.MainRAM + (addr & cpu->NDS.MainRAMMask);
    return cpu->ITCM + (addr & (ITCMPhysicalSize - 1));
}

void Stamp(u32 id)
{
    const RcChunk& c = rc_chunks[id];
    for (u32 a = c.start; a < c.end; a += 2)
        if (u32* slot = ViewSlot(a)) *slot = id + 1;
    stamps++;
}

void Evict(u32 id)
{
    const RcChunk& c = rc_chunks[id];
    for (u32 a = c.start; a < c.end; a += 2)
        if (u32* slot = ViewSlot(a))
            if (*slot == id + 1) *slot = 0;
    evictions++;
}

// Finds a chunk whose code is really present at addr and stamps it. Returns chunk index + 1, or 0.
u32 Miss(ARMv5* cpu, u32 addr, bool thumb)
{
    for (u32 s = 0; s < rc_num_sections; s++)
    {
        const RcSection& sec = rc_sections[s];
        if (addr < sec.base || addr >= sec.base + sec.size || !sec.numChunks) continue;

        u32 lo = sec.firstChunk, hi = sec.firstChunk + sec.numChunks;
        while (lo < hi)
        {
            u32 mid = (lo + hi) / 2;
            if (rc_chunks[mid].end <= addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= sec.firstChunk + sec.numChunks) continue;
        const RcChunk& c = rc_chunks[lo];
        if (addr < c.start || (bool)c.thumb != thumb) continue;

        if (retryDelay[lo])
        {
            retryDelay[lo]--;
            continue;
        }
        if (memcmp(sec.image + (c.start - sec.base), GuestPtr(cpu, c.start), c.end - c.start) != 0)
        {
            retryDelay[lo] = 255;
            continue;
        }
        Stamp(lo);
        return lo + 1;
    }
    return 0;
}

void BailCommon(u32 section, u32 addr)
{
    rc_stats.guardFails++;
    if (u32* slot = ViewSlot(addr))
        if (*slot) Evict(*slot - 1);
    (void)section;
}
}

// Tables without the game's code (rc_register_portable): the game the ROM is, and where each
// chunk is in it (SoulSilver's addresses with that game's shifts applied).
namespace
{
const RcPortable* portable = nullptr;
const RcPortableGame* portableGame = nullptr;
bool portableLoaded = false;
std::vector<std::vector<u8>> portableImages;
std::vector<u32> chunkStart, chunkEnd;

// A SoulSilver address in section `sec` as it is in the game being played.
u32 Remap(u32 sec, u32 addr)
{
    if (!portableGame) return addr;
    const char* name = rc_sections[sec].name;
    for (unsigned i = 0; i < portableGame->numShifts; i++)
    {
        const RcPortableShift& sh = portableGame->shifts[i];
        if (addr >= sh.from && addr < sh.to && !strcmp(sh.section, name)) return addr + sh.shift;
    }
    return addr;
}

void ChunkAddresses()
{
    if (chunkStart.size() == rc_num_chunks) return;
    chunkStart.resize(rc_num_chunks);
    chunkEnd.resize(rc_num_chunks);
    for (u32 i = 0; i < rc_num_chunks; i++)
    {
        const RcChunk& c = rc_chunks[i];
        chunkStart[i] = Remap(c.section, c.start);
        chunkEnd[i] = chunkStart[i] + (c.end - c.start);
    }
}
}

bool rc_register_portable(const RcPortable* tables)
{
    portable = tables;
    return true;
}

bool rc_portable_needed() { return portable && !portableLoaded; }

const char* rc_portable_load(const char code[4], const unsigned char* (*overlay)(void*, int, unsigned*), void* user)
{
    if (!portable) return nullptr;
    portableLoaded = true;
    portableGame = nullptr;
    for (unsigned g = 0; g < portable->numGames; g++)
        if (!memcmp(portable->games[g].code, code, 4)) portableGame = &portable->games[g];
    if (!portableGame)
    {
        fprintf(stderr, "[recomp] game %.4s: not one these tables know; layouts that need the game's functions are off\n", code);
        return nullptr;
    }
    portableImages.assign(rc_num_sections, {});
    unsigned loaded = 0, missing = 0;
    for (u32 s = 0; s < rc_num_sections; s++)
    {
        if (portable->overlayOf[s] < 0) continue;
        unsigned size = 0;
        const unsigned char* data = overlay(user, portable->overlayOf[s], &size);
        if (!data) { missing++; continue; }
        portableImages[s].assign(data, data + size);
        rc_sections[s].image = portableImages[s].data();
        // (An overlay can be a little longer in another game: HeartGold's title screen.)
        if (size > rc_sections[s].size) rc_sections[s].size = size;
        loaded++;
    }
    chunkStart.clear();
    ChunkAddresses();
    fprintf(stderr, "[recomp] %s (%.4s): code of %u overlays read from the ROM%s\n", portableGame->name, code, loaded,
        missing ? ", some missing" : "");
    return portableGame->name;
}

// The watch used without the recompiled code (rc_watch_attach).
namespace
{
const u32 kWatchBase = 0x01FF8000, kWatchEnd = 0x02400000; // ITCM and main RAM
NDS* watchNds = nullptr;
std::vector<u8> watchBitmap;                             // one bit per halfword
// Chunk indices by start address: per watched halfword, an index + 1 into watchList, where
// the chunks starting there follow each other, ended by ~0u (0: none).
std::vector<u32> watchFirst, watchList;
std::vector<u8> sectionShared;                           // other sections use its addresses

void WatchAddress(u32 addr)
{
    if (watchBitmap.empty() || addr < kWatchBase || addr >= kWatchEnd) return;
    const u32 bit = (addr - kWatchBase) >> 1;
    watchBitmap[bit >> 3] |= 1 << (bit & 7);
}

// Whether the chunk's own code is in memory: many overlays are loaded at the same addresses.
// Its first bytes are compared (up to 16; a chunk is a whole function, rarely shorter).
bool WatchChunkPresent(u32 id)
{
    const RcChunk& c = rc_chunks[id];
    const RcSection& sec = rc_sections[c.section];
    if (!sectionShared[c.section]) return true;
    if (!sec.image) return false;
    const u32 start = chunkStart[id];
    u32 n = c.end - c.start < 16 ? c.end - c.start : 16;
    if (start + n > sec.base + sec.size) return false;
    return memcmp(sec.image + (start - sec.base), GuestPtr(&watchNds->ARM9, start), n) == 0;
}

void WatchHit(void*, u32 addr, bool thumb)
{
    // A probe: on a function, which is not always the start of a chunk (the recompiler puts
    // small functions together), so it is looked up by its own address. Its overlay is told
    // by the bytes of the chunk it is in.
    if (!probeHead.empty() && addr >= kMainBase && addr < kMainEnd)
        for (u32 p = probeHead[(addr - kMainBase) >> 1]; p; p = probes[p - 1].next)
        {
            Probe& pr = probes[p - 1];
            if (pr.chunk == ~0u || (bool)rc_chunks[pr.chunk].thumb != thumb) continue;
            if (pr.hook)
            {
                if (WatchChunkPresent(pr.chunk)) pr.hook(pr.hookUser, &watchNds->ARM9);
                continue;
            }
            if (pr.lastFrame == rc_frame) continue;
            if (!WatchChunkPresent(pr.chunk)) continue;
            pr.lastFrame = rc_frame;
            rc_section_last_frame[pr.section] = rc_frame;
            chunkLastFrame[pr.chunk] = rc_frame;
        }
    const u32 first = watchFirst[(addr - kWatchBase) >> 1];
    if (!first) return;
    for (u32 i = first - 1; watchList[i] != ~0u; i++)
    {
        const u32 id = watchList[i];
        const RcChunk& c = rc_chunks[id];
        if ((bool)c.thumb != thumb) continue;
        // Most functions are entered many times a frame: once is enough to know.
        if (chunkLastFrame[id] == rc_frame) return;
        if (!WatchChunkPresent(id)) continue;
        rc_section_last_frame[c.section] = rc_frame;
        chunkLastFrame[id] = rc_frame;
        return;
    }
}
}

bool rc_watch_attach(NDS& nds)
{
    if (SsrWatch.Hook) return false;
    if (retryDelay.empty()) rc_reset();
    watchNds = &nds;
    watchBitmap.assign(((kWatchEnd - kWatchBase) >> 1) / 8 + 1, 0);
    sectionShared.assign(rc_num_sections, 0);
    for (u32 a = 0; a < rc_num_sections; a++)
        for (u32 b = 0; b < rc_num_sections; b++)
            if (a != b && rc_sections[a].base < rc_sections[b].base + rc_sections[b].size &&
                rc_sections[b].base < rc_sections[a].base + rc_sections[a].size)
                sectionShared[a] = 1;
    // Every chunk can be recognised; the overlays' chunk starts are watched, so that it is known
    // which overlays run (the main program and the ITCM run all the time and are only watched
    // where there are probes).
    size_t watched = 0;
    ChunkAddresses();
    std::unordered_map<u32, std::vector<u32>> byStart;
    for (u32 id = 0; id < rc_num_chunks; id++)
        if (chunkStart[id] >= kWatchBase && chunkStart[id] < kWatchEnd)
            byStart[chunkStart[id]].push_back(id);
    watchFirst.assign((kWatchEnd - kWatchBase) >> 1, 0);
    watchList.clear();
    for (auto& [start, ids] : byStart)
    {
        watchFirst[(start - kWatchBase) >> 1] = (u32)watchList.size() + 1;
        watchList.insert(watchList.end(), ids.begin(), ids.end());
        watchList.push_back(~0u);
    }
    for (u32 id = 0; id < rc_num_chunks; id++)
    {
        const RcChunk& c = rc_chunks[id];
        const char* name = rc_sections[c.section].name;
        if (strcmp(name, "main") && strcmp(name, "ITCM") && strcmp(name, "bios9"))
        {
            WatchAddress(chunkStart[id]);
            watched++;
        }
    }
    for (const Probe& p : probes) WatchAddress(p.addr);
    SsrWatch.Bitmap = watchBitmap.data();
    SsrWatch.Base = kWatchBase;
    SsrWatch.End = kWatchEnd;
    SsrWatch.Hook = &WatchHit;
    SsrWatch.User = nullptr;
    fprintf(stderr, "[recomp] without the recompiled code: watching %zu overlay functions and the probes\n", watched);
    return true;
}

void rc_reset()
{
    std::fill(viewMain.begin(), viewMain.end(), 0);
    std::fill(viewItcm.begin(), viewItcm.end(), 0);
    std::fill(viewBios.begin(), viewBios.end(), 0);
    retryDelay.assign(rc_num_chunks, 0);
    chunkLastFrame.assign(rc_num_chunks, 0);
    rc_stats = RcStats();
    stamps = evictions = 0;
    memset(noChunk, 0, sizeof(noChunk));
}

void rc_print_stats()
{
    u64 total = rc_stats.dispatches + rc_stats.interpreted;
    fprintf(stderr, "[recomp] chunk entries %llu, interpreter fallbacks %llu (%.2f%% of dispatches), "
        "guard failures %llu, chunks stamped %llu, evicted %llu\n",
        (unsigned long long)rc_stats.dispatches, (unsigned long long)rc_stats.interpreted,
        total ? 100.0 * rc_stats.interpreted / total : 0.0,
        (unsigned long long)rc_stats.guardFails, (unsigned long long)stamps, (unsigned long long)evictions);
}

void rc_bail_arm(ARMv5* cpu, u32 section)
{
    u32 addr = cpu->R[15] - 8;
    if (cpu->CheckCondition(cpu->CurInstr >> 28))
    {
        u32 icode = ((cpu->CurInstr >> 4) & 0xF) | ((cpu->CurInstr >> 16) & 0xFF0);
        ARMInterpreter::ARMInstrTable[icode](cpu);
    }
    else if ((cpu->CurInstr & 0xFE000000) == 0xFA000000)
        ARMInterpreter::A_BLX_IMM(cpu);
    else
        cpu->AddCycles_C();
    BailCommon(section, addr);
}

void rc_bail_thumb(ARMv5* cpu, u32 section)
{
    u32 addr = cpu->R[15] - 4;
    u32 icode = (cpu->CurInstr >> 6) & 0x3FF;
    ARMInterpreter::THUMBInstrTable[icode](cpu);
    BailCommon(section, addr);
}

namespace melonDS
{
// Called by the core's ARM9 loop in place of interpreting one instruction.
// Returns false if there is no recompiled code for the current PC.
bool RecompRun(ARMv5* cpu)
{
    if (retryDelay.empty()) rc_reset();

    bool thumb = (cpu->CPSR & 0x20) != 0;
    u32 addr = cpu->R[15] - (thumb ? 2 : 4);
    u32* slot = ViewSlot(addr);
    if (!slot)
    {
        rc_stats.interpreted++;
        return false;
    }
    u32 id = *slot;
    if (!id || (bool)rc_chunks[id - 1].thumb != thumb)
    {
        NoChunk& known = NoChunkSlot(addr | thumb);
        if (known.addr == (addr | thumb) && known.until >= rc_frame)
        {
            rc_stats.interpreted++;
            return false;
        }
        id = Miss(cpu, addr, thumb);
        if (!id)
        {
            known.addr = addr | thumb;
            known.until = rc_frame + kNoChunkFrames;
            rc_stats.interpreted++;
            return false;
        }
    }
    rc_stats.dispatches++;
    rc_section_last_frame[rc_chunks[id - 1].section] = rc_frame;
    chunkLastFrame[id - 1] = rc_frame;
    if (!chunkHasProbe.empty() && chunkHasProbe[id - 1] && addr >= kMainBase && addr < kMainEnd)
    {
        for (u32 p = probeHead[(addr - kMainBase) >> 1]; p; p = probes[p - 1].next)
            if (probes[p - 1].section == rc_chunks[id - 1].section)
            {
                probes[p - 1].lastFrame = rc_frame;
                if (probes[p - 1].hook) probes[p - 1].hook(probes[p - 1].hookUser, cpu);
            }
    }
    rc_chunks[id - 1].fn(cpu, addr);
    return true;
}
}

bool rc_ran_recently(const char* section, unsigned addr, unsigned frames)
{
    if (chunkLastFrame.empty()) return false;
    for (u32 s = 0; s < rc_num_sections; s++)
    {
        const RcSection& sec = rc_sections[s];
        if (strcmp(sec.name, section) != 0) continue;
        ChunkAddresses();
        addr = Remap(s, addr);
        for (u32 c = sec.firstChunk; c < sec.firstChunk + sec.numChunks; c++)
            if (addr >= chunkStart[c] && addr < chunkEnd[c])
                return chunkLastFrame[c] && chunkLastFrame[c] + frames >= rc_frame;
    }
    return false;
}

int rc_add_probe(const char* section, unsigned addr)
{
    u32 sec = ~0u;
    for (u32 s = 0; s < rc_num_sections; s++)
        if (!strcmp(rc_sections[s].name, section)) sec = s;
    if (sec == ~0u) return -1;
    ChunkAddresses();
    addr = Remap(sec, addr);
    if (addr < kMainBase || addr >= kMainEnd) return -1;
    if (probeHead.empty()) probeHead.assign((kMainEnd - kMainBase) >> 1, 0);
    u32& head = probeHead[(addr - kMainBase) >> 1];
    probes.push_back({ addr, sec, 0, head, ~0u });
    if (chunkHasProbe.empty()) chunkHasProbe.assign(rc_num_chunks, 0);
    for (u32 c = rc_sections[sec].firstChunk; c < rc_sections[sec].firstChunk + rc_sections[sec].numChunks; c++)
        if (addr >= chunkStart[c] && addr < chunkEnd[c])
        {
            chunkHasProbe[c] = 1;
            probes.back().chunk = c;
        }
    head = (u32)probes.size();
    WatchAddress(addr);
    return (int)probes.size() - 1;
}

int rc_add_hook(const char* section, unsigned addr, RcHook hook, void* user)
{
    const int p = rc_add_probe(section, addr);
    if (p >= 0)
    {
        probes[p].hook = hook;
        probes[p].hookUser = user;
    }
    return p;
}

unsigned rc_probe_last_frame(int probe)
{
    return probe >= 0 && probe < (int)probes.size() ? probes[probe].lastFrame : 0;
}

unsigned rc_current_frame()
{
    return rc_frame;
}
