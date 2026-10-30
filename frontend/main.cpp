// ssrecomp frontend: boots the game on the melonDS core with an SDL2 window,
// or headless with scripted input, screenshots and state hashes for testing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>
#include "sampler.h"
#include "ssr_os.h"
#include <vector>
#include <tuple>
#include <deque>

#ifdef __APPLE__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#if defined(__linux__) || defined(__SWITCH__) // (Android too)
#include <unistd.h>
#endif
#ifndef SSR_NO_PNG
#include <png.h>
#endif
#ifdef __ANDROID__
#include <atomic>
#include <jni.h>
#endif

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "GPU.h"
#include "GPU2D_Soft.h"
#include "SPU.h"
#include "RTC.h"
#include "Savestate.h"
#include "host.h"
#include "layout.h"
#include "ui.h"
#include "merged.h"
#include "audiostretch.h"
#include "gpucompose.h"
#include "emuthread.h"
#include "pc_launcher.h"

// WORLD ON GPU, the widescreen world drawn by the graphics chip (melonDS/src/GPU3D_WideGpu.h):
// deko3D on the Switch, OpenGL on a PC (EGL on Linux, WGL on Windows).
#if defined(SSR_DEKO3D) || (defined(SSR_GPU3D) && !defined(__SWITCH__))
#define SSR_WORLD_ON_GPU 1
#else
#define SSR_WORLD_ON_GPU 0
#endif
// 3D ON GPU, melonDS's OpenGL renderer for the whole 3D scene (GPU3D_GLThread.h): a PC test;
// not offered in the PC all-games program, which has WORLD ON GPU for the same purpose.
#if defined(SSR_GPU3D) && !defined(__SWITCH__) && !defined(SSR_PC_LAUNCHER)
#define SSR_MENU_GPU3D 1
#else
#define SSR_MENU_GPU3D 0
#endif
#ifdef SSR_GPU3D
#include "GPU3D_GLThread.h"
#include "glcontext.h"
#endif
#include "battlewide.h"
#include "platinum.h" // Pokemon Platinum: which of its screens is up
#include "belowscreen.h"
#include "recomp_rt.h"
#include "recomp_api.h"
#include "GPU3D_Wide.h"
#include "GPU3D_WideGpu.h"
#include "SsrThreads.h"
#include "SsrProf.h"
#include "switch_platform.h"
#include "funcwatch.h"
#include "bw1.h" // Pokemon Black and White's own layouts
#include "bw2map.h" // Black 2 and White 2: more of the map loaded beside the player
#if defined(__SWITCH__) && defined(SSR_DEKO3D)
#include "dekoprobe.h"
#endif

#ifdef __ANDROID__
// The app holds the player's own game, so it cannot update itself as the setup window does:
// at start it asks GitHub for the newest release, and if that is newer than this build
// (SSR_VERSION, from CHANGELOG.md) the game says so, to rebuild it on the PC.
namespace UpdateCheck {
static std::atomic<bool> g_done{false};
static std::string g_newer; // set before g_done

// The body of an HTTPS GET through Java (the NDK has no TLS of its own); empty on failure.
static std::string Get(JNIEnv* env, const char* address)
{
    std::string body;
    auto failed = [&]() {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    };
    jclass urlClass = env->FindClass("java/net/URL");
    jclass connClass = env->FindClass("java/net/URLConnection");
    jclass streamClass = env->FindClass("java/io/InputStream");
    if (failed() || !urlClass || !connClass || !streamClass) return body;
    jobject url = env->NewObject(urlClass, env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V"), env->NewStringUTF(address));
    if (failed()) return body;
    jobject conn = env->CallObjectMethod(url, env->GetMethodID(urlClass, "openConnection", "()Ljava/net/URLConnection;"));
    if (failed() || !conn) return body;
    env->CallVoidMethod(conn, env->GetMethodID(connClass, "setConnectTimeout", "(I)V"), 10000);
    env->CallVoidMethod(conn, env->GetMethodID(connClass, "setReadTimeout", "(I)V"), 10000);
    env->CallVoidMethod(conn, env->GetMethodID(connClass, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"),
        env->NewStringUTF("Accept"), env->NewStringUTF("application/vnd.github+json"));
    if (failed()) return body;
    jobject in = env->CallObjectMethod(conn, env->GetMethodID(connClass, "getInputStream", "()Ljava/io/InputStream;"));
    if (failed() || !in) return body;
    jmethodID read = env->GetMethodID(streamClass, "read", "([BII)I");
    jbyteArray buf = env->NewByteArray(4096);
    while (body.size() < (1 << 20))
    {
        jint n = env->CallIntMethod(in, read, buf, 0, 4096);
        if (failed()) { body.clear(); break; }
        if (n < 0) break;
        const size_t at = body.size();
        body.resize(at + n);
        env->GetByteArrayRegion(buf, 0, n, (jbyte*)&body[at]);
    }
    env->CallVoidMethod(in, env->GetMethodID(streamClass, "close", "()V"));
    failed();
    return body;
}

static std::vector<int> Key(const std::string& v)
{
    std::vector<int> key;
    std::stringstream ss(v);
    std::string part;
    while (std::getline(ss, part, '.'))
    {
        if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos) return {};
        key.push_back(atoi(part.c_str()));
    }
    return key;
}

static void Start()
{
    const std::string current = SSR_VERSION;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    JavaVM* vm = nullptr;
    if (current.empty() || !env || env->GetJavaVM(&vm) != JNI_OK) return;
    std::thread([vm, current]() {
        JNIEnv* env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        const std::string body = Get(env, "https://api.github.com/repos/ClickingMaster/ssrecomp/releases/latest");
        vm->DetachCurrentThread();
        // "tag_name": "v1.2.3"
        std::string latest;
        size_t at = body.find("\"tag_name\"");
        if (at != std::string::npos && (at = body.find('"', body.find(':', at))) != std::string::npos)
        {
            const size_t end = body.find('"', at + 1);
            if (end != std::string::npos) latest = body.substr(at + 1, end - at - 1);
        }
        if (!latest.empty() && latest[0] == 'v') latest.erase(0, 1);
        const std::vector<int> a = Key(latest), b = Key(current);
        if (latest.empty() || a.empty())
            fprintf(stderr, "update check: no answer from GitHub\n");
        else
        {
            fprintf(stderr, "update check: this is %s, the newest release is %s\n", current.c_str(), latest.c_str());
            if (!b.empty() && a > b) g_newer = latest;
        }
        g_done = true;
    }).detach();
}
}
#endif

// With --log-sections and a shot directory, a screenshot is taken around every change of merged scene.
static std::set<uint32_t> g_sceneShots;

// Defined in the 2D renderer: layers not to draw, per engine (see GPU2D_Soft.cpp).
extern unsigned rc_hide_layers[2];
extern bool rc_frame_hidden, rc_wide_render_skip; // melonDS/src/GPU.cpp

using namespace melonDS;

namespace Host
{
bool StopRequested = false;
int LogLevel = 2;

static std::vector<uint8_t> SaveData;
static bool SaveIsDirty = false;

void SaveDirty(const uint8_t* data, uint32_t len)
{
    SaveData.assign(data, data + len);
    SaveIsDirty = true;
}
}

enum
{
    KEY_A, KEY_B, KEY_SELECT, KEY_START, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_R, KEY_L, KEY_X, KEY_Y,
};

struct ScriptEvent
{
    uint32_t start, end;
    uint32_t keys = 0;
    bool touch = false;
    int tx = 0, ty = 0;
    // Virtual controller input, for testing the controller paths without hardware.
    uint32_t padButtons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

// Controller state in SDL's button order, filled from a real pad or from a test script.
enum
{
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LS, PAD_RS, PAD_LB, PAD_RB,
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_COUNT
};

struct PadState
{
    bool btn[PAD_COUNT] = {};
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

#include "touchpad.h"
#include "controls.h"
#include "cheats.h"

static int PadButtonFromName(const std::string& n)
{
    static const char* names[PAD_COUNT] = { "a", "b", "x", "y", "back", "guide", "start", "ls", "rs", "lb", "rb",
        "up", "down", "left", "right" };
    for (int i = 0; i < PAD_COUNT; i++)
        if (n == names[i]) return i;
    return -1;
}

static int KeyFromName(const std::string& n)
{
    static const char* names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y" };
    for (int i = 0; i < 12; i++)
        if (n == names[i]) return i;
    return -1;
}

// Script lines: "<first frame> <last frame> KEY [KEY...]" or "<first> <last> touch <x> <y>"
static std::vector<ScriptEvent> LoadScript(const std::string& path)
{
    std::vector<ScriptEvent> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        ScriptEvent ev;
        std::string tok;
        if (!(ss >> ev.start >> ev.end)) continue;
        while (ss >> tok)
        {
            if (tok == "touch")
            {
                ev.touch = true;
                ss >> ev.tx >> ev.ty;
            }
            else if (tok.rfind("pad:", 0) == 0)
            {
                // pad:a, pad:guide, ... for buttons; pad:rt, pad:lt, pad:rx+, pad:ry-, pad:lx+ ... for axes
                std::string n = tok.substr(4);
                int b = PadButtonFromName(n);
                if (b >= 0) ev.padButtons |= 1u << b;
                else if (n == "rt") ev.rt = 1;
                else if (n == "lt") ev.lt = 1;
                else if (n.size() == 3 && (n[2] == '+' || n[2] == '-'))
                {
                    float v = n[2] == '+' ? 1.0f : -1.0f;
                    if (n.compare(0, 2, "lx") == 0) ev.lx = v;
                    else if (n.compare(0, 2, "ly") == 0) ev.ly = v;
                    else if (n.compare(0, 2, "rx") == 0) ev.rx = v;
                    else if (n.compare(0, 2, "ry") == 0) ev.ry = v;
                    else fprintf(stderr, "script: unknown pad input '%s'\n", tok.c_str());
                }
                else fprintf(stderr, "script: unknown pad input '%s'\n", tok.c_str());
            }
            else
            {
                int k = KeyFromName(tok);
                if (k < 0) fprintf(stderr, "script: unknown key '%s'\n", tok.c_str());
                else ev.keys |= 1u << k;
            }
        }
        out.push_back(ev);
    }
    return out;
}

static uint32_t TopChanges = 0;
static int AudioPeak = 0;
static double AudioSumSq = 0;
static uint64_t AudioSamples = 0;

static uint64_t Fnv1a(const void* data, size_t len, uint64_t h = 0xcbf29ce484222325ull)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static bool WritePNG(const std::string& path, const uint32_t* pixels, int w, int h)
{
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++)
    {
        rgb[i * 3 + 0] = (pixels[i] >> 16) & 0xFF;
        rgb[i * 3 + 1] = (pixels[i] >> 8) & 0xFF;
        rgb[i * 3 + 2] = pixels[i] & 0xFF;
    }
#ifdef SSR_NO_PNG
    // No libpng in this build (the Switch): a 24-bit BMP of the same name instead.
    std::string bmp = path;
    if (bmp.size() > 4 && bmp.compare(bmp.size() - 4, 4, ".png") == 0) bmp.replace(bmp.size() - 4, 4, ".bmp");
    FILE* f = fopen(bmp.c_str(), "wb");
    if (!f) return false;
    const int stride = (w * 3 + 3) & ~3;
    const uint32_t dataSize = (uint32_t)stride * h, fileSize = 54 + dataSize;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, (uint32_t)w);
    put32(22, (uint32_t)h);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, dataSize);
    // (In one write: the SD card is slow with many small ones.)
    std::vector<uint8_t> file(54 + (size_t)dataSize, 0);
    memcpy(file.data(), hdr, 54);
    for (int y = h - 1; y >= 0; y--) // bottom row first, blue green red
    {
        const uint8_t* src = &rgb[(size_t)y * w * 3];
        uint8_t* row = &file[54 + (size_t)(h - 1 - y) * stride];
        for (int x = 0; x < w; x++)
        {
            row[x * 3 + 0] = src[x * 3 + 2];
            row[x * 3 + 1] = src[x * 3 + 1];
            row[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    const bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
    fclose(f);
    return ok;
#else
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    img.width = w;
    img.height = h;
    img.format = PNG_FORMAT_RGB;
    return png_image_write_to_file(&img, path.c_str(), 0, rgb.data(), 0, nullptr) != 0;
#endif
}

static std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
#ifdef __ANDROID__
    // Relative paths are files packed in the APK (the ROM); SDL reads both kinds.
    if (SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb"))
    {
        Sint64 len = SDL_RWsize(rw);
        if (len > 0)
        {
            data.resize((size_t)len);
            if (SDL_RWread(rw, data.data(), 1, (size_t)len) != (size_t)len) data.clear();
        }
        SDL_RWclose(rw);
    }
    return data;
#endif
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(len);
    if (fread(data.data(), 1, len, f) != (size_t)len) data.clear();
    fclose(f);
    return data;
}

static void FlushSave(const std::string& path)
{
    if (!Host::SaveIsDirty || path.empty()) return;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(Host::SaveData.data(), 1, Host::SaveData.size(), f) == Host::SaveData.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) return;
#ifdef __SWITCH__
    // The Switch's file system will not rename onto an existing file, so the old save steps
    // aside first, and comes back if the new one cannot take its place.
    const std::string old = path + ".old";
    remove(old.c_str());
    const bool hadOld = rename(path.c_str(), old.c_str()) == 0;
    if (rename(tmp.c_str(), path.c_str()) != 0)
    {
        if (hadOld) rename(old.c_str(), path.c_str());
        fprintf(stderr, "cannot write save %s\n", path.c_str());
        return;
    }
    if (hadOld) remove(old.c_str());
#else
    // std::filesystem::rename replaces an existing file on Windows too (plain rename()
    // fails there when the target exists, which kept every save after the first).
    std::error_code err;
    std::filesystem::rename(tmp, path, err);
    if (err)
    {
        fprintf(stderr, "cannot write save %s: %s\n", path.c_str(), err.message().c_str());
        return;
    }
#endif
    Host::SaveIsDirty = false;
}

struct Options
{
    std::string rom;
    std::string save;
    std::string script;
    std::string shotDir;
    std::set<uint32_t> shotAt;
    uint32_t shotEvery = 0;
    uint32_t hashEvery = 0;
    std::string nav;                     // test aid: walk to these tiles, "x,z;x,z;...", holding the script meanwhile
    bool navStopOnBattle = false;        // end --nav when a battle starts, so the script can play it
    uint32_t stopAt = 0;                 // real frame at which to quit, whatever the script clock says
    uint32_t navAfter = 0;               // script frame at which --nav starts
    std::vector<std::pair<int, std::string>> navMapIds; // grids of single interior maps
    std::string navMap;                  // permission grid from tools/navmap.py for --nav
    uint32_t posEvery = 0;               // test aid: print map and player tile every N frames
    uint32_t frames = 0;
    bool headless = false;
    bool noSave = false;
    bool recomp = true;
    bool lockstep = false;
    bool fullscreen = false;
    bool swapped = false;
    bool logSections = false;
    bool noWide = false;                 // turn the widescreen 3D renderer off
    std::string funcs;                   // function table for a game that is not recompiled (tools/bw2/funcmap.py); with --interp or --jit
    bool jit = false;                    // melonDS's JIT (a game that is not recompiled, built with SSR_JIT)
    bool forceField = false;             // test aid: treat every frame as the overworld (for games other than SoulSilver)
    bool benchCompose = false;           // headless: compose the merged picture every frame, to time it
    bool autoFocus = true;
    bool layoutGiven = false;
    bool audioStats = false;
    int interp60 = -1;                   // 3D frame interpolation: -1 = from settings (off when headless)
    int gpu3d = -1;                      // 3D on the graphics chip: -1 = from settings (off when headless)
    bool battleMash = false;             // test aid: tap A through battles, pausing the script meanwhile
    bool countChanges = false;           // report how many frames changed the top screen
    bool shotLayers = false;             // also write a per-pixel layer map with each screenshot
    std::set<uint32_t> traceFuncsAt;     // frames at which to list the functions that just ran
    std::string symbols = "gen/symbols.txt";
    std::string config;          // settings file
    std::string stateLoad;       // snapshot to resume from
    std::string stateSave;       // snapshot to write
    uint32_t stateSaveAt = 0;    // frame at which to write it
    int composeW = 0, composeH = 0; // screenshots use the single-display layout at this size
    int scale = 3;
    std::string layout = "merged";
    // Test runs (on the Switch from test.txt, see switch_platform.cpp):
    bool relative = false;       // --frames, --shot-at, --stop-at and the input script count from the snapshot's frame
    std::string timing;          // a line per frame with where its time went (CSV)
    std::vector<std::string> sets; // settings for this run only ("wide_lines=720"); the settings file is then not written
    bool keepSettings = false;   // never write the settings file
    bool dekoProbe = false;      // Switch: a test of deko3D when the window is up (frontend/dekoprobe.h)
    bool testRun = false;        // a run from test.txt: controllers and the pause menu ignored
    bool windowShots = true;     // in a window, screenshots also read back what was shown
    int wideGpu = -1;            // the widescreen world by the graphics chip: -1 = from settings
    int ds3dGpu = -1;            // with it, the DS-resolution 3D picture: -1 = from settings
};

static void Usage()
{
    fprintf(stderr,
        "usage: ssrecomp <rom.nds> [options]\n"
        "  --save <file>        save file (default: <rom>.sav)\n"
        "  --no-save            never write the save file\n"
        "  --layout <name>      merged | hybrid | side | vertical | top | bottom (default merged)\n"
        "  --config <file>      settings file (default: ssrecomp.cfg beside the save)\n"
        "  --scale <n>          window scale (default 3)\n"
        "  --fullscreen         start in fullscreen (F11 toggles)\n"
        "  --headless           no window or audio; run as fast as possible\n"
        "  --frames <n>         stop after n frames\n"
        "  --input <file>       scripted input\n"
        "  --swap               start with the bottom screen as the large one (hybrid)\n"
        "  --no-auto-focus      keep the top screen large instead of choosing automatically\n"
        "  --compose <WxH>      screenshots show the chosen layout at this size instead of\n"
        "                       the two raw screens stacked\n"
        "  --state-save <file> --state-at <frame>   write a snapshot at that frame\n"
        "  --state-load <file>  resume from a snapshot (frame numbering continues)\n"
        "  --relative           --frames, --shot-at, --stop-at and --input count from the snapshot's frame\n"
        "  --timing <file.csv>  a line per frame: where its time went (ms)\n"
        "  --set key=value      a setting for this run only (as in ssrecomp.cfg); settings are not saved\n"
        "  --keep-settings      never write the settings file\n"
        "  --deko-probe         Switch: test deko3D (the Switch's graphics API) at start, in the log\n"
        "  --test-run           ignore controllers and the pause menu (test runs from test.txt)\n"
        "  --no-window-shots    in a window, no window_*.png read back from the screen (only frame_*)\n"
        "  --env NAME=VALUE     set an environment variable (diagnostics such as SSR_WIDE_PROF=1)\n"
        "  --wide-gpu           the widescreen world drawn by the graphics chip (Switch: deko3D;\n"
        "                       elsewhere OpenGL, with --headless only, for tests; also SSR_WIDE_GPU=1)\n"
        "  --no-ds3d-gpu        with --wide-gpu, the DS-resolution 3D picture still by the processor\n"
        "  --shot-dir <dir>     where screenshots go\n"
        "  --shot-at <a,b,c>    screenshot these frames\n"
        "  --shot-every <n>     screenshot every n frames\n"
        "  --hash-every <n>     print a state hash every n frames\n"
        "  --log-sections       print which overlays run code and which merged scene is shown, on change\n"
        "  --interp60 / --no-interp60   force the 60 fps 3D interpolation on or off\n"
        "  --gpu3d / --no-gpu3d   draw the 3D scene on the graphics chip (OpenGL through EGL) or not\n"
        "  --battle-mash        while a battle is running, tap A and hold the input script (and the\n"
        "                       frame used by --state-at) still, so scripts can walk through grass\n"
        "  --count-changes      report on how many frames the top screen image changed\n"
        "  --audio-stats        print the peak and RMS level of the generated audio at exit\n"
        "  --shot-layers        with each screenshot, also write layers_<frame>.png colouring every\n"
        "                       pixel by the 2D layer it came from\n"

        "  --trace-funcs <a,b>  at these frames, write the names of all game functions entered in\n"
        "                       the last two frames to funcs_<frame>.txt in the screenshot directory\n"
        "  --symbols <file>     function list for --trace-funcs (default gen/symbols.txt)\n"
        "  --interp             run the ARM9 on the interpreter instead of recompiled code\n"
        "  --jit                run the ARM9 on melonDS's JIT (a game that is not recompiled;\n"
        "                       built with -DSSR_JIT=ON)\n"
        "  --funcs <file>       function table of a game that is not recompiled (Black 2:\n"
        "                       tools/bw2/funcmap.py); its layouts follow the functions that run\n"
        "  --lockstep           run interpreter and recompiled code side by side and compare\n"
        "                       every ARM9 instruction (headless only)\n"
        "  --log <0-3>          core log level (0 = everything)\n");
}

static bool ParseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--save") o.save = next();
        else if (a == "--no-save") o.noSave = true;
        else if (a == "--layout") { o.layout = next(); o.layoutGiven = true; }
        else if (a == "--config") o.config = next();
        else if (a == "--audio-stats") o.audioStats = true;
        else if (a == "--shot-layers") o.shotLayers = true;
        else if (a == "--count-changes") o.countChanges = true;
        else if (a == "--battle-mash") o.battleMash = true;
        else if (a == "--interp60") o.interp60 = 1;
        else if (a == "--no-interp60") o.interp60 = 0;
        else if (a == "--gpu3d") o.gpu3d = 1;
        else if (a == "--no-gpu3d") o.gpu3d = 0;

        else if (a == "--symbols") o.symbols = next();
        else if (a == "--trace-funcs")
        {
            std::istringstream ss(next());
            std::string tok;
            while (std::getline(ss, tok, ',')) o.traceFuncsAt.insert(strtoul(tok.c_str(), nullptr, 10));
        }
        else if (a == "--scale") o.scale = atoi(next().c_str());
        else if (a == "--headless") o.headless = true;
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--swap") o.swapped = true;
        else if (a == "--log-sections") o.logSections = true;
        else if (a == "--no-wide") o.noWide = true;
        else if (a == "--force-field") o.forceField = true;
        else if (a == "--funcs") o.funcs = next();
        else if (a == "--jit") o.jit = true;
        else if (a == "--bench-compose") o.benchCompose = true;
        else if (a == "--no-auto-focus") o.autoFocus = false;
        else if (a == "--state-save") o.stateSave = next();
        else if (a == "--state-at") o.stateSaveAt = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--state-load") o.stateLoad = next();
        else if (a == "--relative") o.relative = true;
        else if (a == "--timing") o.timing = next();
        else if (a == "--set") o.sets.push_back(next());
        else if (a == "--keep-settings") o.keepSettings = true;
        else if (a == "--deko-probe") o.dekoProbe = true;
        else if (a == "--test-run") o.testRun = true;
        else if (a == "--no-window-shots") o.windowShots = false;
        else if (a == "--wide-gpu") o.wideGpu = 1;
        else if (a == "--env")
        {
            // (For test runs from test.txt on the Switch, which cannot set the environment.)
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq != std::string::npos)
            {
#ifdef _WIN32
                _putenv_s(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
#else
                setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
#endif
            }
        }
        else if (a == "--no-wide-gpu") o.wideGpu = 0;
        else if (a == "--no-ds3d-gpu") o.ds3dGpu = 0;
        else if (a == "--compose") sscanf(next().c_str(), "%dx%d", &o.composeW, &o.composeH);
        else if (a == "--interp") o.recomp = false;
        else if (a == "--lockstep") { o.lockstep = true; o.headless = true; o.noSave = true; }
        else if (a == "--frames") o.frames = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--input") o.script = next();
        else if (a == "--shot-dir") o.shotDir = next();
        else if (a == "--shot-every") o.shotEvery = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--nav") o.nav = next();
        else if (a == "--nav-stop-on-battle") o.navStopOnBattle = true;
        else if (a == "--stop-at") o.stopAt = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--nav-after") o.navAfter = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--nav-map") o.navMap = next();
        else if (a == "--nav-map-id")
        {
            std::string v = next();
            size_t eq = v.find('=');
            if (eq != std::string::npos) o.navMapIds.push_back({ atoi(v.c_str()), v.substr(eq + 1) });
        }
        else if (a == "--pos-every") o.posEvery = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--hash-every") o.hashEvery = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--log") Host::LogLevel = atoi(next().c_str());
        else if (a == "--shot-at")
        {
            std::istringstream ss(next());
            std::string tok;
            while (std::getline(ss, tok, ',')) o.shotAt.insert(strtoul(tok.c_str(), nullptr, 10));
        }
        else if (a[0] == '-') return false;
        else o.rom = a;
    }
    return !o.rom.empty();
}

// The DS clock. The game lights the overworld by the time of day (dark blue at night,
// orange in the evening), so it follows the computer's clock as on a real DS; the tests
// use a fixed noon so their pictures do not depend on when they run.
// SSR_CLOCK="YYYY-MM-DD HH:MM" overrides it.
static void SetClock(NDS& nds, bool fixed)
{
    int y = 2026, mo = 1, d = 1, h = 12, mi = 0, sec = 0;
    if (const char* c = getenv("SSR_CLOCK"))
        sscanf(c, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
#ifdef __SWITCH__
    // (localtime() below gives UTC there: the console's own time zone is asked for instead.)
    else if (!fixed && SwitchPlatform::LocalTime(y, mo, d, h, mi, sec)) {}
#endif
    else if (!fixed)
    {
        time_t now = time(nullptr);
        struct tm t = *localtime(&now);
        y = t.tm_year + 1900; mo = t.tm_mon + 1; d = t.tm_mday;
        h = t.tm_hour; mi = t.tm_min; sec = t.tm_sec;
    }
    nds.RTC.SetDateTime(y, mo, d, h, mi, sec);
}

// SoulSilver's widened battle picture (battlewide.h) in two halves, where its BattleWide has
// them (ssrecomp-soulsilver.patch splits Build into Grab, a quick copy right after the frame, and
// Render, the slow part): with the pipeline, Render then runs beside the next frame's emulation
// instead of between frames. Without them (the shared code alone), Build as before. (Overloads
// chosen by whether the calls compile: the int one is preferred where it does.)
template <class B> static auto BattleGrab(B& bw, melonDS::NDS& nds, const ScreenImage& top, int extend, const WideImage* world,
    float viewWidth, int unitNum, int) -> decltype(bw.Grab(nds, top, extend, world, viewWidth, unitNum, true))
{
    return bw.Grab(nds, top, extend, world, viewWidth, unitNum, true); // (a copy of what it needs)
}
template <class B> static bool BattleGrab(B&, melonDS::NDS&, const ScreenImage&, int, const WideImage*, float, int, long)
{
    return false;
}
template <class B> static auto BattleRender(B& bw, WideImage& out, int) -> decltype(bw.Render(out), bool())
{
    bw.Render(out);
    return true;
}
template <class B> static bool BattleRender(B&, WideImage&, long) { return false; }
template <class B> static auto BattleSplit(B& bw, int) -> decltype(&B::Render, bool()) { return true; }
template <class B> static bool BattleSplit(B&, long) { return false; }

static bool UseJit = false;
static int WideLinesNow = 0; // the widescreen 3D's height this frame (see where it is set)
static bool UseFastMem = false; // the JIT's fast memory came on (MakeConsole)
// The pause menu's EMULATION row: on the Switch, in builds with melonDS's JIT (Black 2).
// (Not any more: FAST JIT is always used, with its automatic fallback for a start that fails
// and the interpreter when L is held. Set to 1 to bring the row back for testing.)
#define SSR_MENU_EMULATION 0

// Whether the 3D scene is drawn by the graphics chip (melonDS/src/GPU3D_GLThread.h) rather than
// by melonDS's software renderer on a thread of its own.
static bool UseGpu3D = false;
// A file that exists while the 3D on the graphics chip is on and has not yet drawn 600 frames:
// if it is there at the next start, that one hung or crashed (the whole Switch hung once), and
// the setting is turned off.
static std::string Gpu3DMarker;
// The same for the widescreen world on the graphics chip (GPU3D_WideGpu.h).
static std::string WideGpuMarker;
static uint32_t WideGpuFrames = 0;
// Whether the DS-resolution 3D picture is drawn by the graphics chip too (GPU3D_WideGpu.h's
// WideGpuDSRenderer), which goes with the widescreen world on it.
static bool UseDSGpu = false;
static void ApplyDSGpu(NDS& nds, bool on)
{
    if (on == UseDSGpu) return;
    melonDS::WideWait();
    if (on)
    {
        std::string info;
        auto r = melonDS::WideGpuMakeDS(info);
        if (!r)
        {
            fprintf(stderr, "[widegpu] the DS-resolution picture stays with the processor (%s)\n", info.c_str());
            return;
        }
        if (auto* soft = dynamic_cast<melonDS::SoftRenderer*>(&nds.GPU.GetRenderer3D())) soft->SetThreaded(false, nds.GPU);
        nds.GPU.GPU3D.SetCurrentRenderer(std::move(r));
        UseDSGpu = true;
        fprintf(stderr, "[widegpu] the DS-resolution 3D picture is drawn by the graphics chip too\n");
    }
    else
    {
        nds.GPU.GPU3D.SetCurrentRenderer(std::make_unique<melonDS::SoftRenderer>());
        UseDSGpu = false;
        if (!getenv("SSR_NO_RENDER_THREAD"))
            if (auto* soft = dynamic_cast<melonDS::SoftRenderer*>(&nds.GPU.GetRenderer3D())) soft->SetThreaded(true, nds.GPU);
        fprintf(stderr, "[widegpu] the DS-resolution 3D picture is drawn by the processor\n");
    }
}
// Whether the DS-resolution 3D picture goes to the graphics chip with the widescreen world.
static bool DSPictureOnGpu(bool headless)
{
#ifdef __SWITCH__
    return true;
#else
    return headless;
#endif
}
static bool StartWideGpu()
{
    if (!WideGpuMarker.empty())
        if (FILE* f = fopen(WideGpuMarker.c_str(), "wb"))
        {
            fclose(f);
            WideGpuFrames = 0;
        }
    std::string info;
    if (melonDS::WideGpuStart(info))
    {
        fprintf(stderr, "[widegpu] the widescreen world is drawn by the graphics chip (%s)%s%s\n",
            melonDS::WideGpuCurrent->Name(), info.empty() ? "" : ": ", info.c_str());
        return true;
    }
    fprintf(stderr, "[widegpu] the graphics chip could not be set up (%s); the processor draws the widescreen world\n",
        info.c_str());
    if (!WideGpuMarker.empty()) remove(WideGpuMarker.c_str());
    return false;
}
static uint32_t Gpu3DFrames = 0;

#ifdef SSR_GPU3D
// Each step of the 3D on the graphics chip's first frames, made to reach the log file at once.
static void Gpu3DTrace(const char* step)
{
    fprintf(stderr, "[gpu3d] %s\n", step);
    fflush(stderr);
#if defined(__SWITCH__) || defined(__linux__)
    fsync(fileno(stderr));
#endif
}
#endif

// Puts the chosen 3D renderer in place, between frames. The graphics chip's needs an OpenGL
// context (frontend/glcontext.cpp); without one the software renderer stays. The framebuffers
// stay as they are (GPU3D::SetCurrentRenderer, not GPU::SetRenderer3D, which makes new ones):
// the frame waiting to be shown points into them.
static void ApplyRenderer3D(NDS& nds, bool gpu)
{
#ifdef SSR_GPU3D
    if (gpu && !UseGpu3D)
    {
        melonDS::WideWait(); // (its threads read the software renderer's textures)
        melonDS::GLThreadRenderer::CreateContext = SsrCreateGLContext;
        melonDS::GLThreadRenderer::Enter = SsrEnterGLContext;
        melonDS::GLThreadRenderer::Leave = SsrLeaveGLContext;
        melonDS::GLThreadRenderer::Trace = Gpu3DTrace;
        if (!Gpu3DMarker.empty())
            if (FILE* f = fopen(Gpu3DMarker.c_str(), "wb"))
            {
                fclose(f);
                Gpu3DFrames = 0;
            }
        std::string info;
        auto r = melonDS::GLThreadRenderer::Start(info);
        if (r)
        {
            if (auto* soft = dynamic_cast<melonDS::SoftRenderer*>(&nds.GPU.GetRenderer3D()))
                soft->SetThreaded(false, nds.GPU);
            nds.GPU.GPU3D.SetCurrentRenderer(std::move(r));
            UseGpu3D = true;
            fprintf(stderr, "[gpu3d] on: %s\n", info.c_str());
            return;
        }
        fprintf(stderr, "[gpu3d] not available (%s): the 3D is drawn by the processor\n", info.c_str());
        if (!Gpu3DMarker.empty()) remove(Gpu3DMarker.c_str());
    }
    else if (!gpu && UseGpu3D)
    {
        melonDS::WideWait(); // (the graphics chip's thread may be drawing it)
        nds.GPU.GPU3D.SetCurrentRenderer(std::make_unique<melonDS::SoftRenderer>());
        UseGpu3D = false;
        if (!Gpu3DMarker.empty()) remove(Gpu3DMarker.c_str());
        fprintf(stderr, "[gpu3d] off\n");
    }
#else
    (void)gpu;
#endif
    // The DS-resolution 3D picture is drawn on its own thread, alongside the CPU emulation.
    if (!UseGpu3D && !getenv("SSR_NO_RENDER_THREAD"))
        if (auto* soft = dynamic_cast<melonDS::SoftRenderer*>(&nds.GPU.GetRenderer3D()))
            soft->SetThreaded(true, nds.GPU);
}

static std::unique_ptr<NDS> MakeConsole(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& sav, bool fixedClock)
{
    // Black 2 and White 2 (US) get 8 MB of main RAM, for their wider map (frontend/bw2map.h).
    const bool largeMainRAM = Bw2Map::WantsLargeRAM(rom);
    NDSCart::NDSCartArgs cartArgs;
    if (!sav.empty())
    {
        cartArgs.SRAM = std::make_unique<u8[]>(sav.size());
        memcpy(cartArgs.SRAM.get(), sav.data(), sav.size());
        cartArgs.SRAMLength = (u32)sav.size();
    }
    auto cart = NDSCart::ParseROM(rom.data(), (u32)rom.size(), nullptr, std::move(cartArgs));
    if (!cart)
    {
        fprintf(stderr, "ROM rejected by the core\n");
        return nullptr;
    }
    NDSArgs args;
    // melonDS's JIT only for a game that is not recompiled (--jit, Black 2).
    // SSR_JIT_NOFASTMEM=1: as on the Switch, where fast memory is not available, for testing.
    if (UseJit)
    {
        JITArgs jit;
        if (getenv("SSR_JIT_NOFASTMEM")) jit.FastMemory = false;
        args.JIT = jit;
    }
    else args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
#ifdef JIT_ENABLED
    UseFastMem = UseJit && nds->JIT.FastMemoryEnabled();
#endif
    nds->SetNDSCart(std::move(cart));
    nds->LargeMainRAM = largeMainRAM;
    nds->Reset();
    nds->SetupDirectBoot("game.nds");
    SetClock(*nds, fixedClock);
    nds->Start();
    return nds;
}

struct TraceState
{
    u32 r[16];
    u32 cpsr;
    s32 cycles;
    u64 ts;
};

static std::vector<TraceState> TraceLog;
static size_t TracePos = 0;
static uint32_t TraceFrame = 0;
static bool TraceFailed = false;
static u32 TracePrevR15 = 0, TracePrevCPSR = 0;

static void TraceCapture(ARMv5* cpu, TraceState& st)
{
    memcpy(st.r, cpu->R, sizeof(st.r));
    st.cpsr = cpu->CPSR;
    st.cycles = cpu->Cycles;
    st.ts = cpu->NDS.ARM9Timestamp;
}

static void TraceRecord(ARMv5* cpu, void*)
{
    TraceLog.emplace_back();
    TraceCapture(cpu, TraceLog.back());
}

static void TraceDump(const char* tag, const TraceState& st)
{
    fprintf(stderr, "  %s:", tag);
    for (int i = 0; i < 16; i++) fprintf(stderr, " r%d=%08x", i, st.r[i]);
    fprintf(stderr, " cpsr=%08x cycles=%d ts=%llu\n", st.cpsr, st.cycles, (unsigned long long)st.ts);
}

static void TraceCompare(ARMv5* cpu, void*)
{
    if (TraceFailed) return;
    TraceState now;
    memset(&now, 0, sizeof(now));
    TraceCapture(cpu, now);
    bool bad = TracePos >= TraceLog.size();
    if (!bad)
    {
        const TraceState& want = TraceLog[TracePos];
        bad = memcmp(want.r, now.r, sizeof(now.r)) != 0 || want.cpsr != now.cpsr ||
            want.cycles != now.cycles || want.ts != now.ts;
    }
    if (bad)
    {
        TraceFailed = true;
        bool thumb = (TracePrevCPSR & 0x20) != 0;
        fprintf(stderr, "LOCKSTEP MISMATCH frame %u instruction #%zu at %08x (%s) opcode %08x\n",
            TraceFrame, TracePos, TracePrevR15 - (thumb ? 2 : 4), thumb ? "thumb" : "arm", cpu->CurInstr);
        if (TracePos < TraceLog.size()) TraceDump("interp", TraceLog[TracePos]);
        else fprintf(stderr, "  interp: ran fewer instructions this frame (%zu)\n", TraceLog.size());
        TraceDump("recomp", now);
    }
    TracePos++;
    TracePrevR15 = cpu->R[15];
    TracePrevCPSR = cpu->CPSR;
}

// Decides which DS screen should be the large one on a single display.
//
// Two signals: a screen showing one flat colour has nothing to look at, and some
// applications (recognised by their per-frame function having just run) are driven
// from the bottom screen. A manual swap overrides the decision until it next changes.
struct AutoFocus
{
    int mode = 0;              // 0 automatic, 1 top screen large, 2 bottom screen large
    bool wantBottom = false;   // current automatic decision
    bool manualFlip = false;
    int pending = 0;           // frames the opposite decision has been stable
    std::vector<const RcApp*> bottomApps;

    void Init()
    {
        // Applications whose controls and content are on the bottom screen.
        static const char* names[] = {
            "Bag", "PartyMenu", "NamingScreen", "Pokegear", "GearConfigure", "GearMap", "GearPhone", "GearRadio",
            "TownMap", "PCBox", "EasyChat", "VoltorbFlip", "Pokedex", "ScratchOffCards", "AlphPuzzle",
            "TrainerCard", "TrainerCardSignature", "BugContestSwapMon", "WirelessTradeSelectMon", "OptionsMenu",
            "MainMenu", "CheckSave", "DeleteSave",
        };
        for (unsigned i = 0; i < rc_num_apps; i++)
            for (const char* n : names)
                if (!strcmp(rc_apps[i].name, n)) bottomApps.push_back(&rc_apps[i]);
    }

    static bool Blank(const u32* fb)
    {
        u32 c = fb[0] & 0xFFFFFF;
        for (int i = 1; i < 256 * 192; i++)
            if ((fb[i] & 0xFFFFFF) != c) return false;
        return true;
    }

    // appBottom: one of bottomApps is starting, running or ending (its probes, which see
    // the start and end too, unlike the check below).
    void Update(const u32* top, const u32* bot, bool fieldNow = false, bool appBottom = false)
    {
        bool topBlank = Blank(top), botBlank = Blank(bot);
        bool want = wantBottom;
        int hold = 12;
        if (topBlank && botBlank)
            ; // fade or load: keep whatever is showing
        else if (appBottom && !fieldNow)
        {
            // Straight to the bottom screen, even while it is still dark and the other one
            // is not: an application loading draws one screen before the other, and
            // following that showed the top screen first and then switched.
            want = true;
            hold = 1;
        }
        else if (fieldNow && !topBlank)
        {
            // Back in the overworld: the application that was open is over, even though
            // its code ran a few frames ago.
            want = false;
            hold = 1;
        }
        else if (topBlank)
        {
            // A top screen of one colour is also what a flash or fade looks like for a
            // moment, so this has to last before the bottom screen is shown instead.
            want = true;
            hold = 45;
        }
        else if (botBlank)
            want = false;
        else
        {
            want = false;
            for (const RcApp* app : bottomApps)
                if (rc_ran_recently(app->section, app->execAddr, 6)) want = true;
        }
        if (want == wantBottom)
        {
            pending = 0;
            return;
        }
        // Require the new state to hold briefly so transitions do not flicker.
        if (++pending >= hold)
        {
            wantBottom = want;
            manualFlip = false;
            pending = 0;
        }
    }

    bool Swapped() const
    {
        bool base = mode == 0 ? wantBottom : mode == 2;
        return base != manualFlip;
    }
};

// Software version of what the window shows, for screenshots.
static void Compose(const Layout& layout, int w, int h, const u32* top, const u32* bot, std::vector<uint32_t>& out)
{
    out.assign((size_t)w * h, 0);
    SDL_Rect rects[2];
    LayoutRects(layout, w, h, rects);
    int first = LayoutDrawFirst(layout);
    for (int n = 0; n < 2; n++)
    {
        int i = n == 0 ? first : 1 - first;
        const SDL_Rect& r = rects[i];
        const u32* src = i == 0 ? top : bot;
        if (r.w <= 0 || r.h <= 0) continue;
        for (int y = 0; y < r.h; y++)
        {
            int dy = r.y + y;
            if (dy < 0 || dy >= h) continue;
            const u32* row = src + (y * 192 / r.h) * 256;
            for (int x = 0; x < r.w; x++)
            {
                int dx = r.x + x;
                if (dx >= 0 && dx < w) out[(size_t)dy * w + dx] = row[x * 256 / r.w];
            }
        }
    }
}

// DS buttons from the keyboard (the keys in CONTROLS, Controls::DefaultKey to start with).
static uint32_t KeyboardKeys(const Controls::Bindings& b)
{
    const Uint8* st = SDL_GetKeyboardState(nullptr);
    static const int ds[12] = { KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_START, KEY_SELECT,
                                KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT }; // (Controls::A to RIGHT)
    uint32_t k = 0;
    for (int a = Controls::A; a <= Controls::RIGHT; a++)
        if (st[b.Key(a)]) k |= 1 << ds[a];
    // (Backspace is SELECT too, unless it was given to something.)
    if (st[SDL_SCANCODE_BACKSPACE] && b.ActionOfKey(SDL_SCANCODE_BACKSPACE) < 0) k |= 1 << KEY_SELECT;
    return k;
}

// The controllers in use: all of them at once, as which one is first is not to be relied on
// (Windows often lists a virtual or idle one first: Steam's, DS4Windows', a wheel's). Those
// SDL knows the layout of are game controllers; any other joystick (a generic USB pad) is
// read with the usual layout: the first buttons as A B X Y, L R, Select Start, the hat and
// the first two axes as the D-pad and the left stick.
struct Pads
{
    std::vector<SDL_GameController*> controllers;
    std::vector<SDL_Joystick*> joysticks;
    // Per joystick, whether its first two axes rest in the middle (1), not (2) or not known
    // yet (0, until half a second after it was opened, when its state has surely arrived):
    // some keyboards, mice and wheels report themselves as joysticks with an axis resting at
    // an end, which would walk the player on its own. And when it was opened.
    std::vector<int> axesUsable;
    std::vector<Uint32> openedAt;

    bool Has(SDL_JoystickID id) const
    {
        for (SDL_GameController* c : controllers)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) return true;
        for (SDL_Joystick* j : joysticks)
            if (SDL_JoystickInstanceID(j) == id) return true;
        return false;
    }
    bool Empty() const { return controllers.empty() && joysticks.empty(); }

    // Opens the device at `index` unless already open; its name, or null.
    const char* Open(int index)
    {
        if (getenv("SSR_NO_PAD") || Has(SDL_JoystickGetDeviceInstanceID(index))) return nullptr; // tests must not hear a real controller
        if (SDL_IsGameController(index))
        {
            SDL_GameController* c = SDL_GameControllerOpen(index);
            if (!c)
            {
                fprintf(stderr, "controller %d could not be opened: %s\n", index, SDL_GetError());
                return nullptr;
            }
            controllers.push_back(c);
            const char* name = SDL_GameControllerName(c);
            fprintf(stderr, "controller: %s\n", name ? name : "?");
            return name ? name : "";
        }
#ifdef SSR_PHONE
        // Android reports some of a device's own keys as a joystick (and iOS its motion sensor),
        // and using one hides the on-screen controls.
        return nullptr;
#endif
        SDL_Joystick* j = SDL_JoystickOpen(index);
        if (!j)
        {
            fprintf(stderr, "joystick %d could not be opened: %s\n", index, SDL_GetError());
            return nullptr;
        }
        // Not one without buttons (an accelerometer, a wheel's pedals alone).
        if (SDL_JoystickNumButtons(j) < 2)
        {
            SDL_JoystickClose(j);
            return nullptr;
        }
        joysticks.push_back(j);
        axesUsable.push_back(SDL_JoystickNumAxes(j) >= 2 ? 0 : 2);
        openedAt.push_back(SDL_GetTicks());
        const char* name = SDL_JoystickName(j);
        fprintf(stderr, "joystick without a known layout: %s (%d buttons, %d axes, %d hats)\n", name ? name : "?",
            SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j));
        return name ? name : "";
    }

    // Closes the device with instance `id`; whether it was open.
    bool Close(SDL_JoystickID id)
    {
        for (size_t i = 0; i < controllers.size(); i++)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[i])) == id)
            {
                SDL_GameControllerClose(controllers[i]);
                controllers.erase(controllers.begin() + i);
                return true;
            }
        for (size_t i = 0; i < joysticks.size(); i++)
            if (SDL_JoystickInstanceID(joysticks[i]) == id)
            {
                SDL_JoystickClose(joysticks[i]);
                joysticks.erase(joysticks.begin() + i);
                axesUsable.erase(axesUsable.begin() + i);
                openedAt.erase(openedAt.begin() + i);
                return true;
            }
        return false;
    }
};

static PadState ReadPad(Pads& pads)
{
    PadState st;
    // Each button pressed on any of them; each stick as far as on the one pushed furthest.
    auto merge = [](float& to, float v) { if (fabsf(v) > fabsf(to)) to = v; };
    for (SDL_GameController* pad : pads.controllers)
    {
        for (int i = 0; i < PAD_COUNT; i++)
        {
            int from = i;
#ifdef __SWITCH__
            // SDL on the Switch maps its own controllers by Xbox positions: its "A" is the
            // bottom button, which the Switch labels B (and its "X" the left one, labelled Y).
            // Everything here takes PAD_A to be the button printed A, so the pairs are swapped back.
            static const int labelled[] = { SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A,
                                            SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_X };
            if (i <= PAD_Y) from = labelled[i];
#endif
            st.btn[i] |= SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)from) != 0;
        }
        auto axis = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(pad, a) / 32767.0f; };
        merge(st.lx, axis(SDL_CONTROLLER_AXIS_LEFTX));
        merge(st.ly, axis(SDL_CONTROLLER_AXIS_LEFTY));
        merge(st.rx, axis(SDL_CONTROLLER_AXIS_RIGHTX));
        merge(st.ry, axis(SDL_CONTROLLER_AXIS_RIGHTY));
        merge(st.lt, axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        merge(st.rt, axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    }
    for (size_t k = 0; k < pads.joysticks.size(); k++)
    {
        SDL_Joystick* j = pads.joysticks[k];
        for (int i = 0; i < SDL_JoystickNumButtons(j); i++)
            if (const int in = Controls::JoyButtonInput(i); in >= 0) st.btn[in] |= SDL_JoystickGetButton(j, i) != 0;
        if (SDL_JoystickNumHats(j) > 0)
        {
            const Uint8 h = SDL_JoystickGetHat(j, 0);
            st.btn[PAD_UP] |= (h & SDL_HAT_UP) != 0;
            st.btn[PAD_DOWN] |= (h & SDL_HAT_DOWN) != 0;
            st.btn[PAD_LEFT] |= (h & SDL_HAT_LEFT) != 0;
            st.btn[PAD_RIGHT] |= (h & SDL_HAT_RIGHT) != 0;
        }
        int& usable = pads.axesUsable[k];
        if (usable == 0 && SDL_GetTicks() - pads.openedAt[k] >= 500)
            usable = (abs(SDL_JoystickGetAxis(j, 0)) < 8000 && abs(SDL_JoystickGetAxis(j, 1)) < 8000) ? 1 : 2;
        if (usable == 1)
        {
            merge(st.lx, SDL_JoystickGetAxis(j, 0) / 32767.0f);
            merge(st.ly, SDL_JoystickGetAxis(j, 1) / 32767.0f);
        }
    }
    return st;
}

// The controller as CONTROLS has it: each action's button (Controls::Bindings) pressed puts the
// button that action has as it comes (Controls::DefaultPad) down, so the game, the menus and
// START opening the menu all follow the new layout. A trigger given to a button is pressed past
// half way; FAST FORWARD given to a button holds the right trigger right down. The left trigger
// (the touch cursor's press) and the left stick's click (the next layout) stay where they are
// unless an action took them.
static PadState RemapPad(const PadState& p, const Controls::Bindings& b, bool byLabel)
{
    if (!b.PadCustom()) return p;
    auto on = [&](int in) { return in < PAD_COUNT ? p.btn[in] : in == Controls::IN_LT ? p.lt > 0.5f : p.rt > 0.5f; };
    PadState o = p;
    for (bool& x : o.btn) x = false;
    bool used[Controls::IN_COUNT] = {};
    for (int a = 0; a < Controls::COUNT; a++)
    {
        const int in = b.Pad(a, byLabel);
        if (in < 0) continue;
        used[in] = true;
        if (a == Controls::FAST) o.rt = in == Controls::IN_RT ? p.rt : on(in) ? 1.0f : 0.0f;
        else o.btn[Controls::DefaultPad(a, byLabel)] |= on(in);
    }
    o.btn[PAD_LS] = !used[Controls::IN_LS] && p.btn[PAD_LS];
    o.lt = used[Controls::IN_LT] ? 0.0f : p.lt;
    return o;
}

// The input first pressed in `now` that was not in `before` (Controls::IN_*), or -1: what is
// pressed for a binding in CONTROLS.
static int NewPadInput(const PadState& now, const PadState& before)
{
    for (int i = 0; i < PAD_COUNT; i++)
        if (now.btn[i] && !before.btn[i]) return i;
    if (now.lt > 0.5f && before.lt <= 0.5f) return Controls::IN_LT;
    if (now.rt > 0.5f && before.rt <= 0.5f) return Controls::IN_RT;
    return -1;
}

// DS buttons from the controller. With byLabel the button printed "A" is A on any pad;
// otherwise the DS diamond is mapped by position (right = A, bottom = B).
static uint32_t PadKeys(const PadState& p, bool byLabel)
{
    uint32_t k = 0;
    if (p.btn[byLabel ? PAD_A : PAD_B]) k |= 1 << KEY_A;
    if (p.btn[byLabel ? PAD_B : PAD_A]) k |= 1 << KEY_B;
    if (p.btn[byLabel ? PAD_X : PAD_Y]) k |= 1 << KEY_X;
    if (p.btn[byLabel ? PAD_Y : PAD_X]) k |= 1 << KEY_Y;
    if (p.btn[PAD_LB]) k |= 1 << KEY_L;
    if (p.btn[PAD_RB]) k |= 1 << KEY_R;
    if (p.btn[PAD_START]) k |= 1 << KEY_START;
    if (p.btn[PAD_BACK]) k |= 1 << KEY_SELECT;
    if (p.btn[PAD_UP] || p.ly < -0.5f) k |= 1 << KEY_UP;
    if (p.btn[PAD_DOWN] || p.ly > 0.5f) k |= 1 << KEY_DOWN;
    if (p.btn[PAD_LEFT] || p.lx < -0.5f) k |= 1 << KEY_LEFT;
    if (p.btn[PAD_RIGHT] || p.lx > 0.5f) k |= 1 << KEY_RIGHT;
    return k;
}

// Settings that persist between runs.
struct Settings
{
    std::string layout = "merged";
    int focusMode = 0;
    bool fullscreen = false;
    bool smooth = false;
    bool wide = true;   // widescreen 3D overworld
    // Black, White, Black 2 and White 2: more of the map loaded around the player, so the wide
    // view does not run out of land at its sides (frontend/bw2map.h). Read when the game starts.
    bool wideMap = true;
    int wideLines = 720; // height the widescreen 3D is drawn at, at most
    // Fewer of those lines while the drawing cannot keep up: off unless turned on (LOWER DETAIL /
    // RES WHEN BUSY). Kept as busy_detail: the auto_detail an older version saved was on by default.
    bool autoDetail = false;
    bool byLabel = true;
    bool fps60 = true;
    // Right stick as a touch cursor, left trigger as the press (the right one fast forwards),
    // and the stick clicks that switch screens. Off unless asked for: brushing the stick put
    // the bottom screen up.
    bool padTouch = false;
    bool showFps = false; // frames shown per second, in a corner
    bool cheatDynamicWidth = true;
    int cheatTextSize = 10; // tenths of the usual menu text size (0.1X to 5X)
    // A controller's START (+ on the Switch) opens the pause menu instead of being the DS's
    // START, which the games hardly use (START BUTTON: PAUSE MENU or DS START).
    bool startMenu = true;
    // While fast forwarding, the widescreen world at no more than 360 lines (more frames a
    // second, so a faster fast forward where drawing it is the limit). Off on a PC (the PC
    // defaults below), where it is a setting: FAST FORWARD LOW RES.
    bool ffLowRes = true;
    bool cpuBoost = true; // Switch only: the processor's boost mode (see SwitchPlatform)
    bool jit = true;      // Switch, games that are not recompiled: melonDS's JIT (read at start-up)
    bool fastMem = true;  // ... and its fast memory (also read at start-up)
    bool cgearOff = true; // Black 2: answer its C-Gear question on starting (cgear_start=off|ask)
    bool pipeline = true; // Switch: show each frame while the next one is emulated (emuthread.h)
    bool frameAhead = true; // ... and one frame further ahead, shown at even times (`ahead`)
    // The 3D scene drawn by the graphics chip (melonDS/src/GPU3D_GLThread.h). Off unless asked
    // for: on the Switch, turning it on hung the whole console (Oct 24), so there it is only
    // read from ssrecomp.cfg ("gpu3d_test=1"), never offered in the menu or on by default. (The
    // key replaced "gpu3d", which the build that hung saved as 1.)
    bool gpu3d = false;
#if defined(SSR_DEKO3D) || defined(SSR_PC_LAUNCHER)
    bool wideGpu = true;         // the widescreen world drawn by the graphics chip (GPU3D_WideGpu.h)
#else
    bool wideGpu = false;
#endif
    bool ds3dGpu = true;         // with it, the DS-resolution 3D picture too
    // Black and Black 2: how a battle's buttons are shown beside the wide view (BATTLE BUTTONS):
    // 0 CORNER, the touch screen's panel in the lower right corner; 1 STRIP (Black only; CORNER
    // for Black 2), the buttons large in the message box's band; 2 PANEL, the touch screen's
    // panel as tall as the message box (Bw1Layouts::BUTTONS_*).
    int battleButtons = 1;
    // The PC all-games program: the window's size when it last closed (0: 1280x720, a wide view).
    int windowW = 0, windowH = 0;
    Controls::Bindings controls; // CONTROLS (controls.h): map_key_* and map_pad_* lines

    void Load(const std::string& path)
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line))
        {
            Apply(line);
        }
#ifdef __SWITCH__
        // The Switch's buttons are labelled where the DS's are, so labels and positions are
        // the same thing there (see M_BUTTONS).
        byLabel = true;
#endif
    }

    // One "key=value" line of the settings file.
    void Apply(const std::string& line)
    {
        {
            size_t eq = line.find('=');
            if (eq == std::string::npos) return;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (controls.Apply(k, v)) return;
            // "view" replaced the older "layout" key when the merged view became the default.
            if (k == "view") layout = v;
            else if (k == "large_screen") focusMode = v == "top" ? 1 : v == "bottom" ? 2 : 0;
            else if (k == "fullscreen") fullscreen = v == "1";
            else if (k == "smooth") smooth = v == "1";
            else if (k == "wide") wide = v == "1";
            else if (k == "wide_map") wideMap = v == "1";
            else if (k == "wide_lines") wideLines = std::clamp(atoi(v.c_str()), 192, 2160);
            else if (k == "busy_detail") autoDetail = v == "1";
            else if (k == "buttons") byLabel = v != "positions";
            else if (k == "fps60") fps60 = v == "1";
            else if (k == "pad_touch") padTouch = v == "1";
            else if (k == "show_fps") showFps = v == "1";
            else if (k == "cheat_dynamic_width") cheatDynamicWidth = v != "0";
            else if (k == "cheat_text_size") cheatTextSize = std::clamp(atoi(v.c_str()), 1, 50);
            else if (k == "ff_low_res") ffLowRes = v == "1";
            else if (k == "start_button") startMenu = v != "ds";
            else if (k == "cpu_boost") cpuBoost = v == "1";
            else if (k == "jit") jit = v == "1";
            else if (k == "fastmem") fastMem = v == "1";
            else if (k == "cgear_start") cgearOff = v != "ask";
            else if (k == "pipeline") pipeline = v != "0";
            else if (k == "frame_ahead") frameAhead = v != "0";
            else if (k == "gpu3d_test") gpu3d = v == "1";
            else if (k == "wide_gpu") wideGpu = v == "1";
            else if (k == "ds3d_gpu") ds3dGpu = v != "0";
            else if (k == "battle_buttons") battleButtons = v == "corner" ? 0 : v == "panel" ? 2 : 1;
            else if (k == "window")
            {
                const size_t x = v.find('x');
                if (x != std::string::npos)
                {
                    windowW = std::clamp(atoi(v.c_str()), 0, 16384);
                    windowH = std::clamp(atoi(v.c_str() + x + 1), 0, 16384);
                }
            }
        }
    }

    bool readOnly = false; // a test run's own settings (--set): the file is left as it was

    void Save(const std::string& path) const
    {
        if (readOnly) return;
        std::ofstream f(path);
        f << "view=" << layout << "\n";
        f << "large_screen=" << (focusMode == 1 ? "top" : focusMode == 2 ? "bottom" : "auto") << "\n";
        f << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
        f << "smooth=" << (smooth ? 1 : 0) << "\n";
        f << "wide=" << (wide ? 1 : 0) << "\n";
        f << "wide_map=" << (wideMap ? 1 : 0) << "\n";
        f << "wide_lines=" << wideLines << "\n";
        f << "busy_detail=" << (autoDetail ? 1 : 0) << "\n";
        f << "buttons=" << (byLabel ? "labels" : "positions") << "\n";
        f << "fps60=" << (fps60 ? 1 : 0) << "\n";
        f << "pad_touch=" << (padTouch ? 1 : 0) << "\n";
        f << "show_fps=" << (showFps ? 1 : 0) << "\n";
        f << "cheat_dynamic_width=" << (cheatDynamicWidth ? 1 : 0) << "\n";
        f << "cheat_text_size=" << cheatTextSize << "\n";
        f << "ff_low_res=" << (ffLowRes ? 1 : 0) << "\n";
        f << "start_button=" << (startMenu ? "menu" : "ds") << "\n";
        f << "battle_buttons=" << (battleButtons == 0 ? "corner" : battleButtons == 2 ? "panel" : "strip") << "\n";
        controls.Save(f);
#ifdef SSR_PC_LAUNCHER
        if (windowW > 0 && windowH > 0) f << "window=" << windowW << "x" << windowH << "\n";
#endif
#ifdef SSR_GPU3D
        f << "gpu3d_test=" << (gpu3d ? 1 : 0) << "\n";
        f << "wide_gpu=" << (wideGpu ? 1 : 0) << "\n";
        f << "ds3d_gpu=" << (ds3dGpu ? 1 : 0) << "\n";
#endif
#ifdef __SWITCH__
        f << "cpu_boost=" << (cpuBoost ? 1 : 0) << "\n";
        f << "jit=" << (jit ? 1 : 0) << "\n";
        f << "fastmem=" << (fastMem ? 1 : 0) << "\n";
        f << "cgear_start=" << (cgearOff ? "off" : "ask") << "\n";
        f << "pipeline=" << (pipeline ? 1 : 0) << "\n";
        f << "frame_ahead=" << (frameAhead ? 1 : 0) << "\n";
#endif
    }
};

static const char* LayoutName(LayoutMode m)
{
    switch (m)
    {
    case LayoutMode::Merged: return "merged";
    case LayoutMode::Hybrid: return "hybrid";
    case LayoutMode::Side: return "side";
    case LayoutMode::Vertical: return "vertical";
    case LayoutMode::Top: return "top";
    default: return "bottom";
    }
}

// Draws the overlay canvas over an ARGB image of the same size.
static void BlendCanvas(const Canvas& c, std::vector<uint32_t>& img)
{
    for (size_t i = 0; i < img.size() && i < c.px.size(); i++)
    {
        uint32_t o = c.px[i];
        uint32_t a = o >> 24;
        if (!a) continue;
        if (a == 255)
        {
            img[i] = o;
            continue;
        }
        uint32_t d = img[i];
        uint32_t r = (((o >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
        uint32_t g = (((o >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
        uint32_t b2 = ((o & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
        img[i] = 0xFF000000 | (r << 16) | (g << 8) | b2;
    }
}

static bool WriteSnapshot(NDS& nds, const std::string& path, uint32_t frame)
{
    Savestate st;
    if (!nds.DoSavestate(&st) || st.Error) return false;
    st.Finish();
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(&frame, 4, 1, f);
    fwrite(st.Buffer(), 1, st.Length(), f);
    fclose(f);
    return true;
}

// Snapshot file: 4-byte frame number, then the core's savestate.
static bool ReadSnapshot(NDS& nds, const std::string& path, uint32_t* frame)
{
    std::vector<uint8_t> blob = ReadFile(path);
    if (blob.size() <= 4) return false;
    Savestate st(blob.data() + 4, (u32)blob.size() - 4, false);
    if (st.Error || !nds.DoSavestate(&st)) return false;
    if (frame) memcpy(frame, blob.data(), 4);
    rc_reset();
    return true;
}

int main(int argc, char** argv)
{
    Options opt;
#ifdef __ANDROID__
    // Started from the app icon: the ROM is packed in the APK, the save and the settings
    // go in the app's own storage.
    std::vector<std::string> androidArgs;
    std::vector<char*> androidArgv;
    if (argc < 2)
    {
        const char* dir = SDL_AndroidGetInternalStoragePath();
        // Files the emulator core keeps by relative name go there too.
        if (dir) chdir(dir);
        // Messages go to log.txt (Android drops them otherwise), with the frame timing,
        // for bug reports: in the app's folder on the shared storage
        // (Android/data/io.github.clickingmaster.ssrecomp/files), which a file manager or
        // a PC over USB can open, or else beside the save.
        setenv("SSR_PERF", "1", 0);
#ifndef SSR_PC_LAUNCHER
        const char* ext = SDL_AndroidGetExternalStoragePath();
        if (!ext || !freopen((std::string(ext) + "/log.txt").c_str(), "w", stderr))
            freopen("log.txt", "w", stderr);
        setvbuf(stderr, nullptr, _IOLBF, 0);
        androidArgs = { "ssrecomp", "rom.nds", "--save", std::string(dir ? dir : ".") + "/soulsilver.sav", "--fullscreen" };
        for (auto& a : androidArgs) androidArgv.push_back(a.data());
        argc = (int)androidArgv.size();
        argv = androidArgv.data();
#endif
        // (The all-games app: no ROM inside; the game list (pc_launcher.cpp) finds the
        // player's ROMs and sends the log to game.log in the app's folder.)
    }
#endif
#ifdef __SWITCH__
    // Started from the Homebrew Menu: the ROM, save and settings are in the app's folder
    // on the SD card (sdmc:/switch/ssrecomp), and the log goes there too.
    if (!SwitchPlatform::Start(argc, argv)) return 1;
#endif
#ifdef SSR_PC_LAUNCHER
    // The PC all-games program started without arguments: the game list (pc_launcher.cpp).
    if (!PcLauncher::Start(argc, argv)) return 0;
#endif
    if (!ParseArgs(argc, argv, opt))
    {
        Usage();
        return 2;
    }
    if (opt.jit)
    {
        UseJit = true;
        opt.recomp = false;
    }
#ifdef __SWITCH__
    fprintf(stderr, "[switch] emulation: %s\n", UseJit ? "JIT" : opt.recomp ? "recompiled code" : "interpreter");
#endif
    if (opt.save.empty()) opt.save = opt.rom + ".sav";
#ifdef __SWITCH__
    {
        // A save left aside by FlushSave when the console went off between its two renames.
        const std::string old = opt.save + ".old";
        FILE* f = fopen(opt.save.c_str(), "rb");
        if (f) fclose(f);
        else if (rename(old.c_str(), opt.save.c_str()) == 0) fprintf(stderr, "[switch] save restored from %s\n", old.c_str());
    }
#endif

    std::vector<uint8_t> rom = ReadFile(opt.rom);
    FuncWatch::RomPath = opt.rom; // (for a function table that reads its first bytes from it)
    // Pokemon SoulSilver (US): its new game's first menu is skipped (see where keys are sent).
    // (HeartGold too: the same game where it matters here.)
    const bool soulSilverRom = rom.size() > 0x10 && (memcmp(&rom[0x0C], "IPGE", 4) == 0 || memcmp(&rom[0x0C], "IPKE", 4) == 0);
    if (rom.empty())
    {
        fprintf(stderr, "cannot read ROM: %s\n", opt.rom.c_str());
        return 1;
    }

    std::vector<uint8_t> sav = ReadFile(opt.save);
    fprintf(stderr, "ROM %s: %zu bytes; save %s: %zu bytes\n", opt.rom.c_str(), rom.size(), opt.save.c_str(), sav.size());
    auto nds = MakeConsole(rom, sav, opt.headless);
    if (!nds) return 1;
    // Cheats (cheats.h): from the cheats folder beside the saves, the ones that are on running
    // from the start. (Not in a lockstep test, which compares two runs of the game as it is.)
    Cheats::List cheats;
    const std::string gameCode = rom.size() >= 0x200 ? std::string((const char*)&rom[0x0C], 4) : std::string();
    if (!opt.lockstep && rom.size() >= 0x200)
    {
        cheats.Load(Cheats::FolderFor(opt.save), gameCode, rom.data());
        cheats.Apply(nds->AREngine);
        fprintf(stderr, "[cheats] %zu for %s in %s, %d on\n", cheats.items.size(), gameCode.c_str(), cheats.folder.c_str(), cheats.On());
        for (const std::string& src : cheats.sources) fprintf(stderr, "[cheats]   from %s\n", src.c_str());
    }
#ifdef __SWITCH__
    if (UseJit) fprintf(stderr, "[switch] fast memory: %s\n", UseFastMem ? "on" : "off");
#endif
    nds->ARM9.RecompEnable = opt.recomp;

    // Lockstep: a second console on the plain interpreter runs each frame first and
    // records the CPU state after every instruction; the recompiled console must match.
    std::unique_ptr<NDS> ref;
    if (opt.lockstep)
    {
        ref = MakeConsole(rom, sav, true);
        if (!ref) return 1;
        nds->ARM9.RecompEnable = true;
        rc_single = true;
        ref->ARM9.TraceHook = TraceRecord;
        nds->ARM9.TraceHook = TraceCompare;
    }
    // The cartridge has its own copy now; Black 2's ROM is about 300 MB, which counts on the Switch.
    rom.clear();
    rom.shrink_to_fit();

    std::vector<ScriptEvent> script;
    if (!opt.script.empty()) script = LoadScript(opt.script);

    // Function tracing: a probe on every known function.
    std::vector<std::pair<int, std::string>> traceProbes;
    // A game that is not recompiled: which of its functions run comes from a table and a hook
    // on the interpreter instead of recompiled-code probes.
    static FuncWatch funcWatch;
    const bool funcWatchOn = !opt.funcs.empty() && funcWatch.Load(opt.funcs);
    // A SoulSilver build without the recompiled code (tools/soulsilver/tables_only.py) started
    // without --jit or --interp: the interpreter, rather than calling code that is not there.
    if (nds->ARM9.RecompEnable && !ref && rc_num_chunks && !rc_chunks[0].fn)
    {
        fprintf(stderr, "[recomp] this build has no recompiled code: running on the interpreter\n");
        nds->ARM9.RecompEnable = opt.recomp = false;
    }
    // SoulSilver without its recompiled code (--interp, --jit): which of its functions and
    // overlays run, for the layouts, comes from the core's watch instead (runtime/recomp_rt.cpp).
    // With tables that hold no code of the game (tools/soulsilver/tables_only.py), the overlays'
    // code it compares is read from this ROM first, which tells SoulSilver from HeartGold too.
    if (!nds->ARM9.RecompEnable && !funcWatchOn && rc_num_chunks && rc_portable_needed())
    {
        NdsOverlays overlays;
        char code[4] = {};
        if (FILE* f = fopen(opt.rom.c_str(), "rb"))
        {
            if (fseek(f, 0x0C, SEEK_SET) != 0 || fread(code, 1, 4, f) != 4) memset(code, 0, 4);
            fclose(f);
        }
        if (overlays.Open(opt.rom))
            rc_portable_load(code, [](void* user, int id, unsigned* size) -> const unsigned char* {
                const NdsOverlays::Overlay* ov = static_cast<NdsOverlays*>(user)->Get((uint32_t)id);
                if (!ov) return nullptr;
                *size = (unsigned)ov->data.size();
                return ov->data.data();
            }, &overlays);
        else
            fprintf(stderr, "[recomp] cannot read the overlays of %s\n", opt.rom.c_str());
    }
    if (!nds->ARM9.RecompEnable && !funcWatchOn && rc_num_chunks) rc_watch_attach(*nds);
    // (Attached once the layouts' functions are known, below.)
    if (!opt.traceFuncsAt.empty() && !funcWatchOn)
    {
        std::ifstream f(opt.symbols);
        std::string sec, name;
        unsigned addr, size;
        while (f >> sec >> std::hex >> addr >> size >> name)
        {
            int p = rc_add_probe(sec.c_str(), addr);
            if (p >= 0) traceProbes.emplace_back(p, sec + ":" + name);
        }
        fprintf(stderr, "[trace] %zu function probes\n", traceProbes.size());
    }

    uint32_t frame = 0;
    if (!opt.stateLoad.empty())
    {
        for (NDS* n : { ref.get(), nds.get() })
        {
            if (n && !ReadSnapshot(*n, opt.stateLoad, &frame))
            {
                fprintf(stderr, "cannot load snapshot: %s\n", opt.stateLoad.c_str());
                return 1;
            }
        }
    }
    if (opt.relative && frame)
    {
        // (Test runs written without knowing which frame the snapshot was made at.)
        std::set<uint32_t> at;
        for (uint32_t f : opt.shotAt) at.insert(f + frame);
        opt.shotAt = at;
        if (opt.frames) opt.frames += frame;
        if (opt.stopAt) opt.stopAt += frame;
        if (opt.stateSaveAt) opt.stateSaveAt += frame;
        for (ScriptEvent& ev : script)
        {
            ev.start += frame;
            ev.end += frame;
        }
        fprintf(stderr, "[test] the snapshot is at frame %u; the run's frames count from there\n", frame);
    }
    // A snapshot carries the clock of when it was made; play goes on at the current time.
    if (!opt.headless || getenv("SSR_CLOCK")) SetClock(*nds, opt.headless);

    const size_t slash = opt.save.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "." : opt.save.substr(0, slash);
    if (opt.config.empty()) opt.config = dir + "/ssrecomp.cfg";
    const std::string quickState = opt.save + ".state";

    Settings cfg;
#ifdef __SWITCH__
    // The Switch's processor is several times slower than a PC's, so the first start leaves
    // out what costs the most: the widescreen world is drawn at 288 lines (192, the DS's own
    // sharpness, looks blocky on its screen) and the in-between frames are off. Both can be
    // changed in the pause menu, and are remembered.
    cfg.wideLines = 288;
    cfg.fps60 = false;
#ifdef SSR_DEKO3D
    // Black, Black 2 and White 2 (a function table) with the world drawn by the graphics chip
    // (WORLD ON GPU, on by default): measured at about 16 ms a frame at 720 lines with the
    // in-between frames on, so they start like that. (If the graphics chip cannot be used,
    // LOWER DETAIL WHEN BUSY brings the lines down where needed.)
    if (!opt.funcs.empty())
    {
        cfg.wideLines = 720;
        cfg.fps60 = true;
    }
#endif
#endif
#ifdef SSR_PC_LAUNCHER
    // The PC download's first start: borderless fullscreen, the world at 1080 lines (with WORLD
    // ON GPU; LOWER RES WHEN BUSY brings it down on a PC that cannot keep up) and 60 FPS.
    // (Its game list's settings page shows the same defaults: launcher.h, SettingsList.)
    cfg.fullscreen = true;
    cfg.wideLines = 1080;
    cfg.fps60 = true;
    cfg.ffLowRes = false; // (FAST FORWARD LOW RES: the resolution stays unless it is turned on)
#if defined(__ANDROID__) || defined(__APPLE__)
    // (The Android app and the Mac draw the world with the processor: 720 lines. A Mac's OpenGL
    // stops at 4.1, short of WORLD ON GPU's 4.3.)
    cfg.wideLines = 720;
#endif
#endif
    if (!opt.headless) cfg.Load(opt.config);
#ifdef __SWITCH__
    // Not in the Switch's pause menu any more, so what an older version saved is not kept: the
    // world on the graphics chip, one frame ahead, and the stick touch cursor (the right stick
    // moves it, ZL touches: docked there is no touch screen, and some questions take only a
    // touch, Platinum's first YES / NO). (A test run's "set" lines below can still change them.)
    cfg.wideGpu = true;
    cfg.frameAhead = true;
    cfg.padTouch = true;
#endif
    for (const std::string& kv : opt.sets) cfg.Apply(kv);
    if (!opt.sets.empty() || opt.keepSettings) cfg.readOnly = true;
#ifdef __SWITCH__
    // (No CPU BOOST any more: the system kept taking its boost mode away and the game dropped
    // frames. sys-clk sets the clocks; see README. A boost a saved setting asks for is ignored.)
    // One wide screen always: a layout or large screen saved by an older version is ignored.
    if (!opt.layoutGiven) cfg.layout = "merged";
    cfg.focusMode = 0;
#endif
    if (opt.layoutGiven || opt.headless) cfg.layout = opt.layout;
    if (opt.fullscreen) cfg.fullscreen = true;
    if (!opt.autoFocus) cfg.focusMode = 1;
    if (opt.headless) cfg.fps60 = false; // tests compare images with the plain renderer unless asked
    if (opt.interp60 >= 0) cfg.fps60 = opt.interp60 == 1;
    nds->GPU.GPU3D.InterpolationEnabled = cfg.fps60 && !opt.lockstep;
    // The 3D scene: by the graphics chip where chosen (not in headless tests unless asked),
    // otherwise by the processor on a thread of its own. If the graphics chip was on last time
    // and the game never got 600 frames into it (see Gpu3DMarker), it is turned off.
    bool gpu3dWanted = opt.gpu3d >= 0 ? opt.gpu3d == 1 : (cfg.gpu3d && !opt.headless && !opt.lockstep);
#ifdef SSR_GPU3D
    if (opt.gpu3d < 0 && !opt.headless)
    {
        Gpu3DMarker = opt.config + ".gpu3d-starting";
        if (FILE* f = fopen(Gpu3DMarker.c_str(), "rb"))
        {
            fclose(f);
            remove(Gpu3DMarker.c_str());
            gpu3dWanted = cfg.gpu3d = false;
            cfg.Save(opt.config);
            fprintf(stderr, "[gpu3d] the 3D on the graphics chip did not keep going last time: "
                "turned off (3D ON GPU in the pause menu turns it back on)\n");
        }
    }
#endif
    ApplyRenderer3D(*nds, gpu3dWanted);

    // The widescreen world by the graphics chip (GPU3D_WideGpu.h): deko3D on the Switch;
    // elsewhere OpenGL, for comparing with the software rasteriser in headless tests (its
    // context is then current on this thread, which is the one that draws).
    {
        if (opt.wideGpu < 0 && getenv("SSR_WIDE_GPU")) opt.wideGpu = atoi(getenv("SSR_WIDE_GPU")) ? 1 : 0;
        bool want = opt.wideGpu >= 0 ? opt.wideGpu == 1 : cfg.wideGpu;
#if !defined(__SWITCH__) && !SSR_WORLD_ON_GPU
        want = want && opt.headless;
#endif
#ifdef SSR_GPU3D
        if (want && UseGpu3D) want = false; // (the 3D on the graphics chip draws the wide view itself)
        melonDS::GLThreadRenderer::CreateContext = SsrCreateGLContext;
        melonDS::WideGpuGLGetProc = SsrGLGetProc;
#ifndef __SWITCH__
        // In play its drawing goes on a thread of its own (the headless tests keep everything on
        // their one thread).
        melonDS::WideGpuGLThreaded = !opt.headless;
        melonDS::WideGpuGLRelease = SsrReleaseGLContext;
#endif
#endif
        // If it was on last time and the game never got 600 frames into it (a file made when it
        // starts and removed after that says so), it is turned off.
        if (opt.wideGpu < 0 && !opt.headless && !opt.testRun)
        {
            WideGpuMarker = opt.config + ".widegpu-starting";
            if (FILE* f = fopen(WideGpuMarker.c_str(), "rb"))
            {
                fclose(f);
                remove(WideGpuMarker.c_str());
                if (cfg.wideGpu)
                {
#ifdef __SWITCH__
                    // (For this start only: there is no menu row to turn it back on, and the
                    // next start tries again.)
                    want = false;
                    fprintf(stderr, "[widegpu] the world on the graphics chip did not keep going last time: "
                        "the processor draws it this time\n");
#else
                    want = cfg.wideGpu = false;
                    cfg.Save(opt.config);
                    fprintf(stderr, "[widegpu] the world on the graphics chip did not keep going last time: turned off "
                        "(WORLD ON GPU in the pause menu turns it back on)\n");
#endif
                }
            }
        }
        // (The DS-resolution picture too, on the Switch and in the tests; a PC's processor draws
        // that small picture in no time.)
        if (want && StartWideGpu() && cfg.ds3dGpu && opt.ds3dGpu != 0 && DSPictureOnGpu(opt.headless)) ApplyDSGpu(*nds, true);
    }

    // SSR_PERF=1 prints where the time of a frame goes, averaged over 300 frames.
    const bool perfOn = getenv("SSR_PERF") != nullptr;
    // SSR_EMU_PROF=1 (--env SSR_EMU_PROF=1 in a test run) adds where the emulation's time went
    // (melonDS/src/SsrProf.h).
    SsrProf::On = perfOn && getenv("SSR_EMU_PROF") != nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* tex[2] = { nullptr, nullptr };
    SDL_Texture* overlayTex = nullptr;
    int overlayTexW = 0, overlayTexH = 0;
    SDL_AudioDeviceID audio = 0;
    Pads pads;
    Layout layout = LayoutFromName(cfg.layout);
    const uint8_t* singleScreenFlag = nullptr; // set by the ROM: nonzero while a scene needs both screens
    LayoutMode preSingleLayout = LayoutMode::Merged;
    uint32_t singleFieldFrame = 0; // the last frame the overworld ran in a single-screen build
    bool singleScreenRom = false; // a SINGLE_SCREEN build of the decompilation (see the main loop)
    AutoFocus focus;
    focus.mode = cfg.focusMode;
    focus.manualFlip = opt.swapped;
    focus.Init();
    layout.swapped = focus.Swapped();

    auto applySmooth = [&]() {
        for (int i = 0; i < 2; i++)
            if (tex[i]) SDL_SetTextureScaleMode(tex[i], cfg.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    };

    if (!opt.headless)
    {
#ifdef SSR_PHONE
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        // Fingers are handled one by one (on-screen controls and the touch screen).
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        // Tilting the phone must not move the player.
        SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
        // Controllers come through Android's own input, which knows every gamepad. With SDL's
        // HIDAPI drivers on, a pad one of them claims (a Backbone One, a PlayStation pad) is
        // left out of Android's list, and is lost if SDL cannot open it over USB itself.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
#endif
#ifdef SSR_IOS
        // The home bar at the bottom fades away, and a swipe up from the bottom edge (where the
        // DS's SELECT and START are) needs a second swipe to leave the game.
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
#endif
#ifdef __SWITCH__
        // The touch screen is read from its finger events (see switchFinger), not as a mouse.
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
        // The controller keeps working while another window has the focus (on Windows SDL
        // otherwise ignores it then, as when the game was started from the setup window).
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        #ifdef _WIN32
        // The program's own icon (resource 1, when it was built with one) for its windows.
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        #endif
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        int w, h;
        LayoutNativeSize(layout, w, h);
        w *= opt.scale;
        h *= opt.scale;
        const char* windowTitle = "Pokemon SoulSilver";
#ifdef SSR_PC_LAUNCHER
        // The PC all-games program: a wide window (the widescreen view follows the window's
        // shape), as large as it was last time, named after the game.
        w = cfg.windowW >= 320 && cfg.windowH >= 240 ? cfg.windowW : 1280;
        h = cfg.windowW >= 320 && cfg.windowH >= 240 ? cfg.windowH : 720;
        static std::string gameTitle;
        gameTitle = PcLauncher::GameName();
        if (!gameTitle.empty()) windowTitle = gameTitle.c_str();
#endif
        window = SDL_CreateWindow(windowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            w, h,
            SDL_WINDOW_RESIZABLE |
#ifdef __APPLE__
            SDL_WINDOW_ALLOW_HIGHDPI |
#endif
            (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        // The OpenGL ES renderer uploads the picture several times faster than SDL's default
        // (desktop OpenGL) on the machines this was measured on. SDL_RENDER_DRIVER overrides.
        // Not on Windows: there OpenGL ES can come with a framebuffer that converts to sRGB,
        // which washes the colours out (seen on AMD), and SDL's default is Direct3D anyway.
#if defined(__APPLE__)
        if (!getenv("SDL_RENDER_DRIVER")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#elif !defined(_WIN32)
        if (!getenv("SDL_RENDER_DRIVER")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#endif
        Uint32 rendererFlags = SDL_RENDERER_ACCELERATED;
#if defined(__APPLE__)
        // A Mac: its pictures handed over in step with the screen (Metal's display sync), so no
        // tearing in fullscreen, where macOS can send them to the screen without its compositor.
        // The frames keep their own pace (nextTick, 59.8 a second): one a little under the
        // screen's 60 never waits for it, with Metal's three pictures in flight.
        rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
#endif
        renderer = SDL_CreateRenderer(window, -1, rendererFlags);
#ifdef SSR_GPU3D
        // (Its OpenGL context, which the 3D on the graphics chip hands back after each turn.)
        if (renderer) SsrSetSdlContext(window, SDL_GL_GetCurrentContext());
#endif
        if (!renderer)
        {
            fprintf(stderr, "SDL accelerated renderer failed: %s; trying fallback\n", SDL_GetError());
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (!window || !renderer)
        {
            fprintf(stderr, "cannot create window: %s\n", SDL_GetError());
            return 1;
        }
        {
            // In every log: bug reports about the picture need to know this.
            SDL_RendererInfo info;
            if (SDL_GetRendererInfo(renderer, &info) == 0)
                fprintf(stderr, "SDL renderer %s, video driver %s\n", info.name, SDL_GetCurrentVideoDriver());
        }
        if (opt.dekoProbe)
        {
#if defined(__SWITCH__) && defined(SSR_DEKO3D)
            // (Beside SDL's OpenGL, which has drawn a frame by now.)
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
            DekoProbe::Run(1280, 720);
            DekoProbe::Run(960, 540);
#else
            fprintf(stderr, "[deko] not in this build (Switch builds with deko3d and uam only)\n");
#endif
        }
        for (int i = 0; i < 2; i++)
            tex[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 192);
        applySmooth();
        SDL_ShowCursor(cfg.fullscreen ? SDL_DISABLE : SDL_ENABLE);

        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);

        // For bug reports about controllers: what SDL sees, and what may hide a device from it
        // (Steam sets these variables for the games it starts; a container may lack udev).
        SDL_version sdlv;
        SDL_GetVersion(&sdlv);
        fprintf(stderr, "SDL %d.%d.%d, %d joystick device(s)\n", sdlv.major, sdlv.minor, sdlv.patch, SDL_NumJoysticks());
#ifdef __linux__
        if (access("/run/.containerenv", F_OK) == 0 || access("/.dockerenv", F_OK) == 0)
            fprintf(stderr, "running in a container\n");
#endif
        for (const char* var : {"SteamGameId", "SDL_GAMECONTROLLER_IGNORE_DEVICES", "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT",
                                "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "SDL_JOYSTICK_DISABLE_UDEV", "SDL_GAMECONTROLLERCONFIG"})
            if (const char* v = getenv(var))
                fprintf(stderr, "  %s=%.200s\n", var, v);
        for (int i = 0; i < SDL_NumJoysticks(); i++)
        {
            const char* name = SDL_JoystickNameForIndex(i);
            fprintf(stderr, "  device %d: %s, %04x:%04x, %s\n", i, name ? name : "?", SDL_JoystickGetDeviceVendor(i),
                SDL_JoystickGetDeviceProduct(i), SDL_IsGameController(i) ? "known layout" : "no known layout");
            pads.Open(i);
        }
    }

    // Functions whose activity selects the merged scene.
    auto focusProbe = [&](const char* name) {
        for (unsigned i = 0; i < rc_num_focus_syms; i++)
            if (!strcmp(rc_focus_syms[i].name, name))
                return rc_add_probe(rc_focus_syms[i].section, rc_focus_syms[i].execAddr);
        return -1;
    };
    const int probeStartMenu = focusProbe("Task_StartMenu");
    const int probeStartMenuWait = focusProbe("Task_StartMenu_WaitApp");
    const int probeBattleInput = focusProbe("BattleInput_CheckTouch");
    const int probeSave[] = { focusProbe("sub_0203D264"), focusProbe("sub_0203D288"), focusProbe("sub_0203D2CC"),
        focusProbe("sub_0203D2EC"), focusProbe("sub_0203D304"), focusProbe("TouchSaveApp_HandleSaveConfirmation") };
    const int probeChoice[] = { focusProbe("ov27_0225C434") };
    const int probeMart = focusProbe("ov31_0225D7A0");
    const int probeTouch[] = { focusProbe("TouchscreenHitbox_FindRectAtTouchHeld"), focusProbe("TouchscreenHitbox_FindRectAtTouchNew"),
        focusProbe("TouchscreenHitbox_FindHitboxAtTouchNew"), focusProbe("TouchscreenHitbox_FindHitboxAtTouchHeld"),
        focusProbe("TouchscreenHitbox_TouchHeldIsIn"), focusProbe("TouchscreenHitbox_TouchNewIsIn") };
    const int probeFieldMap = focusProbe("FieldMap_Main");
    // Writing the save: for the seconds this takes the game sits in its flash write loop
    // and the code of the screen it shows (the save question, the overworld) runs only now
    // and then, so nothing on screen changes but the probes go quiet.
    const int probeSaveWrite[] = { focusProbe("HandleWriteSaveAsync_NormalData"), focusProbe("HandleWriteSaveAsync_PCBoxes") };
    auto probeActive = [&](int probe) {
        unsigned last = rc_probe_last_frame(probe);
        return last && last + 3 >= rc_current_frame();
    };
    auto saveWriting = [&]() {
        for (int probe : probeSaveWrite)
        {
            unsigned last = rc_probe_last_frame(probe);
            if (last && last + 8 >= rc_current_frame()) return true;
        }
        return false;
    };
    // Whether the overworld was running when the save write began: it counts as running
    // until the write ends, so the widescreen picture does not drop out meanwhile.
    bool fieldHeldBySave = false;
    // The overworld's main function is not entered for stretches of about ten frames while
    // some scripts run (a nurse bowing, for one), so it gets a longer memory than the rest:
    // otherwise the widescreen picture and the menu layout drop out for those frames.
    // A single-screen build of the game shows one screen, the bottom one, and the merged pipeline
    // treats it as the "top" screen, so the widescreen 3D and the battle widening apply to it.
    auto singleView = [&]() { return singleScreenRom && !opt.layoutGiven && singleScreenFlag && *singleScreenFlag == 0; };
    auto topUnitOf = [&]() {
        const int normal = (nds->PowerControl9 & (1 << 15)) ? 0 : 1;
        return singleView() ? 1 - normal : normal;
    };
    auto fieldActive = [&]() {
        if (opt.forceField) return true; // test aid for other games: treat every frame as the overworld
        unsigned last = rc_probe_last_frame(probeFieldMap);
        return (last && last + 16 >= rc_current_frame()) || fieldHeldBySave;
    };
    // Coming back from the Bag, the Pokegear and so on, the game fades the overworld in with
    // the X menu already drawn, a little before the menu's task runs again. The strip it
    // puts along the bottom of the top screen gives that away; while this counts down, the
    // menu layout is used although the task's probe is still silent.
    int menuReturn = 0;
    // SoulSilver and HeartGold: the Running Shoes button. The game has it on the touch screen
    // beside its menu (x 182-255, y 89-130), but greys it out while the X menu is open, and the
    // single-screen view shows the touch screen only then. So its picture is kept from the last
    // frame it was seen (shoesPix, shoesOn: running on) and drawn next to the X menu, where Y or
    // a touch on it toggles it: the menu is closed (B), the button touched, and the menu opened
    // again (X), one step after another (shoesStep, from shoesStepAt).
    static const int SHOES_X = 182, SHOES_Y = 89, SHOES_W = 74, SHOES_H = 42;
    std::vector<uint32_t> shoesPix(SHOES_W * SHOES_H);
    bool shoesSeen = false, shoesOn = false;
    int shoesStep = 0;
    uint32_t shoesStepAt = 0;
    float shoesRect[4] = {}; // where it was last drawn, in the view's units (x, y, w, h); w 0: not shown
    // Frames in a row in which the battle's touch screen showed something that is neither
    // of the menus with a layout of their own nor the idle backdrop.
    int battleUnknown = 0;
    // The frame until which the battle strip stays up while its buttons are not recognised:
    // between the commands and the moves the game slides one set out and the other in, and
    // the strip would otherwise vanish for those frames, uncovering the message box.
    uint32_t battleStripUntil = 0;
    // The button chosen in a battle strip (see the input handling before the frame runs).
    int battleSel = 0;
    bool battleFightOnly = false; // the command panel has FIGHT alone (gen4BattleInput)
    // Per application, the overlay that holds its code when no other application's code is
    // there (-1 otherwise), and per section the frames on which it has run since it started
    // running (see where it is counted).
    std::vector<int> appOwnSection(rc_num_apps, -1);
    std::vector<unsigned> sectionStreak(rc_num_sections, 0);
    for (unsigned i = 0; i < rc_num_apps; i++)
    {
        if (!strcmp(rc_apps[i].section, "main") || !strcmp(rc_apps[i].section, "field")) continue;
        unsigned sharing = 0;
        for (unsigned j = 0; j < rc_num_apps; j++)
            if (!strcmp(rc_apps[j].section, rc_apps[i].section)) sharing++;
        if (sharing != 1) continue;
        for (u32 sec = 0; sec < rc_num_sections; sec++)
            if (!strcmp(rc_sections[sec].name, rc_apps[i].section)) appOwnSection[i] = (int)sec;
    }
    // Probes per game application, to know which one is running: on its per-frame function
    // and on the functions that start and end it, which run every frame while it loads and
    // fades in and while it fades out. Its layout then stays from the first frame to the
    // last instead of other layouts showing in between.
    std::vector<std::pair<const char*, int>> appProbes;
    std::vector<std::pair<const char*, int>> appMainProbes; // the per-frame functions alone
    for (unsigned i = 0; i < rc_num_apps; i++)
    {
        appProbes.emplace_back(rc_apps[i].name, rc_add_probe(rc_apps[i].section, rc_apps[i].execAddr));
        appMainProbes.push_back(appProbes.back());
        if (rc_apps[i].initAddr)
            appProbes.emplace_back(rc_apps[i].name, rc_add_probe(rc_apps[i].initSection, rc_apps[i].initAddr));
        if (rc_apps[i].exitAddr)
            appProbes.emplace_back(rc_apps[i].name, rc_add_probe(rc_apps[i].exitSection, rc_apps[i].exitAddr));
    }
    // The application's per-frame function alone, without its start and end: what the test
    // options that wait for a battle (--battle-mash, --nav-stop-on-battle) go by.
    auto appRunning = [&](const char* name) {
        for (auto& ap : appMainProbes)
            if (!strcmp(ap.first, name))
            {
                unsigned last = rc_probe_last_frame(ap.second);
                if (last && last + 6 >= rc_current_frame()) return true;
            }
        return false;
    };
    auto appActive = [&](const char* name) {
        for (auto& ap : appProbes)
            if (!strcmp(ap.first, name))
            {
                unsigned last = rc_probe_last_frame(ap.second);
                if (last && last + 6 >= rc_current_frame()) return true;
            }
        // Opened from the start menu, an application is prepared by the field with code of
        // its own overlay for a while before its start function runs: that counts too, when
        // the overlay is that application's alone and has kept running for a while
        // (the start menu calls into some overlays once in passing).
        const unsigned waitLast = rc_probe_last_frame(probeStartMenuWait);
        if (waitLast && waitLast + 3 >= rc_current_frame())
            for (unsigned i = 0; i < rc_num_apps; i++)
                if (!strcmp(rc_apps[i].name, name) && appOwnSection[i] >= 0 && sectionStreak[appOwnSection[i]] >= 8)
                    return true;
        return false;
    };

    auto sectionWithin = [&](const char* name, unsigned frames) {
        for (u32 i = 0; i < rc_num_sections; i++)
            if (!strcmp(rc_sections[i].name, name))
                return rc_section_last_frame[i] && rc_section_last_frame[i] + frames >= rc_current_frame();
        return false;
    };
    auto sectionActive = [&](const char* name) { return sectionWithin(name, 3); };

    // Whether an application driven from the bottom screen is starting, running or ending
    // (worked out each frame before the scene is chosen).
    bool appBottomNow = false;
    // The command strip's own cursor: d-pad presses seen by the frontend this frame, and the
    // selection they have moved it to (reset whenever the ROM's published selection changes).
    std::string stripNav; int stripSel = 0, stripPubSel = -1, stripPubMenu = 0;
    // Chooses what the single merged view shows this frame.
    // Pokemon Black 2 / White 2 (a game that is not recompiled): the layout follows which of the
    // game's own functions ran in the last few frames (FuncWatch, from the pokebw2
    // decompilation's names). A first set of screens; anything not recognised shows both screens
    // side by side, so nothing is ever hidden.
    struct Bw2Funcs
    {
        int field = -1, fieldMenu = -1, bag = -1, battle = -1;
        int battleCommand = -1, battleMove = -1, party = -1, trainerCard = -1, townMap = -1;
        int options = -1, summary = -1, dexList = -1, dexEntry = -1, box = -1;
        int startContinue = -1, confirmDialog = -1, startMain = -1, intro = -1, nameEntry = -1;
        int battleList = -1, battleBag = -1, battleChoice = -1, battleYesNo = -1;
        int setupSingle = -1, setupDouble = -1, setupTriple = -1, setupRotation = -1;
        int save = -1, saveWrite = -1;
        std::vector<int> levelUpWin; // the level-up window in a battle (BtlvCore_Wait*LevelUp*)
        int battleTarget = -1;       // choosing a move's target (double, triple, rotation battles)
        int evolution = -1;          // the evolution scene, which the battle fades out into
        // The bar of Poke Balls (each side's party) that slides in as a trainer battle starts,
        // across the places of the HP boxes: made, its task each frame, removed.
        int ballBarCreate = -1, ballBarTask = -1, ballBarDelete = -1;
        std::vector<int> weatherObj; // the weathers with particles (see Scene::weatherObj)
    } bw2;
    // Black 2's question on every start, whether to launch the C-Gear (see where keys are sent).
    int cgearStep = 0;
    bool cgearAsked = false;  // its YES / NO was up in the last frame
    bool cgearHidden = false; // the question is being answered unseen this frame
    Bw1Layouts bw1; // Pokemon Black and White's own layouts (frontend/bw1.h; nothing for other games)
    Bw2Map bw2map;  // Black 2: more of the map loaded beside the player (frontend/bw2map.h)
    PtFuncs pt;     // Pokemon Platinum's functions (frontend/platinum.h; nothing for other games)
    if (funcWatchOn)
    {
        std::vector<std::string> bw2Names;
        auto bw2Find = [&](const char* name) {
            bw2Names.push_back(name);
            return funcWatch.Find(name);
        };
        bw2.field = bw2Find("FieldmapProc_Update");     // the overworld, every frame
        bw2.fieldMenu = bw2Find("EventFieldMenu_Callback"); // the X menu over the overworld
        bw2.bag = bw2Find("ItemMenuProc_Main");         // the Bag
        bw2.battle = bw2Find("BtlMain_ProcMain");       // any battle
        bw2.battleCommand = bw2Find("BtlvCore_WaitCommandSelect"); // FIGHT / BAG / POKEMON / RUN
        bw2.battleMove = bw2Find("BtlvCore_WaitMoveSelect");       // the four moves
        bw2.party = bw2Find("PokeListProc_Main");       // the party
        bw2.trainerCard = bw2Find("TrainerCard_Main");
        bw2.townMap = bw2Find("TownMap_ProcMain");      // the town map and Fly
        bw2.options = bw2Find("ConfigPanel_Main");      // Options
        bw2.summary = bw2Find("PStatus_ProcMain");      // a Pokemon's summary
        bw2.dexList = bw2Find("ZknListSeq_Main");       // the Pokedex list
        bw2.dexEntry = bw2Find("ZukanDetail_Main");     // a Pokedex entry
        bw2.box = bw2Find("Box2Proc_Main");             // the PC's boxes
        bw2.startContinue = bw2Find("StartMenu_Continue"); // CONTINUE chosen on the start menu
        bw2.startMain = bw2Find("StartMenu_Main");      // the start menu (CONTINUE, NEW GAME, ...)
        bw2.intro = bw2Find("IntroProc_Main");          // a new game's introduction (the professor)
        bw2.nameEntry = bw2Find("NameEntry_ProcMain");  // entering a name (the keyboard)
        bw2.confirmDialog = bw2Find("ConfirmDialog_Update"); // a YES / NO question
        bw2.battleList = bw2Find("BPlistMain_Task");    // the party in a battle (POKEMON, or after fainting)
        bw2.battleBag = bw2Find("BBagMain_Task");       // the Bag in a battle
        bw2.battleChoice = bw2Find("BChoiceMain_Task"); // a choice on the touch screen in a battle
        bw2.battleYesNo = bw2Find("BtlvInput_YesNoTask"); // YES / NO in a battle ("Use next Pokemon?")
        bw2.setupSingle = bw2Find("BtlMain_SetupAloneSingle"); // which kind of battle starts
        bw2.setupDouble = bw2Find("BtlMain_SetupAloneDouble");
        bw2.setupTriple = bw2Find("BtlMain_SetupAloneTriple");
        bw2.setupRotation = bw2Find("BtlMain_SetupAloneRotation");
        bw2.save = bw2Find("EventSave_Callback");       // SAVE from the X menu, until it is done
        // Waits for the save memory: while the game writes its save it does nothing else, for
        // over a second, not even the overworld (see the save layout).
        bw2.saveWrite = bw2Find("CARD_WaitBackupAsync");
        bw2.battleTarget = bw2Find("BtlvCore_WaitTargetSelect");
        bw2.evolution = bw2Find("ShinkaDemo_Main");
        bw2.ballBarCreate = bw2Find("BtlvBGauge_Create");
        bw2.ballBarTask = bw2Find("BtlvBGauge_SlideTask");
        bw2.ballBarDelete = bw2Find("BtlvBGauge_Delete");
        for (const char* f : { "BtlvCore_WaitLevelUpWin", "BtlvCore_WaitLevelUpStats", "BtlvCore_WaitCloseLevelUpWin" })
            bw2.levelUpWin.push_back(bw2Find(f));
        // The weathers with particles, each running in its own overlay (75: snow, 76: rain, 77:
        // sandstorm; pokebw2's include/field/weather_*.h) while it fades in, lasts and fades out.
        for (const char* w : { "Flake", "Blizzard", "Flutter", "Speck", "Rain", "Thunder", "GustyRain", "Sandstorm" })
            for (const char* phase : { "_Main", "_FadeIn", "_FadeOut" })
                bw2.weatherObj.push_back(bw2Find((std::string(w) + phase).c_str()));
        bw1.Init(opt.rom, funcWatch, bw2Names, opt.config); // Pokemon Black and White: their own functions too
        pt.Init(opt.rom, funcWatch, bw2Names);              // Pokemon Platinum: its own functions
        // (Platinum has functions with Black 2's names, StartMenu_Main and CARD_WaitBackupAsync
        // among them: Black 2's layouts must not pick them up.)
        if (pt.on) bw2 = Bw2Funcs();
        bw1.battleButtons = cfg.battleButtons;
        {
            char code[5] = {};
            if (FILE* rf = fopen(opt.rom.c_str(), "rb"))
            {
                if (fseek(rf, 0x0C, SEEK_SET) != 0 || fread(code, 1, 4, rf) != 4) code[0] = 0;
                fclose(rf);
            }
            bw2map.Init(code, funcWatch, bw2Names, *nds, cfg.wide && cfg.wideMap);
        }
        // Only these are watched (all functions for --trace-funcs).
        funcWatch.Attach(*nds, opt.traceFuncsAt.empty() ? bw2Names : std::vector<std::string>());
        fprintf(stderr, "[bw2] field %d, menu %d, bag %d, battle %d\n", bw2.field, bw2.fieldMenu, bw2.bag, bw2.battle);
    }
    Scene lastKnownBw2;
    uint32_t lastKnownBw2Frame = 0;
    auto weatherParticles = [&]() {
        for (int f : bw2.weatherObj)
            if (funcWatch.Ran(f, 4)) return true;
        return false;
    };
    // A Gen 4 battle (SoulSilver, HeartGold, Platinum) choosing a command or a move: the scene
    // (see where pickScene calls it). The two games' touch screens are drawn alike there.
    auto gen4BattleInput = [&](float viewWidth, const ScreenImage* screens) {
        Scene sc;
        sc.name = "battle";
        sc.items = { FullScreen(0) };
        sc.edgeFill = true;
        const uint32_t* b = screens[1].pixels;
        auto rgb = [&](int x, int y, int& r, int& g, int& bl) {
            uint32_t c = b[y * 256 + x];
            r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; bl = c & 0xFF;
        };
        auto blue = [&](int x, int y) { int r, g, bl; rgb(x, y, r, g, bl); return bl > 150 && r < 110 && g > 90 && g < 200 && bl > r + 60; };
        auto red = [&](int x, int y) { int r, g, bl; rgb(x, y, r, g, bl); return r > 180 && g < 120 && bl < 120; };
        auto orange = [&](int x, int y) { int r, g, bl; rgb(x, y, r, g, bl); return r > 180 && g > 110 && g < 200 && bl < 110; };
        // (Platinum's first battle has FIGHT alone: no Bag, no running, no party yet.)
        // (Platinum: which panel the battle drew last, when known. Its "Use next Pokemon?" is a
        // red bar on top like the first battle's FIGHT, with RUN below it in neither colour.)
        const int ptPanel = pt.on ? pt.BattlePanel(funcWatch) : 0;
        battleFightOnly = pt.on && (ptPanel == 0 || ptPanel == 2) &&
            red(128, 60) && !orange(40, 160) && !blue(128, 166) && !blue(40, 166);
        const bool cmd = (red(128, 60) && orange(40, 160) && blue(128, 166) && ptPanel != 4) || battleFightOnly;
        const bool moves = !cmd && ptPanel != 4 && blue(40, 166) && blue(128, 166) && blue(216, 166);
        const bool idle = AutoFocus::Blank(screens[1].pixels) || (!blue(128, 166) && !red(128, 60));
        battleUnknown = (cmd || moves || idle) ? 0 : battleUnknown + 1;
        // (Another of Platinum's panels, known for what it is: the touch screen shown at once.)
        if (ptPanel == 4 && !cmd && !moves && !AutoFocus::Blank(screens[1].pixels)) battleUnknown = std::max(battleUnknown, 20);
        // Only after the commands (FIGHT): after a move is chosen the battle goes on and
        // its messages must not be covered.
        if (cmd) battleStripUntil = frame + 20;
        else if (moves || idle) battleStripUntil = 0;
        const float L = 128 - viewWidth / 2;
        auto put = [&](int sx, int sy, int sw, int sh, float x, float y, float scale) {
            sc.items.push_back({ 1, sx, sy, sw, sh, x, y, scale, 0, 255, false });
        };
        if (viewWidth >= 300 && (cmd || moves || frame < battleStripUntil))
        {
            // (Neither: the panel without buttons, while the next ones slide in.)
            sc.name = cmd ? "battle-cmd" : moves ? "battle-moves" : "battle-strip";
            // The bottom band of the view (y 144-191, the message box's rows, below the
            // player's HP box which the wide battle moves to the right edge and which ends
            // at y 139) in two halves: the message box ("What will X do?") in the left half,
            // the buttons in the right half, so that the player's Pokemon and the message
            // stay in sight. The box is its left part (the text ends by x 146 with the
            // longest name) closed with a mirrored copy of its left edge; the buttons keep
            // the touch screen's arrangement: FIGHT above BAG, RUN and POKeMON; the four
            // moves two by two.
            const float bandY = 144, bandH = 48, viewL = 128 - viewWidth / 2, viewR = 128 + viewWidth / 2;
            const int edgeW = 10, boxW = std::min(246, (int)(viewWidth / 2) - edgeW);
            sc.items.push_back({ 0, 0, 144, boxW, 48, viewL, bandY, 1.0f, 0, 255, false });
            Element boxEdge = { 0, 0, 144, edgeW, 48, viewL + boxW, bandY, 1.0f, 0, 255, false };
            boxEdge.flipX = true;
            sc.items.push_back(boxEdge);
            const float panelX = viewL + boxW + edgeW, panelY = bandY, panelR = viewR, panelB = bandY + bandH;
            // (The margins leave room for the outlines, which are drawn outside each button: the
            // chosen one's red outline stays inside the half, clear of the scene above.)
            const float panelW = panelR - panelX, panelH = panelB - panelY, pad = 3, gap = 4;
            // (The band's own colour around the message box.)
            sc.items.push_back(FillRect(panelX, panelY, (int)std::ceil(panelW) + 1, (int)panelH, 0x202020));
            // Each button's outline is drawn around the item listed here for it (in the order
            // of battleSel).
            std::vector<size_t> outlined;
            // The buttons fill the half: FIGHT, BAG and RUN from its left (after the message
            // box) and POKeMON to the right edge of the view, the chosen one's outline on it;
            // the moves the same width.
            const float innerX = panelX + pad, innerW = panelW - pad - 16 / 8.0f;
            const float rowH = (panelH - 2 * pad - gap) / 2, rowY = panelY + pad, row2Y = rowY + rowH + gap;
            // A button cut from the touch screen (sx, sy, sw x sh) drawn dw wide and rowH
            // high: its label (lx0..lx1, everything that is not plain) stays as it is, and
            // the plain columns tileX..tileX+tileW (the button is one colour across there)
            // are repeated on both sides of it to fill the width. A dark rectangle under it
            // carries its outline.
            struct Cut { int sx, sy, sw, sh, lx0, lx1, tileX, tileW; };
            auto button = [&](const Cut& c, float dx, float dy, float dw, uint8_t drop) {
                const float scl = rowH / c.sh;
                // (Sized exactly, not to whole base pixels, so that the edges of buttons
                // above one another line up.)
                Element backing = FillRect(dx, dy, 1000, (int)std::lround(1000 * rowH / dw), 0x101218);
                backing.scale = dw / 1000;
                outlined.push_back(sc.items.size());
                sc.items.push_back(backing);
                auto piece = [&](int sx, int sw, float x) {
                    put(sx, c.sy, sw, c.sh, x, dy, scl);
                    sc.items.back().drop = drop;
                };
                if (dw <= c.sw * scl + 0.5f)
                {
                    piece(c.sx, c.sw, dx + (dw - c.sw * scl) / 2);
                    return;
                }
                // The plain columns widened across the whole button as one piece, the ends and
                // the label over them. (Once repeated a few pixels at a time: some 90 pieces a
                // button, each its own picture to upload, which on the Switch dropped the frame
                // rate whenever a battle waited for a command.)
                piece(c.tileX, c.tileW, dx);
                sc.items.back().stretchX = dw / (c.tileW * scl);
                piece(c.sx, c.lx0 - c.sx, dx);
                piece(c.lx1, c.sx + c.sw - c.lx1, dx + dw - (c.sx + c.sw - c.lx1) * scl);
                piece(c.lx0, c.lx1 - c.lx0, dx + (dw - (c.lx1 - c.lx0) * scl) / 2);
            };
            if (cmd)
            {
                // FIGHT above BAG and RUN, as wide as the two; the party beside it, above
                // POKeMON. Each button is the coloured inside of the touch screen's button
                // with its label, cut to the same height (the buttons are one colour across,
                // apart from the label).
                static const Cut cuts[4] = { { 30, 67, 196, 34, 86, 146, 33, 4 }, { 6, 152, 66, 34, 30, 51, 9, 4 },
                                             { 182, 151, 68, 34, 192, 241, 185, 4 }, { 94, 158, 68, 34, 118, 139, 97, 4 } };
                // (Platinum's buttons are shaded towards their edges: their flat insides only.)
                static const Cut ptCuts[4] = { { 39, 64, 178, 34, 84, 146, 50, 4 }, { 12, 154, 56, 33, 30, 51, 14, 4 },
                                               { 188, 154, 56, 33, 192, 241, 188, 4 }, { 100, 162, 56, 30, 118, 139, 102, 4 } };
                const Cut* cut = pt.on ? ptCuts : cuts;
                const float cellW = (innerW - 2 * gap) / 3;
                button(cut[0], innerX, rowY, 2 * cellW + gap, 0);                           // FIGHT
                if (!battleFightOnly)
                {
                    button(cut[1], innerX, row2Y, cellW, 0);                                    // BAG
                    button(cut[2], innerX + 2 * (cellW + gap), row2Y, innerW - 2 * (cellW + gap), 0); // POKeMON
                    button(cut[3], innerX + cellW + gap, row2Y, cellW, 0);                      // RUN
                }
                // The party: the balls on the touch screen's top row (a ball for each
                // Pokemon, a grey dot for an empty place; sprites, taken without their
                // background), three over three with room between them. Each is cut on its
                // own, 19 pixels apart from x 3 (both games; cutting three at a time put the
                // second three a pixel off and clipped their tops; the outer pixel all round is
                // left out, part of the outline anyway), and drawn without its dark
                // outline (0x383030 in Platinum, 0x414141 in SoulSilver), which showed as a
                // grey ring on the panel: the panel shows through it instead.
                const float partyX = innerX + 2 * (cellW + gap), partyW = innerW - 2 * (cellW + gap);
                const uint8_t drop = LAYER_BG0 | LAYER_BG1 | LAYER_BG2 | LAYER_BG3 | LAYER_BACKDROP;
                const float ball = 17, ballGapX = 4, ballGapY = 4; // (source pixels)
                const float ps = std::min(partyW / (3 * ball + 2 * ballGapX), (rowH - 1) / (2 * ball + ballGapY));
                const float px = partyX + (partyW - (3 * ball + 2 * ballGapX) * ps) / 2;
                const float py = rowY + (rowH - (2 * ball + ballGapY) * ps) / 2;
                for (int i = 0; i < 6; i++)
                {
                    put(4 + 19 * i, 5, (int)ball, (int)ball, px + (i % 3) * (ball + ballGapX) * ps, py + (i / 3) * (ball + ballGapY) * ps, ps);
                    sc.items.back().drop = drop;
                    sc.items.back().keys[0] = 0x383030;
                    sc.items.back().keys[1] = 0x414141;
                }
            }
            else if (moves)
            {
                // The four moves two by two, each its button's rows 9-46 of 54 (name, type
                // and PP, between the frame's rounded corners), widened with the plain
                // columns just inside its sides. There is no room for the CANCEL bar; B
                // cancels.
                const float cellW = (innerW - gap) / 2;
                static const int slotX[4] = { 1, 129, 1, 129 }, slotY[4] = { 25, 25, 89, 89 };
                for (int i = 0; i < 4; i++)
                {
                    // (Platinum's: the button inside its outline, its pale middle kept, the dark
                    // band just inside its sides repeated.)
                    const Cut c = pt.on ? Cut{ slotX[i] + 2, slotY[i] + 10, 122, 40, slotX[i] + 14, slotX[i] + 112, slotX[i] + 5, 4 }
                                        : Cut{ slotX[i], slotY[i] + 9, 126, 38, slotX[i] + 15, slotX[i] + 109, slotX[i] + 12, 3 };
                    button(c, innerX + (i & 1) * (cellW + gap), (i >> 1) ? row2Y : rowY, (i & 1) ? innerW - cellW - gap : cellW, LAYER_BG2);
                }
            }
            // The chosen button is outlined in white and the others are shaded, so that the
            // choice shows at a glance on any of the buttons (a red outline was lost around the
            // red FIGHT button, and a thin one was hard to see on a TV). (The game's own cursor
            // is not used by the panel; the keys that would bring up its red corners do not
            // reach the game.)
            std::vector<Element> shades;
            for (size_t k = 0; k < outlined.size(); k++)
            {
                const bool chosen = (int)k == battleSel;
                Element& el = sc.items[outlined[k]];
                el.border = true;
                el.borderColor = chosen ? 0xFFFFFF : 0x101218;
                // (Outlines are drawn outside the buttons: 2 base pixels fits in the gap of 4
                // between buttons and the panel's margins.)
                el.borderWidth = chosen ? 16 : 6;
                if (!chosen)
                {
                    // (A translucent black over the whole button, its backing's size: the
                    // pieces themselves overlap, and made translucent they would show stripes.)
                    Element shade = el;
                    shade.border = false;
                    shade.keys[0] = 0x000000;
                    shade.alpha = 120;
                    shades.push_back(shade);
                }
            }
            sc.items.insert(sc.items.end(), shades.begin(), shades.end());
        }
        else if (viewWidth < 300 || battleUnknown >= 20)
        {
            sc.name = "battle-menu";
            float scale = std::min((126 - L) / 256, 98.0f / 192);
            sc.items.push_back({ 1, 0, 0, 256, 192, L + 1, 144 - 192 * scale, scale, 0, 255, false });
            sc.items.back().border = !AutoFocus::Blank(screens[1].pixels);
        }
        return sc;
    };
    auto pickSceneBw2 = [&](float viewWidth, const ScreenImage* screens) {
        Scene sc;
        sc.smartEdges = true;
        auto only = [&](const char* name, int screen) {
            sc.name = name;
            sc.items = { FullScreen(screen) };
        };
        const bool wideView = viewWidth >= 300;
        // A panel of touch-screen controls in the lower right corner of the view, below the
        // player's HP box (whose numbers end at y 132), over the empty right part of the
        // message box, keeping the game's own arrangement so that the D-pad moves the
        // highlight where it looks like it should.
        auto cornerPanel = [&](Element crop, float scale) {
            const float pad = 2;
            const float w = crop.sw * scale, h = crop.sh * scale;
            const float x = (wideView ? 128 + viewWidth / 2 : 256.0f) - w - pad - 2;
            const float y = 192 - h - pad - 1;
            sc.items.push_back(FillRect(x - pad, y - pad, (int)(w + 2 * pad), (int)(h + 2 * pad), 0x14141C));
            sc.items.back().border = true;
            crop.dx = x;
            crop.dy = y;
            crop.scale = scale;
            sc.items.push_back(crop);
        };
        // The edges of the view the bottom screen fills when it stands against the right edge.
        const float viewLeft = 128 - viewWidth / 2, viewRight = 128 + viewWidth / 2;
        // Platinum's screens other than battles and the overworld (frontend/platinum.h). False
        // for a screen it has no layout for.
        auto ptApp = [&]() {
            auto ran = [&](int f) { return funcWatch.Ran(f, 2); };
            // One screen at full height, the bars beside it in colours from its edges.
            auto alone = [&](const char* name, int screen) {
                only(name, screen);
                sc.softBars = true;
            };
            if (ran(pt.party))
            {
                // The top screen; the touch screen only has Poke Balls. (Its edges are bands of
                // blue that would turn to streaks: one blue beside it instead.)
                only("party", 0);
                sc.bgScreen = 0;
                sc.bgX = 1;
                sc.bgY = 140;
            }
            else if (ran(pt.bag)) alone("bag", 0);       // the touch screen: the pockets, which LEFT and RIGHT reach
            else if (ran(pt.summary)) alone("summary", 0);
            else if (ran(pt.options)) alone("options", 0);
            else if (ran(pt.journal)) alone("journal", 0);
            else if (ran(pt.moveReminder)) alone("move-reminder", 0);
            else if (ran(pt.townMap)) alone("map", 0);   // the touch screen: a zoom button
            else if (ran(pt.dex))
                // The list or a page above, its buttons and tabs on the touch screen.
                sc = Stack("pokedex", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
            else if (ran(pt.trainerCard))
                // The card and its badges.
                sc = Stack("trainer-card", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
            else if (ran(pt.box))
                sc = Stack("pc-box", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
            else if (ran(pt.naming))
                // The question (the top screen's message box) above the name and the keyboard.
                sc = Stack("naming", { Crop(0, 0, 144, 256, 48), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
            else if (ran(pt.evolution) || ran(pt.eggHatch)) alone("evolution", 0);
            else if (ran(pt.intro))
            {
                // A new game's introduction: Professor Rowan, his words and the pages on the
                // controls are all on the top screen; the touch screen only has a pattern, except
                // for his two questions answered by a touch (PtFuncs::IntroTouch). For those, the
                // YES / NO buttons or the Poke Ball are cut out of the touch screen and stand at
                // the right edge of the view, above the message box (y 144 on), touchable.
                alone("intro", 0);
                const int touch = screens ? PtFuncs::IntroTouch(screens[1].pixels) : 0;
                if (touch)
                {
                    Element crop = touch == PtFuncs::INTRO_YESNO ? Crop(1, 94, 64, 54, 74) : Crop(1, 56, 24, 146, 148);
                    const float scale = touch == PtFuncs::INTRO_YESNO ? 1.0f : 0.6f;
                    const float pad = 2, w = crop.sw * scale, h = crop.sh * scale;
                    const float x = (wideView ? viewRight : 256.0f) - w - pad - 2, y = 72 - h / 2;
                    sc.items.push_back(FillRect(x - pad, y - pad, (int)(w + 2 * pad), (int)(h + 2 * pad), 0x14141C));
                    sc.items.back().border = true;
                    crop.dx = x;
                    crop.dy = y;
                    crop.scale = scale;
                    sc.items.push_back(crop);
                    sc.name = "intro-touch";
                }
            }
            else if (ran(pt.starter))
            {
                // The briefcase on the grass, in 3D: widened like the world.
                only("starter", 0);
                sc.wide = wideView;
            }
            else return false;
            return true;
        };
        if (cgearHidden)
        {
            // CONTINUE, while the C-Gear's question is answered unseen (see where keys are
            // sent to the game): black, as the fade out of the start menu ends.
            sc.name = "continue";
            sc.items = { FillRect(viewLeft - 1, 0, (int)viewWidth + 3, 192, 0x000000) };
            return sc;
        }
        if (focus.manualFlip)
            // The player asked for the other screen (right stick click, F1): an escape hatch
            // for whatever a layout leaves out.
            only("manual", focus.wantBottom ? 0 : 1);
        else if (pt.on && funcWatch.Ran(pt.battle, 2))
        {
            // Platinum: a battle. Its Bag and party are touch-screen pages; its commands and
            // moves are SoulSilver's, drawn alike (gen4BattleInput); otherwise the top screen,
            // its background continued to the view's edges (battlewide.h).
            const bool bottomBlank = !screens || AutoFocus::Blank(screens[1].pixels);
            if (funcWatch.Ran(pt.battleBag, 2) || funcWatch.Ran(pt.battleParty, 2))
                only("battle-submenu", 1);
            else if (screens && (funcWatch.Ran(pt.battleInput, 2) || (frame < battleStripUntil && !bottomBlank)))
                sc = gen4BattleInput(viewWidth, screens);
            else
            {
                only("battle", 0);
                sc.edgeFill = true;
            }
        }
        else if (pt.on && funcWatch.Ran(pt.saving, 8) && lastKnownBw2Frame)
            // Platinum writing its save: for a few seconds nothing else runs, not even the
            // overworld; the layout that was up stays.
            return lastKnownBw2;
        else if (pt.on && ptApp())
            ; // Platinum's other screens (ptApp, above)
        else if (pt.on && funcWatch.Ran(pt.field, 16))
        {
            // The overworld, the X menu and anything else over it on the top screen, the
            // world widened; the Poketch on the touch screen is left out (the right stick
            // click shows it).
            only("top", 0);
            sc.wide = wideView;
            sc.weatherObj = pt.Weather(funcWatch, 4); // rain, snow, ...: repeated to the view's edges
            if (wideView && funcWatch.Ran(pt.startMenu, 2))
            {
                // The X menu (its box x 154-253, y 0-181 of the top screen) stands against the
                // view's right edge, as the game has it against the screen's, with the world
                // where it was.
                sc.name = "start-menu";
                Element& top = sc.items[0];
                top.holeCount = 1;
                const int box[4] = { 152, 0, 256, 184 };
                std::copy(box, box + 4, top.holes[0]);
                Element m = Crop(0, box[0], box[1], box[2] - box[0], box[3] - box[1]);
                m.dx = viewRight - (box[2] - box[0]);
                m.dy = box[1];
                m.drop = LAYER_3D | LAYER_BACKDROP;
                m.crisp = true;
                m.wideOnly = true;
                sc.items.push_back(m);
            }
        }
        else if (funcWatch.Ran(bw2.battle, 2) && (funcWatch.Ran(bw2.battleList, 2) || funcWatch.Ran(bw2.battleBag, 2)))
            // The party (after POKEMON, or to send out the next one after fainting) or the Bag:
            // a whole touch-screen page, shown as it is.
            only("battle-list", 1);
        else if (funcWatch.Ran(bw2.battle, 2) && (funcWatch.Ran(bw2.battleChoice, 4) || funcWatch.Ran(bw2.battleYesNo, 4)))
            // A question on the top screen ("Use next Pokemon?") answered on the touch screen:
            // both, side by side.
            sc = Stack("battle-choice", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
        else if (funcWatch.Ran(bw2.battle, 2))
        {
            // The battle is drawn in 3D on the top screen, so the widescreen renderer shows
            // more of the arena; the messages are 2D and stay where they are.
            only("battle", 0);
            sc.wide = wideView;
            // A single battle on a wide view: the HP boxes keep the game's own places but against
            // the view's edges, as the game has them against the screen's: the enemy's in the
            // upper left, the player's in the lower right, above the touch-screen panel. Where
            // they were, the wide arena shows. A double, triple or rotation battle keeps the
            // game's own places.
            int kind = 0; // 1 single, 2 another kind, 0 not seen (a snapshot loaded mid-battle)
            uint32_t seen = 0;
            const int setups[4] = { bw2.setupSingle, bw2.setupDouble, bw2.setupTriple, bw2.setupRotation };
            for (int k = 0; k < 4; k++)
                if (funcWatch.LastFrame(setups[k]) > seen)
                {
                    seen = funcWatch.LastFrame(setups[k]);
                    kind = k == 0 ? 1 : 2;
                }
            if (!kind && screens && screens[0].flags)
            {
                // Not seen: a single battle has one box at each of these places.
                auto height = [&](int x0, int y0, int x1, int y1) {
                    int top = 999, bottom = -1;
                    for (int y = y0; y < y1; y++)
                        for (int x = x0; x < x1; x++)
                        {
                            const uint8_t cl = LayerClass(screens[0].flags[y * 256 + x]);
                            if (cl != LAYER_3D && cl != LAYER_BACKDROP) { top = std::min(top, y); bottom = std::max(bottom, y); }
                        }
                    return bottom - top + 1;
                };
                const int enemyH = height(0, 0, 128, 100), mineH = height(128, 60, 256, 142);
                kind = enemyH > 0 && enemyH <= 32 && mineH > 0 && mineH <= 34 ? 1 : 2;
            }
            // (The compositor leaves the boxes where they are in any frame without the wide arena.)
            // The player's box is lifted this much, to clear the corner panel's frame.
            constexpr int BattleBoxLift = 4;
            // How much of a part of the top screen is 2D, in tenths: where the HP boxes are, some
            // of it; none when the boxes are not shown (the screen going black into an evolution,
            // where the wide arena can still show the last picture through the holes); nearly all
            // under a layer over the whole screen. The boxes move while either side has some (a
            // side whose Pokemon have all fainted has none) and neither is covered.
            auto tenths2D = [&](int x0, int y0, int x1, int y1) {
                if (!screens || !screens[0].flags) return 5;
                int n = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                    {
                        const uint8_t cl = LayerClass(screens[0].flags[y * 256 + x]);
                        n += cl != LAYER_3D && cl != LAYER_BACKDROP;
                    }
                return n * 10 / ((x1 - x0) * (y1 - y0));
            };
            const int foes2D = tenths2D(0, 4, 124, 70), mine2D = tenths2D(136, 78, 256, 147);
            // (And not with the screen as it is shown black there: the battle fading out into an
            // evolution, with the boxes still in place underneath.)
            auto lit = [&](int x0, int y0, int x1, int y1) {
                if (!screens || !screens[0].pixels) return true;
                int n = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                    {
                        const uint32_t c = screens[0].pixels[y * 256 + x];
                        n += ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF) > 48;
                    }
                return n * 20 > (x1 - x0) * (y1 - y0);
            };
            const bool boxesShown = (foes2D >= 1 || mine2D >= 1) && foes2D < 9 && mine2D < 9
                && (lit(0, 4, 124, 70) || lit(136, 78, 256, 147)) && !funcWatch.Ran(bw2.evolution, 2)
                // (Nor while the bars of Poke Balls are up as a trainer battle starts: they cross
                // the boxes' places, and moving those cut the bars into pieces. The battle keeps
                // the screen's own places then, before the boxes come in.)
                && !(funcWatch.Ran(bw2.ballBarTask, 3) || funcWatch.LastFrame(bw2.ballBarCreate) > funcWatch.LastFrame(bw2.ballBarDelete));
            // ... or the top screen darkened or whitened (master brightness): the holes would show
            // the wide arena at full brightness through it.
            const auto& topGpu = topUnitOf() == 0 ? nds->GPU.GPU2D_A : nds->GPU.GPU2D_B;
            const int brightMode = (topGpu.MasterBrightness >> 14) & 3;
            const bool fading = ((brightMode == 1 || brightMode == 2) && (topGpu.MasterBrightness & 0x1F) != 0)
                // (or a brightness effect on the 3D layer, as the frame drawing reads it too)
                || ((topGpu.BlendCnt & 1) && ((topGpu.BlendCnt >> 6) & 3) >= 2 && topGpu.EVY != 0);
            if (wideView && (kind == 1 || kind == 2) && !fading && boxesShown)
            {
                // A single battle: the enemy's box covers x 4-109, y 30-51 of the top screen, the
                // player's x 142-255, y 110-138 (with room for them to shake when hit). A double,
                // triple or rotation battle: the foes' two or three boxes x 4-120, y 8-66, the
                // player's x 140-255, y 82-145 (measured in Black 2 and White 2).
                Element& top = sc.items[0];
                top.holeCount = 2;
                // While the level-up window is open (above the player's box, x 150-255, y 34-132,
                // its last rows over the box) the window moves with the box: the piece starts at
                // its top, or the box would cut through the list of stats.
                bool levelUp = false;
                for (int f : bw2.levelUpWin) levelUp |= funcWatch.Ran(f, 3);
                const bool one = kind == 1;
                const int enemy[4] = { 0, one ? 26 : 4, one ? 116 : 124, one ? 56 : 70 },
                          mine[4] = { one ? 138 : 136, levelUp ? 20 : one ? 106 : 78, 256, one ? 142 : 147 };
                std::copy(enemy, enemy + 4, top.holes[0]);
                std::copy(mine, mine + 4, top.holes[1]);
                // A side whose boxes are gone keeps its place: while a Poke Ball is thrown the
                // camera turns to the foe and the player's box is put away, and the ball, its
                // shadow and the shakes land inside that box's place, where a moved piece would cut
                // the ball in two and show its other half at the view's edge. (A box covers more
                // than a tenth of its side's area; the ball and its sparkles far less.) The piece
                // stays in the list with no size, so that the Black and White layouts
                // (frontend/bw1.h) still find each side's piece where they look for it.
                if (foes2D < 1) std::fill(top.holes[0], top.holes[0] + 4, 0);
                if (mine2D < 1 && !levelUp) std::fill(top.holes[1], top.holes[1] + 4, 0);
                // (The enemy's box starts 4 pixels in from the left edge of its piece, as from the
                // screen's; the player's runs to the right edge of its piece.)
                Element e = Crop(0, enemy[0], enemy[1], enemy[2] - enemy[0], enemy[3] - enemy[1]);
                e.dx = viewLeft;
                e.dy = enemy[1];
                e.drop = LAYER_3D | LAYER_BACKDROP;
                e.crisp = true;
                e.wideOnly = true;
                if (foes2D < 1) e.sw = e.sh = 0;
                sc.items.push_back(e);
                Element m = Crop(0, mine[0], mine[1], mine[2] - mine[0], mine[3] - mine[1]);
                m.dx = viewRight - (mine[2] - mine[0]);
                m.dy = mine[1] - BattleBoxLift;
                m.drop = LAYER_3D | LAYER_BACKDROP;
                m.crisp = true;
                m.wideOnly = true;
                if (mine2D < 1 && !levelUp) m.sw = m.sh = 0;
                sc.items.push_back(m);
            }
            const bool moves = funcWatch.Ran(bw2.battleMove, 1), command = !moves && funcWatch.Ran(bw2.battleCommand, 1);
            // Choosing the target of a move in a double, triple or rotation battle: the foes'
            // panels above, the player's side below (touch screen rows 22-131).
            const bool target = !moves && !command && funcWatch.Ran(bw2.battleTarget, 1);
            // The touch screen's buttons, as BATTLE BUTTONS in the pause menu says: CORNER or
            // PANEL (Black's are drawn differently: frontend/bw1.h changes the corner panel made
            // here into its own, STRIP among them; Black 2 has no STRIP, which left out the party's
            // balls and did not look good). A battle with two or three Pokemon on each side has
            // as many HP boxes down the right, the lowest one ending at row 145, which the corner
            // panel would cover: it always has the panel, in the message box's band below them.
            int style = bw1.on || !wideView || !screens ? 0 : cfg.battleButtons;
            if (wideView && screens && !bw1.on && kind == 2) style = 2;
            if ((moves || command || target) && style == 2)
            {
                // PANEL: the corner panel's piece as tall as the message box (rows 147-189),
                // against the view's right edge.
                Element c = moves ? Crop(1, 0, 30, 256, 98) : target ? Crop(1, 0, 22, 256, 110) : Crop(1, 0, 78, 256, 114);
                c.scale = 43.0f / c.sh;
                c.dx = viewRight - 2 - c.sw * c.scale;
                c.dy = 147;
                c.border = true;
                sc.items.push_back(c);
            }
            else if (moves)
                cornerPanel(Crop(1, 0, 30, 256, 98), 0.5f);   // the four moves, two by two (49 high)
            else if (command)
                cornerPanel(Crop(1, 0, 78, 256, 114), 0.44f); // FIGHT above BAG, RUN and POKEMON (50 high)
            else if (target)
                cornerPanel(Crop(1, 0, 22, 256, 110), 0.45f); // the targets (50 high)
        }
        else if (funcWatch.Ran(bw2.save, 8) || (funcWatch.Ran(bw2.saveWrite, 8) && !strcmp(lastKnownBw2.name, "save")))
        {
            // Saving from the X menu: the question, YES / NO, the card of the saved game and
            // the progress of the save are all on the touch screen (the top one keeps the
            // overworld, with a save icon over the player, and the top of a message box with an
            // arrow pointing down). Shown as the overworld with the message box along its
            // bottom, where the game's other messages are, and the rest of the touch screen as a
            // panel in the upper right corner. (While the save is written the game runs nothing
            // else for more than a second: the layout stays, rather than the two screens side
            // by side popping up for that time, as they did.)
            only("save", 0);
            sc.wide = wideView;
            sc.weatherObj = weatherParticles();
            sc.items[0].drop = LAYER_BG1; // the strip pointing at the other screen (as in the X menu)
            SaveScreenPieces(sc, wideView, viewRight, screens);
        }
        else if (funcWatch.Ran(bw2.fieldMenu, 8)) // (it skips a few frames while the menu opens)
        {
            // The menu's buttons at the right edge, mostly beside the overworld, in the game's
            // own grid of two columns, so that the D-pad moves the highlight where it looks
            // like it should.
            only("start-menu", 0);
            sc.wide = wideView;
            sc.weatherObj = weatherParticles();
            // The strip along the bottom of the top screen (BG1, with an arrow down) only says
            // to look at the other screen; without it the overworld shows there instead.
            sc.items[0].drop = LAYER_BG1;
            // The touch screen holds up to six buttons, two to a row, in a fixed grid: 126 x 44
            // pixels each, rows 48 apart from y = 22. A slot is used when its button's left
            // edge (a light frame) is there; the menu fills the slots row by row.
            struct Slot { Element crop; int row, col; };
            std::vector<Slot> buttons;
            int rows = 0;
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 2; col++)
                {
                    const int bx = col ? 130 : 2, by = 22 + row * 48;
                    const uint32_t c = screens[1].pixels[(by + 20) * 256 + bx + 2];
                    const int lum = (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) / 3;
                    if (lum > 100)
                    {
                        buttons.push_back({ Crop(1, bx, by, 126, 44), row, col });
                        rows = row + 1;
                    }
                }
            // (None drawn: the menu is opening or closing. The overworld alone then.)
            if (!buttons.empty())
            {
                const float scale = wideView ? 0.45f : 0.4f, gap = 2, pad = 3;
                const float bw = 126 * scale, bh = 44 * scale;
                const float w = 2 * bw + gap, h = rows * bh + (rows - 1) * gap;
                const float x = (wideView ? 128 + viewWidth / 2 : 256.0f) - w - pad - 2;
                const float y = (192 - h) / 2;
                sc.items.push_back(FillRect(x - pad, y - pad, (int)(w + 2 * pad), (int)(h + 2 * pad), 0x14141C));
                sc.items.back().border = true;
                for (Slot b : buttons)
                {
                    b.crop.dx = x + b.col * (bw + gap);
                    b.crop.dy = y + b.row * (bh + gap);
                    b.crop.scale = scale;
                    sc.items.push_back(b.crop);
                }
            }
        }
        else if (funcWatch.Ran(bw2.field, 16))
        {
            only("top", 0);
            sc.wide = wideView;
            sc.weatherObj = weatherParticles();
        }
        else if (funcWatch.Ran(bw2.bag, 2))
        {
            // The bag at full size against the right edge, and the item's card from the other
            // screen (name, icon, count and description) over the left part, where the bottom
            // screen only has a picture of the bag (its pockets also change with L and R).
            // (No item chosen, or an empty pocket: the card is blank, the bag alone in the
            // middle. The card's text is white: some of it means there is something to show.)
            int text = 0;
            if (screens && screens[0].pixels)
                for (int y = 28; y < 152; y += 2)
                    for (int x = 8; x < 248; x += 2)
                    {
                        const uint32_t c = screens[0].pixels[y * 256 + x];
                        text += ((c >> 16) & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200 && (c & 0xFF) > 200;
                    }
            sc.name = "bag";
            Element list = FullScreen(1);
            if (text >= 12)
            {
                list.dx = viewRight - 256;
                const float left = viewRight - 256 + 116 - viewLeft; // up to where the list starts
                Element card = Crop(0, 8, 28, 240, 124);
                card.scale = std::min(0.85f, (left - 6) / card.sw);
                card.dx = viewLeft + 3;
                card.dy = 4;
                card.border = true;
                sc.items = { list, card };
            }
            else
                sc.items = { list };
            sc.bgScreen = 1;
            sc.bgX = 2;
            sc.bgY = 2;
        }
        else if (funcWatch.Ran(bw2.options, 2))
            // The explanation of the chosen option (a box near the bottom of the other screen)
            // above the list of options.
            sc = Stack("options", { Crop(0, 0, 146, 256, 44), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
        else if (funcWatch.Ran(bw2.summary, 2))
            // Both pages (memo and details; stats and moves) use both screens.
            sc = Stack("summary", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
        else if (funcWatch.Ran(bw2.box, 2))
            // The chosen Pokemon's card (moves, ability, item) beside the box.
            sc = Stack("pc-box", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
        else if (funcWatch.Ran(bw2.dexEntry, 2))
            only("pokedex-entry", 1); // the other screen is an empty grid
        else if (funcWatch.Ran(bw2.dexList, 2))
            // The list runs on from the top screen into the bottom one: one column.
            sc = Stack("pokedex-list", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
        else if (funcWatch.Ran(bw2.townMap, 2))
            // The map (the place's name is on its own banner); the other screen's card of
            // flavour text is one press away (the escape hatch above).
            only("town-map", 1);
        else if (funcWatch.Ran(bw2.party, 2))
            only("party", 1);         // the top screen only has a backdrop
        else if (funcWatch.Ran(bw2.trainerCard, 2))
            only("trainer-card", 1);  // likewise
        else if (funcWatch.Ran(bw2.startMain, 2) && !funcWatch.Ran(bw2.startContinue, 2))
        {
            // The start menu: its list (the top screen) beside the saved game's cards (the
            // bottom one), across the whole view on the dark grey of the menu's own background
            // (merged.h, CleanMenu): the list's buttons without the Poke Ball and stripes behind
            // them (BG3), the cards without the stripes (BG2). The list goes on below the screen
            // (the DS shows its eighth entry cut in half), and so does the lower card: both are
            // drawn on below the screens (belowscreen.h). (After CONTINUE, the screens with the
            // C-Gear's questions are shown whole, below.)
            // The list: eight entries 24 rows apart from row 16 (x 33-223), the last one below
            // the screen. The cards: the saved game's (rows 15-114) and the key system's (from
            // row 117), which runs off the bottom of the screen and is never drawn further: it
            // is cut below its text and closed with the saved game's card's bottom edge.
            const int extra = 16;
            Element list = Crop(0, 32, 15, 194, 193);        // x 32-225, rows 15-207
            Element cards = Crop(1, 22, 14, 214, 166);       // x 22-235, rows 14-179
            Element closing = Crop(1, 22, 109, 214, 6);      // rows 109-114: a card's bottom edge
            list.drop = cards.drop = closing.drop = LAYER_BACKDROP;
            sc = CleanMenu("launch-menu", { list, cards }, viewWidth, 0x1c1c1c);
            const Element& c = sc.items.back();
            closing.scale = c.scale;
            closing.dx = c.dx;
            closing.dy = c.dy + c.sh * c.scale;
            closing.crisp = true;
            sc.items.push_back(closing);
            sc.extraRows[0] = extra;
            sc.redrawSkip[0] = 1 << 3;   // the list without the Poke Ball and stripes (BG3)
            sc.redrawUnscroll[0] = 2;     // the list whole, also when the game has scrolled it
            sc.redrawSkip[1] = 1 << 2;   // the cards without the stripes (BG2)
        }
        else if (funcWatch.Ran(bw2.nameEntry, 2))
            // The keyboard, below the question it answers ("Your name?", in the message box at
            // the bottom of the other screen).
            sc = Stack("naming", { Crop(0, 0, 144, 256, 48), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
        else if (funcWatch.Ran(bw2.intro, 2))
        {
            // A new game's introduction: everything (the professor, the questions, the choice
            // between the two characters) is on the top screen; the other only has a pattern.
            only("intro", 0);
            sc.softBars = true;
        }
        else
        {
            // Nothing recognised. Mostly a moment between screens (a fade, a button pressed in
            // battle) in which none of the watched functions runs: the last layout stays for
            // up to 45 frames. After that this is a screen without a layout of its own, and
            // both screens are shown side by side, so nothing is hidden.
            if (lastKnownBw2Frame && funcWatch.CurrentFrame() - lastKnownBw2Frame <= 45)
                return lastKnownBw2;
            // Both screens one colour (a fade to black or white between two screens, however
            // long): the last layout stays, rather than the two screens side by side popping up.
            const bool topBlank = screens && AutoFocus::Blank(screens[0].pixels);
            const bool bottomBlank = screens && AutoFocus::Blank(screens[1].pixels);
            if (lastKnownBw2Frame && topBlank && bottomBlank)
                return lastKnownBw2;
            if (topBlank && bottomBlank)
            {
                // (Both one colour before any known screen, as the game starts: that colour
                // over the whole view.)
                only("one-screen", 0);
                sc.bgScreen = 0;
                return sc;
            }
            // One screen with nothing on it (one colour), as in the logos and the opening
            // before the title screen: the other one alone at full size, the bars beside it
            // in soft colours from its edges (white beside a white logo). A change between that and both screens waits until
            // it has lasted 6 frames, so that a screen fading out first does not flash a layout.
            static int shownAlone = -1, wantAlone = -1;
            static uint32_t wantSince = 0;
            const int alone = topBlank == bottomBlank ? -1 : topBlank ? 1 : 0;
            const uint32_t now = funcWatch.CurrentFrame();
            if (alone != wantAlone)
            {
                wantAlone = alone;
                wantSince = now;
            }
            if (wantAlone != shownAlone && now - wantSince >= 6) shownAlone = wantAlone;
            if (shownAlone >= 0)
            {
                only("one-screen", shownAlone);
                // Soft bars from its edges where those are plain (a logo on white or black);
                // beside a picture (the opening's painted movie) they would be streaks of its
                // edge colours: black bars there, as around any film.
                bool plainEdges = true;
                if (screens && screens[shownAlone].pixels)
                    for (int side = 0; side < 2 && plainEdges; side++)
                    {
                        int lo = 255 * 3, hi = 0;
                        for (int y = 0; y < 192; y++)
                        {
                            const uint32_t c = screens[shownAlone].pixels[y * 256 + (side ? 255 : 0)];
                            const int l = ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
                            lo = std::min(lo, l);
                            hi = std::max(hi, l);
                        }
                        plainEdges = hi - lo <= 90;
                    }
                sc.bgScreen = plainEdges ? shownAlone : -1;
                sc.bgX = 0;
                sc.bgY = 0;
                sc.softBars = plainEdges;
                return sc;
            }
            // (Platinum: a moment of an event in the overworld in which its own function stops for
            // a little longer, a fade with only a message on the top screen: the last layout up
            // to 2 seconds after the overworld last ran.)
            if (pt.on && lastKnownBw2Frame && funcWatch.Ran(pt.field, 120))
                return lastKnownBw2;
            sc = Stack("spread", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
            return sc;
        }
        lastKnownBw2 = sc;
        lastKnownBw2Frame = funcWatch.CurrentFrame();
        return sc;
    };

    auto pickScene = [&](bool cursorInUse, float viewWidth, const ScreenImage* screens) {
        Scene sc;
        auto only = [&](const char* name, int screen) {
            sc.name = name;
            sc.items = { FullScreen(screen) };
        };
        if (singleView())
        {
            // The game has put everything on the visible screen: the world is widened on a wide
            // view, a battle's edges are continued, and any other screen's bars take its colours.
            only("single", 0);
            if (sectionActive("OVY_12"))
            {
                sc.name = "battle";
                // The bag and party screens are 4:3 pages: no scene to continue past their edges.
                sc.edgeFill = viewWidth >= 300 && !singleScreenFlag[3];
                // The ROM says which command panel is up (1 main, 2 fight only, 4 moves) and what
                // is chosen. On a wide view they stand as a strip along the bottom, as in the
                // recomp of the original game, with the scene above left alone.
                const int menu = singleScreenFlag[1], sel = singleScreenFlag[2];
                if (menu != stripPubMenu || sel != stripPubSel) { stripPubMenu = menu; stripPubSel = sel; stripSel = sel; }
                // Same moves as the ROM's main menu: left/right along the row, up/down between rows of 2.
                if (menu == 1 || menu == 2)
                    for (char k : stripNav)
                        stripSel = k == 'L' ? std::max(0, stripSel - 1) : k == 'R' ? std::min(3, stripSel + 1)
                                 : k == 'U' ? (stripSel >= 2 ? stripSel - 2 : stripSel)
                                            : (stripSel < 2 ? stripSel + 2 : stripSel);
                stripNav.clear();
                const int shown = (menu == 1 || menu == 2) ? stripSel : sel;
                if (viewWidth >= 300 && (menu == 1 || menu == 2 || menu == 4))
                {
                    const float L = 128 - viewWidth / 2;
                    sc.name = menu == 4 ? "battle-moves" : "battle-cmd";
                    sc.items.push_back(FillRect(L, 137, (int)viewWidth + 1, 55, 0x20242C));
                    sc.items.push_back(FillRect(L, 137, (int)viewWidth + 1, 1, 0x5A6270));
                    Element label = { 0, 16, 153, 62, 16, L + 4.5f, 139.5f, 1.0f, 0, 255, false };
                    Element label2 = { 0, 16, 169, 80, 16, L + 64.5f, 139.5f, 1.0f, 0, 255, false };
                    if (menu != 4) {
                        sc.items.push_back(FillRect(L + 4, 139, 141, 16, 0xFFFFFF));
                        sc.items.push_back(label);
                        sc.items.push_back(label2);
                    }
                    const size_t firstButton = sc.items.size();
                    const float gap = 6;
                    const bool moves = menu == 4;
                    const int bw = moves ? 78 : 50, bh = moves ? 22 : 18;
                    const float sb = std::min(33.0f / bh, (viewWidth - 8 - 3 * gap) / (4 * bw));
                    float x = 128 - (4 * bw * sb + 3 * gap) / 2;
                    const float y = 156.5f + (33 - bh * sb) / 2;
                    for (int i = 0; i < 4; i++)
                    {
                        const int sx = moves ? 5 + (i & 1) * 80 : 133 + (i & 1) * 52;
                        const int sy = moves ? 141 + (i >> 1) * 24 : 149 + (i >> 1) * 20;
                        Element b = { 0, sx, sy, bw, bh, x, y, sb, 0, 255, false };
                        b.border = true;
                        b.borderColor = i == shown ? 0xF04040 : 0x101218;
                        b.borderWidth = i == shown ? 16 : 6;
                        sc.items.push_back(b);
                        x += bw * sb + gap;
                    }
                    (void)firstButton;
                }
            }
            else
            {
                if (sectionActive("field")) singleFieldFrame = rc_current_frame();
                sc.wide = viewWidth >= 300 && (fieldActive() || (singleFieldFrame && rc_current_frame() - singleFieldFrame < 20));
            }
            return sc;
        }
        auto startMenu = [&]() {
            // (SAVE chosen: its screen, the saved game's orange card, is up on the touch screen
            // while the start menu's task still runs, for a second or two before the save's own
            // functions do: that screen whole, not cut to the menu's panel.)
            if (screens && screens[1].pixels)
            {
                auto orange = [&](int x, int y) {
                    const uint32_t c = screens[1].pixels[y * 256 + x];
                    const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
                    return r > 200 && g > 100 && g < 190 && b < 110;
                };
                if (orange(40, 20) && orange(20, 60) && orange(236, 20))
                {
                    only("save", 1);
                    return;
                }
            }
            only("start-menu", 0);
            // (Not while the touch screen is black, the menu closing into an application or
            // the save: it was a black box over the world for a few frames.)
            if (screens && screens[1].pixels && AutoFocus::Blank(screens[1].pixels)) return;
            // On a wide view the panel stands at the right edge, mostly beside the overworld.
            const float menuScale = viewWidth >= 300 ? 0.8f : 0.75f;
            const float menuX = viewWidth >= 300 ? 128 + viewWidth / 2 - 161 * menuScale - 2 : 131;
            sc.items.push_back({ 1, 4, 0, 161, 192, menuX, (192 - 192 * menuScale) / 2, menuScale, 0, 255, false });
            sc.items.back().border = !AutoFocus::Blank(screens[1].pixels);
        };

        // The safety net below must not flash up for the few frames in which the field map is
        // not running during a door transition, so its condition has to hold for a while.
        static int spreadFrames = 0;
        if (std::any_of(std::begin(probeTouch), std::end(probeTouch), probeActive) && !fieldActive() &&
            !appActive("Battle") && !appActive("TitleScreen") && !appActive("IntroMovie") && !appActive("Credits"))
            spreadFrames++;
        else
            spreadFrames = 0;
        if (focus.manualFlip)
            only("manual", focus.wantBottom ? 0 : 1);
        else if (appActive("MainMenu") && viewWidth >= 300)
        {
            // The start menu (CONTINUE with the saved game, NEW GAME, ...) is all on the touch
            // screen; the top one is black. That screen at full height, its plain blue continued
            // to the view's edges instead of black bars.
            only("launch-menu", 1);
            sc.bgScreen = 1;
            sc.bgX = 2;
            sc.bgY = 96;
        }
        else if (appActive("TitleScreen") && !AutoFocus::Blank(screens[0].pixels) && !AutoFocus::Blank(screens[1].pixels))
            // The title screen (the logo above, Ho-Oh or Lugia flying below): the two side by
            // side, as the other games show theirs, instead of the logo alone. (Not the opening
            // movie: its two screens are one tall picture, the sky above the sea.)
            sc = Stack("title", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
        else if (appActive("IntroMovie") && !AutoFocus::Blank(screens[1].pixels))
            // The opening movie: its two screens are one tall picture (the night sky above, the
            // sea with the moon and the legendary bird below, where most of it happens), shown
            // as on a DS, one above the other; the bottom one alone while the top is one colour.
            sc = AutoFocus::Blank(screens[0].pixels) ? Stack("movie", { Crop(1, 0, 0, 256, 192) }, false, viewWidth)
                                                      : Stack("movie", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
        else if (appActive("PokemonSummary") && AutoFocus::Blank(screens[0].pixels))
            // Fading in or out: black, not the card's empty frames on black.
            only("summary", 0);
        else if (appActive("PokemonSummary"))
        {
            // The page's card is one tall card split across the screens; its two halves stand
            // side by side after the column with the page's name and the Pokemon's name,
            // picture and item, on the page's colour. The page tabs and the party grid are
            // left out; the D-pad does both. The game frames the card on its right only (it
            // runs off the screen on the left), so the halves are cut from inside that frame
            // and each gets an even frame of its own, with what is drawn on it centred (the
            // moves list, for one, stands left of the middle).
            const uint32_t page = screens[0].pixels[100 * 256 + 1] & 0xFFFFFF;
            const uint32_t frame = 0x414141;
            Element side = Crop(0, 152, 8, 104, 184), upper = Crop(0, 0, 22, 147, 170), lower = Crop(1, 0, 0, 147, 160);
            // The lower half keeps the EXP bar, which overlaps the frame's bottom edge: the
            // edge's shading and its rounded corner are left out around it.
            lower.keys[0] = screens[1].pixels[158 * 256 + 1] & 0xFFFFFF;
            lower.keyRing = 2;
            lower.cutX = 143;
            lower.cutY = 154;
            const float pad = 4, gap = 6, margin = 6, h = 189;
            const int cardW = upper.sw;
            const float w = side.sw + 2 * (cardW + 2 * pad) + 2 * gap;
            const float scale = std::min((192 - 2 * margin) / h, (viewWidth - 2 * margin) / w);
            float x = 128 - w * scale / 2;
            const float y = 96 - h * scale / 2;
            sc.name = "summary";
            {
                // The name and item boxes run off the screen at the right, and the item box
                // off its bottom too: they are closed with mirrored copies of their left and
                // top edges, and the column's shaded strip at the right is painted over
                // beside them, as is the header's shaded end.
                const uint32_t* px = screens[0].pixels;
                const uint32_t column = px[100 * 256 + 154] & 0xFFFFFF;
                Element back = FillRect(x, y, side.sw, (int)h, column);
                back.scale = scale;
                back.border = true;
                back.borderColor = frame;
                back.borderRadius = 3;
                sc.items.push_back(back);
                auto at = [&](Element e, int sx, int sy) {
                    e.scale = scale;
                    e.dx = x + (sx - 152) * scale;
                    e.dy = y + (sy - 8) * scale;
                    sc.items.push_back(e);
                };
                at(side, 152, 8);
                at(FillRect(0, 0, 10, 16, px[15 * 256 + 153] & 0xFFFFFF), 246, 8);
                auto inBox = [&](int sy) {
                    for (int sx = 156; sx <= 158; sx++)
                        if ((px[sy * 256 + sx] & 0xFFFFFF) == frame) return true;
                    return false;
                };
                for (int sy = 26; sy < 192;)
                {
                    int end = sy;
                    while (end < 192 && inBox(end) == inBox(sy)) end++;
                    if (!inBox(sy)) at(FillRect(0, 0, 4, end - sy, column), 246, sy);
                    sy = end;
                }
                Element right = Crop(0, 152, 26, 6, 166);
                right.flipX = true;
                at(right, 250, 26);
                Element bottom = Crop(0, 152, 156, 98, 2);
                bottom.flipY = true;
                at(bottom, 152, 192);
                Element corner = Crop(0, 152, 156, 6, 2);
                corner.flipX = corner.flipY = true;
                at(corner, 250, 192);
            }
            x += (side.sw + gap) * scale;
            for (Element* e : { &upper, &lower })
            {
                Element back = FillRect(x, y, cardW + 2 * (int)pad, (int)h, page);
                back.scale = scale;
                back.border = true;
                back.borderColor = frame;
                back.borderRadius = 3;
                sc.items.push_back(back);
                // What is drawn on the card comes in blocks, one above the other with the page's
                // colour between them; each block is centred across the panel, and all of them
                // together top to bottom.
                const uint32_t* px = screens[e->screen].pixels;
                const uint32_t edge = e->keys[0];
                auto drawn = [&](int sx, int sy) {
                    const uint32_t c = px[sy * 256 + sx] & 0xFFFFFF;
                    return c != page && !(sy >= e->sy + e->sh - e->keyRing && c == edge) &&
                        !e->Skips(sx, sy);
                };
                struct Block { int y0, y1, x0, x1; };
                std::vector<Block> blocks;
                int blank = 2;
                for (int sy = e->sy; sy < e->sy + e->sh; sy++)
                {
                    int r0 = cardW, r1 = -1;
                    for (int sx = 0; sx < cardW; sx++)
                        if (drawn(sx, sy))
                        {
                            r0 = std::min(r0, sx);
                            r1 = sx;
                        }
                    if (r1 < 0)
                    {
                        blank++;
                        continue;
                    }
                    if (blank >= 2 || blocks.empty())
                        blocks.push_back({ sy, sy, r0, r1 });
                    Block& bl = blocks.back();
                    bl.y1 = sy;
                    bl.x0 = std::min(bl.x0, r0);
                    bl.x1 = std::max(bl.x1, r1);
                    blank = 0;
                }
                if (!blocks.empty())
                {
                    const float top = y + (pad + (h - 2 * pad - (blocks.back().y1 + 1 - blocks[0].y0)) / 2.0f) * scale;
                    for (const Block& bl : blocks)
                    {
                        Element piece = *e;
                        piece.sx = bl.x0;
                        piece.sw = bl.x1 - bl.x0 + 1;
                        piece.sy = bl.y0;
                        piece.sh = bl.y1 - bl.y0 + 1;
                        piece.scale = scale;
                        piece.dx = x + (pad + (cardW - piece.sw) / 2.0f) * scale;
                        piece.dy = top + (bl.y0 - blocks[0].y0) * scale;
                        sc.items.push_back(piece);
                        // A box the game runs off the screen or into the card's frame (the
                        // ability, the number of ribbons) gets sides of its own.
                        if (bl.x0 == 0 || bl.x1 == cardW - 1)
                            for (float bx : { piece.dx - 2 * scale, piece.dx + piece.sw * scale })
                            {
                                Element side = FillRect(bx, piece.dy, 2, piece.sh, frame);
                                side.scale = scale;
                                sc.items.push_back(side);
                            }
                    }
                }
                x += (cardW + 2 * pad + gap) * scale;
            }
            sc.bgScreen = 0;
            sc.bgX = 1;
            sc.bgY = 100;
        }
        else if (appActive("Bag"))
            // The bag screen with the description of the chosen item above it (the box at the
            // bottom of the other screen, rows 142-191; the picture of the bag above it is left
            // out).
            sc = Stack("bag", { Crop(0, 0, 142, 256, 50), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
        else if (appActive("PCBox"))
            // The selected Pokemon's info card from the other screen beside the box.
        {
            sc = SideBySide("pc-box", Crop(0, 0, 34, 216, 156), Crop(1, 0, 0, 256, 192), viewWidth);
            sc.bgScreen = 0;
            sc.bgX = 236;
            sc.bgY = 100;
        }
        else if (appActive("Pokedex"))
        {
            // Every Pokedex page fills both screens, so they are joined into one page: as one
            // tall picture for the closed cover (red at both left edges), otherwise as a spread
            // with the entry on the left and the list or controls on the right.
            auto red = [&](int screen) {
                uint32_t c = screens[screen].pixels[100 * 256 + 10];
                return ((c >> 16) & 0xFF) > 180 && ((c >> 8) & 0xFF) < 100 && (c & 0xFF) < 100;
            };
            bool cover = red(0) && red(1);
            uint32_t title = screens[0].pixels[8 * 256 + 200];
            auto light = [](uint32_t c) { return ((c >> 16) & 0xFF) > 235 && ((c >> 8) & 0xFF) > 235 && (c & 0xFF) > 235; };
            // The list page has a red title bar and, once the cover has slid away, bare paper
            // between the picture and the description.
            int bare = 0;
            for (int y = 127; y < 131; y++)
                for (int x = 4; x < 96; x++) bare += light(screens[0].pixels[y * 256 + x]);
            bool listPage = ((title >> 16) & 0xFF) > 200 && ((title >> 8) & 0xFF) < 90 && (title & 0xFF) < 90 && bare > 280;
            // The detail pages have a coloured title bar on the top screen and a blue tab bar
            // at the foot of the bottom screen.
            uint32_t tab = screens[1].pixels[176 * 256 + 63];
            bool tabPage = !light(title) && (tab & 0xFF) > 150 && ((tab >> 16) & 0xFF) < 120 && !listPage &&
                (((title >> 16) & 0xFF) < 200 || ((title >> 8) & 0xFF) > 90);
            auto put = [&](Element e, float x, float y, float scale) {
                e.dx = x;
                e.dy = y;
                e.scale = scale;
                sc.items.push_back(e);
            };
            const float W = viewWidth, L = 128 - W / 2;
            if (cover || W < 300)
                sc = Stack("pokedex", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, !cover, viewWidth);
            else if (listPage)
            {
                // The list as one page: the pieces of the entry on the top screen are laid out
                // again around the number grid. A title bar runs across the top; on the left
                // are the name, the picture and the type and size card; on the right the
                // description sits above the grid and its buttons.
                sc.name = "pokedex-list";
                sc.items.clear();
                // The page is drawn on plain paper: the two most common colours in an empty
                // strip of the top screen are the paper and its ruling, and the ruling is
                // left out of the rearranged pieces so that they do not show as patches.
                uint32_t paper = 0;
                std::vector<std::pair<int, uint32_t>> ruling;
                {
                    // Sampled from the empty area between the picture and the description.
                    std::map<uint32_t, int> count;
                    for (int y = 113; y < 131; y++)
                        for (int x = 4; x < 96; x++) count[screens[0].pixels[y * 256 + x] & 0xFFFFFF]++;
                    int best = 0;
                    for (auto& c : count)
                        if (c.second > best)
                        {
                            best = c.second;
                            paper = c.first;
                        }
                    uint32_t line = paper;
                    int lineCount = 0;
                    for (auto& c : count)
                        if (c.first != paper && c.second > lineCount)
                        {
                            lineCount = c.second;
                            line = c.first;
                        }
                    // The ruling also has lighter greys where lines cross; those are between
                    // the line colour and the paper, and losing one inside a picture would
                    // hardly show.
                    count.clear();
                    for (int y = 24; y < 126; y++)
                        for (int x = 4; x < 100; x++) count[screens[0].pixels[y * 256 + x] & 0xFFFFFF]++;
                    for (auto& c : count)
                    {
                        uint32_t r = (c.first >> 16) & 0xFF, g = (c.first >> 8) & 0xFF, bl = c.first & 0xFF;
                        if (r == g && g == bl && r > (line & 0xFF) && r < (paper & 0xFF)) ruling.push_back({ -c.second, c.first });
                    }
                    std::sort(ruling.begin(), ruling.end());
                    if (ruling.size() > 3) ruling.resize(3);
                    if (line != paper) ruling.push_back({ 0, line });
                    sc.bgScreen = 0;
                    for (int i = 127 * 256 + 4; i < 131 * 256; i++)
                        if ((screens[0].pixels[i] & 0xFFFFFF) == paper)
                        {
                            sc.bgX = i % 256;
                            sc.bgY = i / 256;
                            break;
                        }
                }
                auto putKeyed = [&](Element e, float x, float y, float scale) {
                    for (size_t k = 0; k < 4 && k < ruling.size(); k++) e.keys[k] = ruling[k].second;
                    put(e, x, y, scale);
                };
                const float t = 0.77f, barH = 17 * t;
                for (float x = L; x < L + W; x += 100 * t) put(Crop(0, 140, 0, 100, 17), x, 0, t);
                put(Crop(0, 0, 0, 80, 17), L, 0, t);
                const float leftW = W * 0.43f, rightW = W - leftW, rx = L + leftW;
                const float nameW = 156 * t;
                putKeyed(Crop(0, 100, 21, 156, 39), L + (leftW - nameW) / 2, barH + 3, t);
                const float statsY = 192 - 64 * t - 2;
                putKeyed(Crop(0, 100, 62, 156, 64), L + (leftW - nameW) / 2, statsY, t);
                const float picTop = barH + 3 + 39 * t, picS = std::min((statsY - picTop) / 102, leftW / 96);
                putKeyed(Crop(0, 4, 24, 96, 102), L + (leftW - 96 * picS) / 2, picTop + (statsY - picTop - 102 * picS) / 2, picS);
                const float descS = std::min(t, (rightW - 4) / 248), descH = 56 * descS;
                putKeyed(Crop(0, 4, 132, 248, 56), rx + (rightW - 248 * descS) / 2, barH + 2, descS);
                const float gridTop = barH + 2 + descH + 1, gridS = std::min((192 - gridTop) / 192, rightW / 256);
                put(Crop(1, 0, 0, 256, 192), rx + (rightW - 256 * gridS) / 2, 192 - 192 * gridS, gridS);
            }
            else if (!tabPage)
            {
                // Search and anything else: both screens as the two columns of a page.
                sc = Stack("pokedex", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
                if (light(screens[0].pixels[2 * 256 + 250]))
                {
                    sc.bgScreen = 0;
                    sc.bgX = 250;
                    sc.bgY = 2;
                }
            }
            else
            {
                // The other pages (area, size, forms): the title bar of the top screen and the
                // tab bar of the bottom screen frame the whole view, with the two bodies as
                // the columns of one page between them.
                sc.name = "pokedex-page";
                sc.items.clear();
                for (int i = 24 * 256; i < 192 * 256; i++)
                {
                    uint32_t c = screens[0].pixels[i];
                    if (((c >> 16) & 0xFF) > 235 && ((c >> 8) & 0xFF) > 235 && (c & 0xFF) > 235)
                    {
                        sc.bgScreen = 0;
                        sc.bgX = i % 256;
                        sc.bgY = i / 256;
                        break;
                    }
                }
                const float headH = 18, footH = 28, room = 192 - headH - footH;
                const float b = std::min(W / 512, room / 170);
                put(Crop(0, 0, 21, 256, 169), L + (W / 2 - 256 * b) / 2, headH + (room - 169 * b) / 2, b);
                put(Crop(1, 0, 0, 256, 162), 128 + (W / 2 - 256 * b) / 2, headH + (room - 162 * b) / 2, b);
                for (float x = L + 200; x < L + W; x += 40) put(Crop(0, 100, 0, 40, 18), x, 0, 1);
                put(Crop(0, 0, 0, 220, 18), L, 0, 1);
                for (float x = L; x < L + W; x += 6) put(Crop(1, 61, 162, 6, 28), x, 192 - footH, 1);
                put(Crop(1, 0, 162, 256, 28), 0, 192 - footH, 1);
            }
        }
        else if (appActive("GearMap") || appActive("TownMap"))
        {
            // The location's name and description from the other screen beside the map.
            sc = SideBySide("map", Crop(0, 0, 64, 256, 128), Crop(1, 0, 0, 256, 192), viewWidth);
            sc.bgScreen = 0;
            sc.bgX = 128;
            sc.bgY = 58;
        }
        else if (appActive("GearPhone") && screens[1].flags &&
            LayerClass(screens[1].flags[170 * 256 + 30]) == LAYER_OBJ)
            // A call in progress (the contact list is inactive and a sprite panel covers its
            // tab bar): the caller's words are in a box on the top screen.
            only("call", 0);
        else if (appActive("GearPhone"))
            // The clock and call text from the other screen above the contact list.
            sc = Stack("phone", { Crop(0, 0, 16, 256, 58), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
        else if (appActive("ChooseStarter"))
        {
            // The Pokemon preview and the professor's text, with the Poke Balls from the
            // other screen inset in a corner.
            only("starter", 0);
            Element balls = Crop(1, 0, 0, 256, 150);
            if (viewWidth >= 300)
            {
                // Wide view: the preview moves to the left edge and the tray of Poke Balls
                // stands in the space that frees up on the right.
                const float L = 128 - viewWidth / 2, R = 128 + viewWidth / 2;
                sc.items[0].dx = L;
                balls.scale = std::min(0.62f, (viewWidth - 256 + 36) / 256);
                balls.dx = R - 256 * balls.scale;
                balls.dy = 72 - 75 * balls.scale;
            }
            else
            {
                balls.dx = 3;
                balls.dy = 3;
                balls.scale = 0.42f;
            }
            balls.border = viewWidth >= 300 && !AutoFocus::Blank(screens[1].pixels);
            sc.items.push_back(balls);
        }
        else if (appActive("NamingScreen"))
            only("naming", 1);
        else if (appActive("OakSpeech") && AutoFocus::Blank(screens[0].pixels) && !AutoFocus::Blank(screens[1].pixels))
            // Pages of the intro that use the bottom screen alone.
            only("bottom", 1);
        else if (appActive("OakSpeech"))
        {
            // The professor on the top screen; choices from the bottom screen (its backdrop art
            // dropped) are placed where they hide the least text.
            only("intro", 0);
            Element el;
            const uint8_t drop = LAYER_BG3 | LAYER_BACKDROP;
            if (viewWidth >= 300)
            {
                // Wide view: the professor's screen moves to the left edge and its backdrop,
                // which is the same all the way across, is continued to the right edge. The
                // choices from the bottom screen (buttons, the boy and the girl) stand in
                // that extra space instead of on top of the professor.
                const float L = 128 - viewWidth / 2, R = 128 + viewWidth / 2, free = viewWidth - 256;
                // Everything on the bottom screen that is not its plain background colour is a
                // choice: buttons, or the cards of the boy and the girl. The cards and the red
                // frame around the selected one are part of the background layer, so the
                // pieces are found and cut out by colour rather than by layer.
                const uint32_t plain = screens[1].pixels[96 * 256 + 2] & 0xFFFFFF;
                // A faint Poke Ball is sometimes drawn into that background in nearly the
                // same blue; it counts as background too.
                auto nearPlain = [&](uint32_t c) {
                    return std::abs((int)((c >> 16) & 0xFF) - (int)((plain >> 16) & 0xFF)) <= 56 &&
                        std::abs((int)((c >> 8) & 0xFF) - (int)((plain >> 8) & 0xFF)) <= 56 &&
                        std::abs((int)(c & 0xFF) - (int)(plain & 0xFF)) <= 56;
                };
                int bx0 = 256, by0 = 192, bx1 = -1, by1 = -1;
                for (int y = 8; y < 184; y++)
                    for (int x = 0; x < 256; x++)
                        if (!nearPlain(screens[1].pixels[y * 256 + x] & 0xFFFFFF))
                        {
                            bx0 = std::min(bx0, x);
                            bx1 = std::max(bx1, x);
                            by0 = std::min(by0, y);
                            by1 = std::max(by1, y);
                        }
                // Alone in the lower right corner there is only a "touch to continue" hint.
                const bool found = bx1 >= bx0 && bx1 - bx0 < 250 && !(bx0 >= 144 && by0 >= 128);
                // The box around the choices only ever grows while they are up: a button that
                // flashes when pressed must not make the whole group jump about.
                static int cx0, cy0, cx1, cy1, cedge;
                static unsigned choiceSeen = 0;
                const unsigned now = rc_current_frame();
                const bool fresh = !choiceSeen || now - choiceSeen > 40;
                if (found)
                {
                    int edge = 0;
                    if (screens[0].flags)
                        for (int y = 8; y < 140; y++)
                            for (int x = 255; x > edge; x--)
                                if (!(LayerClass(screens[0].flags[y * 256 + x]) & drop))
                                {
                                    edge = x;
                                    break;
                                }
                    cx0 = fresh ? bx0 : std::min(cx0, bx0);
                    cy0 = fresh ? by0 : std::min(cy0, by0);
                    cx1 = fresh ? bx1 : std::max(cx1, bx1);
                    cy1 = fresh ? by1 : std::max(cy1, by1);
                    cedge = fresh ? edge : std::max(cedge, edge);
                    choiceSeen = now;
                }
                const bool showing = choiceSeen && now - choiceSeen <= 40;
                // While he only talks, his screen is in the middle. For a choice it slides to
                // the left edge and the choices stand in the space that frees up beside him,
                // instead of on top of him. His backdrop is the same all the way across, so it
                // is continued to both edges of the view.
                static float slide = 0;
                static unsigned slideFrame = 0;
                if (now != slideFrame)
                {
                    slide += std::clamp((showing ? 1.0f : 0.0f) - slide, -1.0f / 14, 1.0f / 14);
                    slideFrame = now;
                }
                const float ease = slide * slide * (3 - 2 * slide);
                const float picX = L * ease;
                sc.items[0].dx = picX;
                for (float x = picX - 2; x > L - 2; x -= 2)
                {
                    Element strip = Crop(0, 0, 0, 2, 192);
                    strip.dx = x;
                    strip.crisp = true;
                    sc.items.push_back(strip);
                }
                for (float x = picX + 256; x < R; x += 2)
                {
                    Element strip = Crop(0, 0, 0, 2, 192);
                    strip.dx = x;
                    strip.crisp = true;
                    sc.items.push_back(strip);
                }
                if (showing)
                {
                    int x0 = std::max(0, cx0 - 2), y0 = std::max(0, cy0 - 2);
                    el = Crop(1, x0, y0, std::min(255, cx1 + 2) - x0 + 1, std::min(191, cy1 + 2) - y0 + 1);
                    el.keys[0] = plain;
                    el.keyTolerance = 56;
                    // They may reach over the right of the professor's screen as far as
                    // there is nothing but backdrop there (above the message box).
                    float room = R - 3 - (L + cedge + 6);
                    if (room < free) room = free;
                    el.scale = std::min({ 1.25f, room / el.sw, 136.0f / el.sh });
                    float w = el.sw * el.scale, h = el.sh * el.scale;
                    el.dx = R - 3 - room + (room - w) / 2;
                    el.dy = 4 + (136 - h) / 2;
                    el.alpha = (uint8_t)(255 * ease);
                    sc.items.push_back(el);
                }
            }
            else if (SmartOverlay(screens, 1, 160, 144, drop, 180, 120, LAYER_BG0, 255, el) &&
                SmartOverlay(screens, 1, 200, 152, drop, 180, 120, LAYER_BG0, 255, el))
                // The lower right corner only ever holds a "touch to continue" hint.
                sc.items.push_back(el);
        }
        else if (std::any_of(std::begin(probeSave), std::end(probeSave), probeActive) &&
                 // Closing, the X menu's save step runs on for a few frames after the save
                 // screen is gone and the menu is back.
                 (sectionActive("touch_save_app") || !probeActive(probeStartMenu)))
            only("save", 1);
        else if (probeActive(probeMart))
            // Poke Mart: the item's description from the other screen above the list of goods.
        {
            sc = Stack("mart", { Crop(0, 0, 140, 256, 52), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
            sc.bgScreen = 1;
            sc.bgX = 6;
            sc.bgY = 100;
        }
        else if (std::any_of(std::begin(probeChoice), std::end(probeChoice), probeActive))
        {
            // A question asked in the overworld: the answer buttons are on the bottom screen.
            // They are drawn, without that screen's background, at the right above the message box.
            only("choice", 0);
            Element el;
            if (SmartOverlay(screens, 1, 256, 192, LAYER_BG2 | LAYER_BG3 | LAYER_BACKDROP, 168, 136, 0, 255, el))
            {
                // Keep the piece the same while the question is up, growing it if needed, so
                // that a button flashing does not move or resize the group.
                static Element held;
                static unsigned heldAt = 0;
                const unsigned now = rc_current_frame();
                if (heldAt && now - heldAt <= 8)
                {
                    int x0 = std::min(held.sx, el.sx), y0 = std::min(held.sy, el.sy);
                    int x1 = std::max(held.sx + held.sw, el.sx + el.sw), y1 = std::max(held.sy + held.sh, el.sy + el.sh);
                    el.sx = x0;
                    el.sy = y0;
                    el.sw = x1 - x0;
                    el.sh = y1 - y0;
                    el.scale = std::min({ 1.0f, 168.0f / el.sw, 136.0f / el.sh });
                }
                held = el;
                heldAt = now;
                el.dx = (viewWidth >= 300 ? 128 + viewWidth / 2 - 3 : 252) - el.sw * el.scale;
                el.dy = 142 - el.sh * el.scale;
                sc.items.push_back(el);
            }
        }
        else if (menuReturn > 0)
            startMenu();
        else if (probeActive(probeStartMenuWait) && !appBottomNow)
        {
            // An application chosen in the start menu is loading (the field prepares it for a
            // while before it starts): the menu while it fades, then the bottom screen,
            // where these applications appear, instead of the top screen and both.
            if (!AutoFocus::Blank(screens[1].pixels))
                startMenu();
            else
                only("bottom", 1);
        }
        else if (focus.wantBottom || appBottomNow)
            only("bottom", 1);
        else if (sectionActive("OVY_8") ||
                 // (Opening, the Bag or the party pauses for about 5 frames after it has started,
                 // with neither it nor the battle running: without this the battle would flash
                 // up at 4:3 in between.)
                 (sectionWithin("OVY_8", 12) && !sectionActive("OVY_12")))
            // In-battle bag and party screens (overlay 8) fill the bottom screen.
            only("battle-submenu", 1);
        else if (probeActive(probeBattleInput) ||
            // While the commands slide out and the moves in, the game stops reading the touch
            // screen for a few frames; the strip stays up meanwhile (see battleStripUntil).
            (frame < battleStripUntil && sectionActive("OVY_12") && !AutoFocus::Blank(screens[1].pixels)))
        {
            // Battle, choosing a command or a move. On a wide view the buttons are taken off
            // the touch screen one by one and laid out as a strip along the bottom, in place
            // of the message box (which only says "What will X do?" at that point): nothing
            // of the scene is covered. Which menu is up is read from the colours of its
            // buttons; while they slide in or out neither is recognised and no buttons are
            // shown. Anything else the battle asks on the touch screen (a target, yes or no)
            // has no strip of its own and is shown whole, as a card at the lower left.
            sc = gen4BattleInput(viewWidth, screens);
        }
        else if (probeActive(probeStartMenu) && fieldActive())
            startMenu();
        else if (spreadFrames >= 40)
            // Safety net for screens without a layout of their own: something is waiting for a
            // touch, so both screens are shown joined as one spread rather than hiding either.
            sc = Stack("spread", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
        else if (cursorInUse)
            // Nothing on screen maps to the touchscreen, so show it while the cursor is out.
            only("bottom", 1);
        else
        {
            only("top", 0);
            if (sectionActive("OVY_12"))
            {
                // A battle outside its menus: messages and animations.
                sc.name = "battle";
                sc.edgeFill = true;
            }
        }
        // The overworld is drawn across the whole wide view by the widescreen 3D renderer.
        sc.wide = viewWidth >= 300 && (fieldActive() || menuReturn > 0) && !sc.items.empty() && sc.items[0].screen == 0 &&
            sc.items[0].sw == 256 && sc.items[0].sh == 192 && sc.items[0].dx == 0 && sc.items[0].scale == 1.0f;
        return sc;
    };
    Scene scene;
    scene.items = { FullScreen(0) };
    std::vector<uint32_t> mergedImage;
    SDL_Texture* mergedTex = nullptr;
    GpuCompositor* gpuCompose = nullptr;
    BattleWide battleWide;
    const bool cpuCompose = getenv("SSR_CPU_COMPOSE") != nullptr;
    int mergedTexW = 0, mergedTexH = 0;

    // Test navigator (--nav): walks the player to a list of tiles, learning which moves
    // are blocked as it goes and routing around them.
    struct NavState
    {
        std::vector<std::pair<int, int>> way;
        std::vector<int> push;
        int pushDir = -1;
        size_t idx = 0;
        std::set<std::tuple<int, int, int>> blocked;
        int dir = -1, lastDir = -1, fromX = -1, fromZ = -1, map = -1;
        uint32_t since = 0, started = 0;
        bool active = false;
        bool outdoor = true; // the permission grid only describes the overworld matrix
    } navState;
    {
        std::istringstream ss(opt.nav);
        std::string item;
        while (std::getline(ss, item, ';'))
        {
            int x, z;
            char push = 0;
            // "x,z" or "x,z,U|D|L|R": on arriving, keep walking that way until the map changes (a door).
            int n = sscanf(item.c_str(), "%d,%d,%c", &x, &z, &push);
            if (n >= 2)
            {
                navState.way.push_back({ x, z });
                const char* dirs = "UDLR";
                const char* p = n == 3 ? strchr(dirs, push) : nullptr;
                navState.push.push_back(p ? (int)(p - dirs) : -1);
            }
        }
        navState.active = !navState.way.empty();
    }
    auto readRam = [&](uint32_t a) -> uint32_t {
        if (a < 0x02000000 || a >= 0x02400000) return 0;
        uint32_t v;
        memcpy(&v, nds->MainRAM + (a & nds->MainRAMMask), 4);
        return v;
    };
    // sFieldSysPtr -> location (map id) and playerAvatar -> mapObject -> current tile.
    auto playerPos = [&](int& map, int& x, int& z) {
        uint32_t fs = readRam(0x021D4158), loc = readRam(fs + 0x20), obj = readRam(readRam(fs + 0x40) + 0x30);
        map = (int)readRam(loc);
        x = (int)readRam(obj + 0x64);
        z = (int)readRam(obj + 0x6C);
        return obj != 0;
    };
    // Movement permissions of the overworld, if given: lets the navigator plan around
    // walls and one-way ledges instead of discovering them.
    struct NavGrid { int w = 0, h = 0; std::vector<uint16_t> cells; };
    auto loadNavGrid = [](const std::string& path) {
        NavGrid g;
        std::ifstream f(path, std::ios::binary);
        uint16_t dims[2] = {};
        f.read((char*)dims, 4);
        g.w = dims[0];
        g.h = dims[1];
        g.cells.resize((size_t)g.w * g.h);
        f.read((char*)g.cells.data(), g.cells.size() * 2);
        return g;
    };
    NavGrid navOverworld;
    std::map<int, NavGrid> navInterior;
    if (!opt.navMap.empty()) navOverworld = loadNavGrid(opt.navMap);
    for (auto& e : opt.navMapIds) navInterior[e.first] = loadNavGrid(e.second);
    static const int navDX[4] = { 0, 0, -1, 1 }, navDZ[4] = { -1, 1, 0, 0 };
    // Where a step in direction d from (x, z) ends up; false if it cannot be made.
    auto navMove = [&](int x, int z, int d, int& ox, int& oz) -> bool {
        if (navState.blocked.count({ x, z, d })) return false;
        ox = x + navDX[d];
        oz = z + navDZ[d];
        const NavGrid* grid = &navOverworld;
        if (!navState.outdoor)
        {
            auto it = navInterior.find(navState.map);
            grid = it == navInterior.end() ? nullptr : &it->second;
        }
        if (!grid || grid->cells.empty() || navState.map < 0) return true;
        auto perm = [&](int px, int pz) -> uint16_t {
            return px < 0 || pz < 0 || px >= grid->w || pz >= grid->h ? 0x8000 : grid->cells[(size_t)pz * grid->w + px];
        };
        if (navState.idx < navState.way.size() && navState.way[navState.idx] == std::make_pair(ox, oz)) return true; // doors
        uint16_t p = perm(ox, oz);
        int b = p & 0xFF;
        // Ladders, stairs and door mats warp: only step on one when it is the waypoint.
        if ((b >= 0x3C && b <= 0x3F) || (b >= 0x5E && b <= 0x6F)) return false;
        if (b >= 0x38 && b <= 0x3B)
        {
            // Ledge: 0x38 jumps right, 0x39 left, 0x3A up, 0x3B down, landing on the far side.
            static const int ledgeDir[4] = { 3, 2, 0, 1 };
            if (ledgeDir[b - 0x38] != d) return false;
            ox += navDX[d];
            oz += navDZ[d];
            return !(perm(ox, oz) & 0x8000);
        }
        return !(p & 0x8000);
    };
    // Next direction (0 up, 1 down, 2 left, 3 right) on a shortest path.
    auto navStep = [&](int x, int z, int tx, int tz) -> int {
        const int R = 64;
        if (x < 0 || z < 0 || x > 4096 || z > 4096) return -1; // not a position (mid-warp)
        int x0 = std::min(x, tx) - R, z0 = std::min(z, tz) - R;
        int w = std::abs(x - tx) + 2 * R + 1, h = std::abs(z - tz) + 2 * R + 1;
        std::vector<int8_t> first((size_t)w * h, -1);
        std::deque<std::pair<int, int>> q;
        q.push_back({ x, z });
        first[(size_t)(z - z0) * w + (x - x0)] = 4;
        while (!q.empty())
        {
            auto [cx, cz] = q.front();
            q.pop_front();
            int8_t f = first[(size_t)(cz - z0) * w + (cx - x0)];
            if (cx == tx && cz == tz) return f;
            for (int d = 0; d < 4; d++)
            {
                int nx, nz;
                if (!navMove(cx, cz, d, nx, nz)) continue;
                if (nx < x0 || nz < z0 || nx >= x0 + w || nz >= z0 + h) continue;
                int8_t& slot = first[(size_t)(nz - z0) * w + (nx - x0)];
                if (slot != -1) continue;
                slot = f == 4 ? (int8_t)d : f;
                q.push_back({ nx, nz });
            }
        }
        return -1;
    };
    uint32_t scriptClock = frame; // equals frame unless --battle-mash holds it during battles

    Canvas canvas;
    Menu menu;
    // The pause menu's items (menuIds: the item in each row) and its pages (menuPage): the menu
    // itself, then DISPLAY, GRAPHICS and CONTROLS, and in CONTROLS the keys and buttons
    // (PAGE_KEYS, controls.h). Items a platform does not have are left out of its pages.
    enum
    {
        M_RESUME, M_LAYOUT, M_LARGE, M_FPS60, M_FULLSCREEN, M_SMOOTH, M_WIDE, M_WIDEMAP, M_BATTLEBUTTONS,
        M_WIDELINES, M_AUTODETAIL, M_WIDEGPU, M_GPU3D, M_SHOWFPS, M_BUTTONS, M_PADTOUCH, M_EMULATION,
        M_CONTROLS, M_STARTBTN, M_FAST, M_FFLOWRES, M_SAVESTATE, M_LOADSTATE, M_RESET, M_QUIT,
        M_DISPLAY, M_GRAPHICS, M_CONTROLMENU, M_BACK, M_CHEATS, M_NOTHING,
        M_CHEATMENU, M_CHEATWIDTH, M_CHEATTEXT,
        M_CHEAT0 = 1000 // (M_CHEAT0 + i: visible row i, CHEATS)
    };
    enum { PAGE_MAIN, PAGE_KEYS, PAGE_DISPLAY, PAGE_GRAPHICS, PAGE_CONTROLS, PAGE_CHEATS, PAGE_CHEATMENU };
    std::vector<int> menuIds;
    std::vector<Cheats::Row> cheatRows;
    MenuLayout cheatLayout;
    MenuRepeat cheatRepeat;
    bool fastLock = false;
    double ffFrameMs = 0; // emulation time of one frame while fast forwarding (see speed)
    // World detail choices: the most lines the widescreen 3D is drawn at (192 is the DS's own).
    static const int detailLines[] = { 192, 288, 384, 540, 720, 1080, 1440, 2160 };
    const int detailCount = (int)(sizeof(detailLines) / sizeof(detailLines[0]));
    auto detailIndex = [&]() {
        int i = 0;
        while (i + 1 < detailCount && detailLines[i + 1] <= cfg.wideLines) i++;
        return i;
    };
    // CONTROLS (controls.h): a page of the pause menu with each action's key or button. Choosing
    // one waits for the key or button to give it (capturing, until captureUntil; a controller's
    // buttons must all be let go first, captureReleased); ESC, or 5 seconds, leaves it as it was.
    // CHANGING says which is shown and changed, the keyboard's keys or a controller's buttons.
    int menuPage = 0; // PAGE_*
#ifdef __SWITCH__
    const bool ctlHasKeyboard = false; // (the Switch: a controller only)
#else
    const bool ctlHasKeyboard = true;
#endif
    bool ctlKeyboard = false;
    int capturing = -1;
    uint32_t captureUntil = 0;
    bool captureReleased = false;
    bool navHoldOff = false; // (the button just given: ignored by the menu until let go)
    PadState physPad, prevPhysPad; // the controller as it is, before RemapPad
    const int ctlRow0 = ctlHasKeyboard ? 1 : 0; // the first action's row
    const int ctlRowReset = ctlRow0 + Controls::COUNT, ctlRowBack = ctlRowReset + 1;
    auto buildMenu = [&]() {
        if (menuPage == PAGE_KEYS)
        {
            menuIds.clear();
            // (The names on the first controller connected: Xbox's, PlayStation's or Nintendo's.)
            const int padStyle = Controls::StyleOf(pads.controllers.empty() ? nullptr : pads.controllers[0]);
            menu.items.clear();
            if (ctlHasKeyboard) menu.items.push_back({ "CHANGING", ctlKeyboard ? "KEYBOARD" : "CONTROLLER" });
            for (int a = 0; a < Controls::COUNT; a++)
            {
                std::string v = capturing == a ? (ctlKeyboard ? "PRESS A KEY" : "PRESS A BUTTON")
                              : ctlKeyboard ? Controls::KeyName(cfg.controls.Key(a))
                                            : Controls::InputName(cfg.controls.Pad(a, cfg.byLabel), padStyle);
                menu.items.push_back({ Controls::ActionName(a), v });
            }
            menu.items.push_back({ "RESET ALL", "" });
            menu.items.push_back({ "BACK", "" });
            return;
        }
#ifndef __SWITCH__
        static const char* large[] = { "AUTO", "TOP", "BOTTOM" };
#endif
        menu.items.clear();
        menuIds.clear();
        auto add = [&](int id, const char* label, const std::string& value, bool directional = false) {
            menu.items.push_back({ label, value, directional });
            menuIds.push_back(id);
        };
        auto onOff = [](bool on) { return std::string(on ? "ON" : "OFF"); };
        if (menuPage == PAGE_MAIN)
        {
            add(M_RESUME, "RESUME", "");
            add(M_SAVESTATE, "SAVE STATE", "");
            add(M_LOADSTATE, "LOAD STATE", "");
            add(M_FAST, "FAST FORWARD", onOff(fastLock));
            add(M_DISPLAY, "DISPLAY", "");
            add(M_GRAPHICS, "GRAPHICS", "");
            add(M_CONTROLMENU, "CONTROLS", "");
            add(M_CHEATMENU, "CHEAT UI", "");
            add(M_CHEATS, "CHEATS", cheats.On() ? std::to_string(cheats.On()) + " ON" : "");
            add(M_RESET, "RESET GAME", "");
            add(M_QUIT, "QUIT", "");
        }
        else if (menuPage == PAGE_DISPLAY)
        {
#ifndef __SWITCH__
            // (The Switch always shows the one wide screen these layouts are built around.)
            add(M_LAYOUT, "SCREEN LAYOUT", LayoutName(layout.mode));
            add(M_LARGE, "LARGE SCREEN", large[focus.mode]);
#if !defined(SSR_PHONE)
            // (Always full screen on the Switch and phones. Borderless: a window the size of the
            // screen, so other windows come up at once.)
            add(M_FULLSCREEN, "BORDERLESS FULLSCREEN", onOff(cfg.fullscreen));
#endif
#endif
            add(M_SMOOTH, "SMOOTH SCALING", onOff(cfg.smooth));
            // (Black and Black 2; SoulSilver has a layout of its own. Black 2 has no STRIP.)
            add(M_BATTLEBUTTONS, "BATTLE BUTTONS",
                !funcWatchOn || pt.on ? "-" : cfg.battleButtons == 2 ? "PANEL" : cfg.battleButtons == 0 || !bw1.on ? "CORNER" : "STRIP");
            add(M_SHOWFPS, "FPS COUNTER", onOff(cfg.showFps));
            add(M_BACK, "BACK", "");
        }
        else if (menuPage == PAGE_GRAPHICS)
        {
            add(M_FPS60, "60 FPS", onOff(cfg.fps60));
            add(M_WIDE, "WIDESCREEN WORLD", onOff(cfg.wide));
            // (Black, White, Black 2 and White 2. The game sets its memory up for it as it
            // starts, so a change waits for the next start.)
            add(M_WIDEMAP, "WIDE MAP LOADING", !bw2map.Supported() ? "-" : onOff(cfg.wideMap) + (cfg.wideMap != bw2map.on ? " (NEXT START)" : ""));
#ifdef __SWITCH__
            add(M_WIDELINES, "WORLD DETAIL", std::to_string(detailLines[detailIndex()]));
            // ON: fewer lines than WORLD DETAIL for a moment where drawing them cannot keep up
            // (a move's animation full of effects), so that the game does not slow down.
            add(M_AUTODETAIL, "LOWER DETAIL WHEN BUSY", onOff(cfg.autoDetail));
#else
            // (The same setting, named as on a PC's other games: the lines the 3D world is drawn
            // with, as a screen resolution.)
            {
                const int l = detailLines[detailIndex()];
                add(M_WIDELINES, "3D RESOLUTION", std::to_string(l) + "P" + (l == 192 ? " (DS)" : l == 1440 ? " (2K)" : l == 2160 ? " (4K)" : ""));
            }
            add(M_AUTODETAIL, "LOWER RES WHEN BUSY", onOff(cfg.autoDetail));
#endif
#if SSR_WORLD_ON_GPU && !defined(__SWITCH__)
            // ON: the widescreen world drawn by the graphics card (OpenGL), not the processor.
            // (On the Switch always on: its fallback after a start that did not keep going is
            // for that start only, see StartWideGpu's marker.)
            add(M_WIDEGPU, "WORLD ON GPU", melonDS::WideGpuCurrent ? "ON" : cfg.wideGpu ? "ON (NOT AVAILABLE)" : "OFF");
#endif
#if SSR_MENU_GPU3D
            // ON: the 3D scene drawn by the graphics chip instead of the processor. (Not on the
            // Switch: its OpenGL driver hung the console from a second thread and, from the main
            // thread, cost 10-12 ms a frame for the DS's picture and up to 22 ms to read a 720-line
            // widescreen picture back, far more than drawing it in software.)
            add(M_GPU3D, "3D ON GPU", UseGpu3D ? "ON" : cfg.gpu3d ? "ON (NOT AVAILABLE)" : "OFF");
#endif
#if !defined(__SWITCH__) && !defined(SSR_PHONE)
            // ON: 360 lines while fast forwarding, for more frames a second (the menu's hint says so).
            add(M_FFLOWRES, "FAST FORWARD LOW RES", onOff(cfg.ffLowRes));
#endif
#if SSR_MENU_EMULATION
            // Read when the game starts (SwitchPlatform::Start); a change waits for the next start.
            // FAST JIT: the JIT with fast memory.
            add(M_EMULATION, "EMULATION", std::string(!cfg.jit ? "INTERPRETER" : cfg.fastMem ? "FAST JIT" : "JIT")
                + (cfg.jit != UseJit || (cfg.jit && cfg.fastMem != UseFastMem) ? " (NEXT START)" : ""));
#endif
            add(M_BACK, "BACK", "");
        }
        else if (menuPage == PAGE_CHEATS)
        {
            if (cheats.items.empty()) add(M_NOTHING, "NONE FOUND FOR THIS GAME", "");
            cheatRows = cheats.Rows();
            for (size_t i = 0; i < cheatRows.size(); i++)
            {
                const auto& row = cheatRows[i];
                const std::string label = cheats.Label(row);
                add(M_CHEAT0 + (int)i, label.c_str(), row.folder ? cheats.FolderValue(row.index)
                    : cheats.items[row.index].on ? "ON" : "OFF", row.folder);
            }
            add(M_BACK, "BACK", "");
        }
        else if (menuPage == PAGE_CHEATMENU)
        {
            add(M_CHEATWIDTH, "DYNAMIC WIDTH", onOff(cfg.cheatDynamicWidth));
            add(M_CHEATTEXT, "TEXT SIZE", Menu::TextSizeName(cfg.cheatTextSize));
            add(M_BACK, "BACK", "");
        }
        else if (menuPage == PAGE_CONTROLS)
        {
            add(M_CONTROLS, "KEYS AND BUTTONS", "");
            add(M_STARTBTN, "START BUTTON", cfg.startMenu ? "PAUSE MENU" : "DS START");
#ifndef __SWITCH__
            // (Nothing to choose on the Switch: its labels are the DS's positions.)
            add(M_BUTTONS, "BUTTONS", cfg.byLabel ? "BY LABEL" : "BY POSITION");
#endif
#if !defined(__SWITCH__) && !defined(SSR_PHONE)
            add(M_PADTOUCH, "STICK TOUCH CURSOR", onOff(cfg.padTouch)); // (the Switch and phones have touch screens)
#endif
            add(M_BACK, "BACK", "");
        }
    };
    // To another page of the pause menu, on the row of item `select` (its first row: -1).
    auto goPage = [&](int page, int select) {
        menuPage = page;
        menu.scroll = 0;
        cheatRepeat.Update(0, 0);
        buildMenu();
        if (page == PAGE_CHEATS)
        {
            cheatLayout = {};
            // Include hidden cheats and the largest possible folder counts in the initial
            // width, so opening folders and switching codes never changes the panel's size.
            for (const auto& row : cheats.Rows(true))
                cheatLayout.items.push_back({ cheats.Label(row), row.folder ? std::to_string(cheats.Count(row.index)) : "OFF" });
            if (cheats.items.empty()) cheatLayout.items.push_back({ "NONE FOUND FOR THIS GAME", "" });
            cheatLayout.items.push_back({ "BACK", "" });
        }
        menu.selected = 0;
        for (size_t i = 0; i < menuIds.size(); i++)
            if (menuIds[i] == select) menu.selected = (int)i;
    };
    // Collapse a folder without losing the selected row when its children disappear.
    auto collapseCheatFolder = [&](int parent) {
        cheats.folders[parent].expanded = false;
        buildMenu();
        for (size_t i = 0; i < cheatRows.size(); i++)
            if (cheatRows[i].folder && cheatRows[i].index == parent) menu.selected = (int)i;
    };
    buildMenu();

    std::string toast;
    uint32_t toastUntil = 0;
    int shownFps = 0;
    uint32_t presented = 0, fpsSince = 0;
    auto say = [&](const std::string& msg) {
        toast = msg;
        toastUntil = frame + 120;
    };
#if defined(__ANDROID__) && !defined(SSR_PC_LAUNCHER)
    bool updateSaid = false;
    UpdateCheck::Start();
#endif

    std::vector<uint32_t> shot;
    std::vector<int16_t> audioBuf(48000 * 2);
    // Sound stretching for when the emulation runs slow (audiostretch.h).
    AudioStretch audioStretch;
    std::vector<int16_t> audioOut;
    uint64_t audioTick = 0;
    double audioSlow = 1.0;
    bool running = true;
    bool mouseDown = false;
    int mouseX = 0, mouseY = 0;
    // On-screen controls (touch screens): shown while no controller is connected.
    TouchPad touchPad;
    bool touchPadUsed = false, touchMenuHeld = false, prevTouchMenu = false;
#ifdef SSR_PHONE
    touchPadUsed = true;
#endif
#ifdef __SWITCH__
    // The Switch's touch screen: its SDL sends finger events and no mouse events made from them,
    // so a finger is followed here (the first one down) and taken as the mouse below.
    bool switchFinger = false;
    SDL_FingerID switchFingerId = 0;
    float switchFingerX = 0, switchFingerY = 0;
#endif
    // Touch cursor for devices without a touchscreen: moved with the right stick or IJKL,
    // pressed with the left trigger or Space.
    float cursorX = 128, cursorY = 96;
    uint32_t cursorShownUntil = 0;
    PadState prevPad;
    MenuInput heldNav;          // navigation directions held last tick, for edge detection
    bool prevComboMenu = false;
    bool ctrlStart = false, touchStart = false, prevCtrlStart = false; // see cfg.startMenu
    bool swallowMenuButtons = false; // keeps the buttons that closed the menu from reaching the game
    const double frameTime = 1.0 / 59.8261;
    uint64_t perfFreq = opt.headless ? 1 : SDL_GetPerformanceFrequency();
    uint64_t nextTick = opt.headless ? 0 : SDL_GetPerformanceCounter();

    Sampler::Start();
    double perfAcc[6] = {};
    double perfWorst = 0;
    int perfN = 0;
    auto perfNow = []() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    double perfT = perfNow(), perfFrameStart = perfT;
    // Of "scene" in the timing lines (SSR_EMU_PROF): the screens' sections and focus, choosing
    // the layout, its edges and extra rows (the Black games), sound and the overlay, the world's
    // picture taken for the view.
    double sceneProf[5] = {};
    double sceneProfT = 0;
    auto sceneMark = [&](int part) {
        if (!SsrProf::On) return;
        const double t = perfNow();
        if (part >= 0) sceneProf[part] += t - sceneProfT;
        sceneProfT = t;
    };
    // --timing: the same, a line per frame.
    double perfFrame[6] = {};
    FILE* timingFile = nullptr;
    if (!opt.timing.empty())
    {
        timingFile = fopen(opt.timing.c_str(), "w");
        if (!timingFile) fprintf(stderr, "cannot write %s\n", opt.timing.c_str());
        else fprintf(timingFile, "frame,total,input,emulate,scene,compose,present,wait,wait_3d,wait_wide,wide_lines,shot\n");
    }
    uint64_t timingWait3D = 0, timingWaitWide = 0;
    bool timingShot = false, timingStarted = false;
    auto perfMark = [&](int phase) {
        if (!perfOn && !timingFile) return;
        double t = perfNow();
        perfAcc[phase] += t - perfT;
        perfFrame[phase] += t - perfT;
        perfT = t;
    };

    // Showing a frame: put together (by the graphics card unless SSR_CPU_COMPOSE), the
    // overlay on top, presented, then the wait that keeps frames 1/60 s apart.
    struct DrawJob
    {
        bool valid = false;
        bool merged = false, overlayUsed = false, wantShot = false;
        uint32_t frameNo = 0; // (for the name of a screenshot)
        int outW = 0, outH = 0;
        SDL_Rect rects[2] {};
        const u32* top = nullptr;
        const u32* bot = nullptr;
        ScreenImage screens[2] {};
        Scene scene;
        WideImage wide;
        bool battleRender = false; // the widened battle picture is still to be made (BattleRender)
        std::vector<uint8_t> flags[2], edgeFlat[2];
        std::vector<uint32_t> edge[2], widePixels;
        std::vector<uint16_t> edgeBlend[2];

        // For drawing while the next frame is emulated: copies of what that changes. The
        // screens themselves stay: the emulation draws into the other framebuffer and swaps
        // them at its very end. The layer flags and edges are redrawn line by line, and the
        // widescreen picture is swapped when the 3D is drawn. (The widescreen picture itself
        // stays as it is for the next frame, GPU3D_Wide.h, but not over several: copied when
        // fast forwarding.)
        void Keep(bool severalFrames)
        {
            for (int i = 0; i < 2; i++)
            {
                ScreenImage& sc = screens[i];
                if (sc.flags) { flags[i].assign(sc.flags, sc.flags + 256 * (192 + scene.extraRows[i])); sc.flags = flags[i].data(); }
                if (sc.edge) { edge[i].assign(sc.edge, sc.edge + 2 * 192); sc.edge = edge[i].data(); }
                if (sc.edgeBlend) { edgeBlend[i].assign(sc.edgeBlend, sc.edgeBlend + 2 * 192); sc.edgeBlend = edgeBlend[i].data(); }
                if (sc.edgeFlat) { edgeFlat[i].assign(sc.edgeFlat, sc.edgeFlat + 2 * 192); sc.edgeFlat = edgeFlat[i].data(); }
            }
            if (severalFrames && wide.pixels && wide.w > 0 && wide.h > 0 && wide.pixels == melonDS::Wide.Front)
            {
                widePixels.assign(wide.pixels, wide.pixels + (size_t)wide.w * wide.h);
                wide.pixels = widePixels.data();
            }
        }
        // For a picture shown a frame later still (`ahead`): by then the emulation has drawn
        // over both framebuffers and the widescreen picture's memory, so copies of all of it.
        // Made once the next frame runs (KeepLater): the frame just run's framebuffer and
        // widescreen picture stay as they are through the next frame, and the copies (4 MB at
        // 720 lines) took 1.5 ms of the time between two frames' emulation.
        std::vector<uint32_t> screenPixels[2], plainPixels[2];
        bool keepLater = false;
        void KeepLater()
        {
            if (!keepLater) return;
            keepLater = false;
            for (int i = 0; i < 2; i++)
            {
                ScreenImage& sc = screens[i];
                if (!sc.pixels) continue;
                const u32* old = sc.pixels;
                screenPixels[i].assign(old, old + 256 * (192 + scene.extraRows[i]));
                sc.pixels = screenPixels[i].data();
                if (top == old) top = sc.pixels;
                if (bot == old) bot = sc.pixels;
            }
            for (int i = 0; i < 2; i++)
            {
                // (top and bot, where not one of the screens above: the plain layouts.)
                const u32*& p = i ? bot : top;
                if (!p || p == screens[0].pixels || p == screens[1].pixels) continue;
                plainPixels[i].assign(p, p + 256 * 192);
                p = plainPixels[i].data();
            }
            if (!battleRender) KeepWide(); // (a widened battle picture: once it is made)
        }
        void KeepWide()
        {
            if (wide.pixels && wide.w > 0 && wide.h > 0 && wide.pixels != widePixels.data())
            {
                widePixels.assign(wide.pixels, wide.pixels + (size_t)wide.w * wide.h);
                wide.pixels = widePixels.data();
            }
        }
    };
    DrawJob nowJob, pendingJob;

    // The pipeline: each frame is shown while the next one is emulated on a thread of its
    // own (emuthread.h), so that the time for showing it no longer adds to the emulation's.
    // On the Switch by default ("pipeline" in the settings), elsewhere with SSR_PIPELINE=1.
    // A frame then reaches the screen one frame later. Not with the headless tests or
    // lockstep, nor with screenshots unless SSR_PIPELINE is set.
#ifdef __SWITCH__
    bool pipelined = cfg.pipeline;
#else
    bool pipelined = getenv("SSR_PIPELINE") != nullptr;
#endif
    if (opt.headless || ref || (!opt.shotDir.empty() && !getenv("SSR_PIPELINE"))) pipelined = false;
    EmuThread emuThread;
    int emuSpeed = 1, emuGot = 0;
    double emuMs = 0, perfEmu = 0;
    uint64_t perfDsFrames = 0; // DS frames emulated since the last timing line (several a frame when fast forwarding)

    // The emulation of one frame (several when fast-forwarding; only the first one's sound
    // is played).
    // Fast forward runs several frames at once and shows the last: the others' 2D and widescreen
    // world are not drawn (rc_frame_hidden, rc_wide_render_skip), so it goes faster and shows
    // more frames. SSR_FF_FULL=1 draws them all, for comparing.
    static const bool ffFull = getenv("SSR_FF_FULL") != nullptr;
    auto runFrames = [&]() {
        const double t0 = perfNow();
        emuGot = 0;
        for (int i = 0; i < emuSpeed; i++)
        {
            // (The widescreen world drawn during a frame is shown after the next one.)
            rc_frame_hidden = !ffFull && emuSpeed > 1 && i < emuSpeed - 1;
            rc_wide_render_skip = !ffFull && emuSpeed > 1 && i < emuSpeed - 2;
            rc_frame++;
            if (funcWatchOn) funcWatch.NextFrame();
            nds->RunFrame();
            int avail = nds->SPU.GetOutputSize();
            if (avail > (int)audioBuf.size() / 2) avail = (int)audioBuf.size() / 2;
            int n = nds->SPU.ReadOutput(audioBuf.data(), avail);
            if (i == 0) emuGot = n;
        }
        rc_frame_hidden = rc_wide_render_skip = false;
        emuMs = perfNow() - t0;
    };
    if (pipelined)
    {
        if (!emuThread.Start(runFrames)) pipelined = false;
#ifdef __SWITCH__
        // The emulation has core 0 (emuthread.h); the main thread goes to core 2, beside a
        // widescreen thread (the DS-resolution 3D thread is on core 1).
        if (pipelined) SwitchPlatform::MoveMainThread();
#endif
        fprintf(stderr, "[pipeline] %s\n", pipelined ? "on: each frame is shown while the next one is emulated" : "could not start");
    }

    uint64_t lastShownAt = 0, shownGaps = 0, shownGapsLong = 0;
    // Showing a frame in two steps: drawn (renderFrame), then handed to the screen
    // (presentFrame), which with `ahead` happens at the frame's own time, a while after.
    auto renderFrame = [&](DrawJob& j) {
        // (The widened battle picture, from what was grabbed after the frame: made here, beside
        // the next frame's emulation.)
        if (j.battleRender)
        {
            BattleRender(battleWide, j.wide, 0);
            j.battleRender = false;
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (j.merged)
        {
            // The graphics card puts the pieces together (gpucompose.h). SSR_CPU_COMPOSE=1
            // uses the compositor of the headless tests instead, for comparing the two.
            if (!cpuCompose)
            {
                if (!gpuCompose) gpuCompose = new GpuCompositor(renderer);
                gpuCompose->Draw(j.scene, j.screens, j.outW, j.outH, cfg.smooth, &j.wide);
                perfMark(3);
            }
            else
            {
                int cw = j.outW, ch = j.outH;
                if (cfg.smooth && j.outH > 768)
                {
                    cw = j.outW * 768 / j.outH;
                    ch = 768;
                }
                MergedCompose(j.scene, j.screens, cw, ch, mergedImage, cfg.smooth, &j.wide);
                perfMark(3);
                if (!mergedTex || mergedTexW != cw || mergedTexH != ch)
                {
                    if (mergedTex) SDL_DestroyTexture(mergedTex);
                    mergedTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, cw, ch);
                    mergedTexW = cw;
                    mergedTexH = ch;
                }
                SDL_SetTextureScaleMode(mergedTex, cfg.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                SDL_UpdateTexture(mergedTex, nullptr, mergedImage.data(), cw * 4);
                SDL_RenderCopy(renderer, mergedTex, nullptr, nullptr);
            }
        }
        else
        {
            SDL_UpdateTexture(tex[0], nullptr, j.top, 256 * 4);
            SDL_UpdateTexture(tex[1], nullptr, j.bot, 256 * 4);
            int first = LayoutDrawFirst(layout);
            for (int n = 0; n < 2; n++)
            {
                int i = n == 0 ? first : 1 - first;
                if (j.rects[i].w > 0) SDL_RenderCopy(renderer, tex[i], nullptr, &j.rects[i]);
            }
        }
        if (j.overlayUsed)
        {
            if (!overlayTex || overlayTexW != j.outW || overlayTexH != j.outH)
            {
                if (overlayTex) SDL_DestroyTexture(overlayTex);
                overlayTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, j.outW, j.outH);
                SDL_SetTextureBlendMode(overlayTex, SDL_BLENDMODE_BLEND);
                overlayTexW = j.outW;
                overlayTexH = j.outH;
            }
            if (canvas.Dirty())
            {
                const SDL_Rect dr = { canvas.x0d, canvas.y0d, canvas.x1d - canvas.x0d, canvas.y1d - canvas.y0d };
                SDL_UpdateTexture(overlayTex, &dr, canvas.px.data() + (size_t)dr.y * j.outW + dr.x, j.outW * 4);
                SDL_RenderCopy(renderer, overlayTex, &dr, &dr);
            }
        }
        // In a window, screenshots are read back from the renderer: exactly what is shown.
        if (j.wantShot)
        {
            std::vector<uint32_t> back((size_t)j.outW * j.outH);
            if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, back.data(), j.outW * 4) == 0)
            {
                char name[64];
                snprintf(name, sizeof(name), "/window_%06u.png", j.frameNo);
                WritePNG(opt.shotDir + name, back.data(), j.outW, j.outH);
            }
        }
        if (touchPadUsed && touchPad.visible)
        {
            // Lit by what the fingers hold (by label, as drawn).
            bool held[PAD_COUNT] = {};
            bool menuHeld = false;
            touchPad.Apply(held, menuHeld);
            touchPad.Draw(renderer, held, menuHeld);
        }
    };
    auto presentFrame = [&](DrawJob& j, bool pace) {
        // How evenly the frames are handed to the screen, for the timing lines: gaps between
        // them of more than 17.5 ms (a frame takes 16.7). (Taken before presenting: how long
        // that takes depends on the screen's own refresh.)
        {
            const uint64_t t = SDL_GetPerformanceCounter();
            if (lastShownAt && !j.wantShot)
            {
                shownGaps++;
                if ((double)(t - lastShownAt) / perfFreq > 0.0175) shownGapsLong++;
            }
            lastShownAt = t;
        }
        SDL_RenderPresent(renderer);
        perfMark(4);
        // Frames shown in the last whole second, for the FPS counter.
        presented++;
        const uint32_t ms = SDL_GetTicks();
        if (ms - fpsSince >= 1000)
        {
            shownFps = (int)(presented * 1000 / (ms - fpsSince));
            presented = 0;
            fpsSince = ms;
        }

        // (Frames shown a frame later keep their own times: see `ahead`.)
        if (!pace)
        {
            perfMark(5);
            return;
        }
        nextTick += (uint64_t)(frameTime * perfFreq);
        uint64_t now = SDL_GetPerformanceCounter();
        if (now < nextTick)
        {
            // Sleep for most of the wait and spin for the last part of a millisecond:
            // frames then start within a few microseconds of an even 16.7 ms apart.
            double left = (double)(nextTick - now) / perfFreq;
            // (With the pipeline the start of the next emulation does not depend on this
            // to the microsecond: only the sleep, leaving the core to the widescreen thread.)
            if (pipelined) std::this_thread::sleep_for(std::chrono::duration<double>(left));
            else
            {
                if (left > 0.0008) std::this_thread::sleep_for(std::chrono::duration<double>(left - 0.0006));
                while (SDL_GetPerformanceCounter() < nextTick) {}
            }
        }
        else
        {
            // Never faster than the DS (59.8 frames a second). A frame that ran late may take
            // up to a quarter of a frame back from the next one, so that frames a little late
            // and a little early in turn still average out to full speed, but a longer stall
            // is not won back: up to 250 ms used to be, as frames run back to back, and the
            // game and its music sped up for a moment after every stall.
            const uint64_t carry = (uint64_t)(frameTime * perfFreq / 4);
            if (now - nextTick > carry) nextTick = now - carry;
        }
        perfMark(5);
    };
    auto drawFrame = [&](DrawJob& j, bool pace = true) {
        renderFrame(j);
        presentFrame(j, pace);
    };

    // One frame further ahead (`ahead`, with the pipeline; SSR_AHEAD=0 to compare): the emulation
    // may finish up to two frames before they are shown, and they are shown at even times of
    // their own (nextTick) instead of as each frame's emulation ends. A frame that takes
    // longer than 16.7 ms then no longer makes its picture late, as long as the frames around
    // it make up for it. Each frame reaches the screen a frame later. Not with the OpenGL 3D
    // of a PC (GPU3D_GLThread.h), which works on this thread between the emulation's steps.
    // Always on on the Switch (cfg.frameAhead, see where the settings are read); a test run's
    // "set frame_ahead=0" or SSR_AHEAD=0 turns it off for comparing.
    bool aheadAllowed = pipelined && !(getenv("SSR_AHEAD") && getenv("SSR_AHEAD")[0] == '0');
#ifdef SSR_GPU3D
    if (melonDS::GLThreadRenderer::Current) aheadAllowed = false;
#endif
    bool ahead = aheadAllowed && cfg.frameAhead;
    if (ahead) fprintf(stderr, "[pipeline] frames shown one frame later, at even times\n");
    DrawJob aheadJobs[3];
    int aheadHead = 0, aheadCount = 0;
    bool aheadRendered = false; // the oldest waiting picture is drawn, to be handed over at its time
    uint64_t aheadShown = 0, aheadLate = 0; // for the timing lines
    // Shows the oldest waiting picture if its time has come (or `now` regardless).
    auto presentDue = [&](bool now) -> bool {
        if (!aheadCount) return false;
        uint64_t t = SDL_GetPerformanceCounter();
        if (!now && t < nextTick) return false;
        const uint64_t ft = (uint64_t)(frameTime * perfFreq);
        if (t > nextTick + perfFreq / 500) aheadLate++; // (over 2 ms after its time)
        aheadShown++;
        DrawJob& j = aheadJobs[aheadHead];
        if (!aheadRendered)
        {
            j.KeepLater(); // (not yet made when no frame has run since, as while paused)
            renderFrame(j);
        }
        presentFrame(j, false);
        aheadRendered = false;
        j.valid = false;
        aheadHead = (aheadHead + 1) % 3;
        aheadCount--;
        nextTick += ft;
        // (As drawFrame's own pacing: a little lateness is won back, a stall is not.)
        t = SDL_GetPerformanceCounter();
        if (t > nextTick && t - nextTick > ft / 4) nextTick = t - ft / 4;
        return true;
    };
    // Draws the oldest waiting picture ahead of its time, so that only handing it over is left
    // for then (drawing it took 4-8 ms, and frames reached the screen that much unevenly).
    auto renderAhead = [&]() {
        if (!aheadCount || aheadRendered) return;
        DrawJob& j = aheadJobs[aheadHead];
        if (j.keepLater || j.battleRender) return; // (still to be finished once the next frame runs)
        renderFrame(j);
        aheadRendered = true;
    };
    auto secondsToTick = [&]() {
        const uint64_t t = SDL_GetPerformanceCounter();
        return t < nextTick ? (double)(nextTick - t) / perfFreq : 0.0;
    };

    while (running && !Host::StopRequested)
    {
        if (timingFile)
        {
            // (Written before the summary below takes the waits' counters back to 0.)
            const double t = perfNow();
            SsrThreadStats& ts = SsrStats();
            const uint64_t w3 = ts.Wait3DNs.load(), ww = ts.WaitWideNs.load();
            if (timingStarted)
                fprintf(timingFile, "%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d\n", frame, t - perfFrameStart,
                    perfFrame[0], pipelined ? emuMs : perfFrame[1], perfFrame[2], perfFrame[3], perfFrame[4], perfFrame[5],
                    (w3 - timingWait3D) / 1e6, (ww - timingWaitWide) / 1e6, WideLinesNow, timingShot ? 1 : 0);
            timingWait3D = w3;
            timingWaitWide = ww;
            for (double& a : perfFrame) a = 0;
            timingShot = false;
            timingStarted = true;
            if (!perfOn) perfFrameStart = perfT = t;
        }
        if (perfOn)
        {
            double t = perfNow();
            if (t - perfFrameStart > perfWorst) perfWorst = t - perfFrameStart;
            perfFrameStart = perfT = t;
            if (++perfN == 300)
            {
                // Of the emulation's time, how much went on waiting for the DS-resolution 3D
                // thread and for the widescreen drawing; on the Switch also where those
                // threads ran (frames started on cores 1 and 2), the clocks and temperatures.
                SsrThreadStats& ts = SsrStats();
                const double w3d = ts.Wait3DNs.exchange(0) / 1e6 / perfN, wwide = ts.WaitWideNs.exchange(0) / 1e6 / perfN;
                char extra[200];
                snprintf(extra, sizeof(extra), "; waited for 3D %.2f wide %.2f ms (wide lines %d)", w3d, wwide, WideLinesNow);
                std::string tail = extra;
                if (gpuCompose && gpuCompose->WideUploads)
                {
                    snprintf(extra, sizeof(extra), "; of compose, the world picture's upload %.2f ms",
                        gpuCompose->WideUploadMs / perfN);
                    tail += extra;
                    gpuCompose->WideUploadMs = 0;
                    gpuCompose->WideUploads = 0;
                }
                if (gpuCompose && SsrProf::On)
                {
                    snprintf(extra, sizeof(extra), "; of compose, signatures %.2f, the piece under the world %.2f, other pieces %.2f ms (%.1f a frame)",
                        gpuCompose->SignatureMs / perfN, gpuCompose->UnderWorldMs / perfN, gpuCompose->UploadMs / perfN,
                        (double)gpuCompose->Uploads / perfN);
                    tail += extra;
                    gpuCompose->SignatureMs = gpuCompose->UnderWorldMs = gpuCompose->UploadMs = 0;
                    gpuCompose->Uploads = 0;
                }
                if (SsrProf::On)
                {
                    snprintf(extra, sizeof(extra), "; of scene: screens and focus %.2f, layout %.2f, edges %.2f, sound and overlay %.2f, world picture %.2f ms",
                        sceneProf[0] / perfN, sceneProf[1] / perfN, sceneProf[2] / perfN, sceneProf[3] / perfN, sceneProf[4] / perfN);
                    tail += extra;
                    for (double& a : sceneProf) a = 0;
                }
                if (SsrProf::On)
                {
                    static const char* const names[SsrProf::Count] = { "geometry", "2D top", "sound", "DMA", "wide lists",
                        "in-between lists", "3D start", "2D bottom", "3D lines wait", "lists wait", "wide 3D start", "GPU send wait" };
                    tail += "; of emulate:";
                    for (int i = 0; i < SsrProf::Count; i++)
                    {
                        snprintf(extra, sizeof(extra), " %s %.2f", names[i], SsrProf::Ns[i] / 1e6 / perfN);
                        tail += extra;
                        SsrProf::Ns[i] = 0;
                    }
                }
#ifdef __SWITCH__
                // (wide: on cores 0/1/2; core 0 only with wide_core0, see melonDS/src/SsrThreads.h)
                snprintf(extra, sizeof(extra), "; 3D thread on cores 1/2 %u/%u, wide %u/%u/%u", ts.RenderCore[1].exchange(0),
                    ts.RenderCore[2].exchange(0), ts.WideCore[0].exchange(0), ts.WideCore[1].exchange(0), ts.WideCore[2].exchange(0));
                tail += extra;
                const std::string status = SwitchPlatform::Status();
                if (!status.empty()) tail += "; " + status;
#endif
                // With the pipeline, emulate is the emulation thread's time, and compose,
                // present and wait happen alongside it, the main thread then waiting for the
                // emulation the time given at the end.
                if (pipelined)
                {
                    snprintf(extra, sizeof(extra), "; pipeline: waited for the emulation %.2f ms", perfAcc[1] / perfN);
                    tail += extra;
                }
                if (shownGaps)
                {
                    snprintf(extra, sizeof(extra), "; shown over 17.5 ms after the last: %llu of %llu",
                        (unsigned long long)shownGapsLong, (unsigned long long)shownGaps);
                    tail += extra;
                    shownGaps = shownGapsLong = 0;
                }
                if (ahead && aheadShown)
                {
                    snprintf(extra, sizeof(extra), "; shown a frame later: %llu of %llu over 2 ms late",
                        (unsigned long long)aheadLate, (unsigned long long)aheadShown);
                    tail += extra;
                    aheadLate = aheadShown = 0;
                }
                if (perfDsFrames > (uint64_t)perfN)
                {
                    snprintf(extra, sizeof(extra), "; fast forward: %.2f DS frames a frame", (double)perfDsFrames / perfN);
                    tail += extra;
                }
                perfDsFrames = 0;
                fprintf(stderr, "[perf] input %.2f  emulate %.2f  scene %.2f  compose %.2f  present %.2f  wait %.2f ms; worst frame %.1f ms%s\n",
                    perfAcc[0] / perfN, (pipelined ? perfEmu : perfAcc[1]) / perfN, perfAcc[2] / perfN, perfAcc[3] / perfN, perfAcc[4] / perfN,
                    perfAcc[5] / perfN, perfWorst, tail.c_str());
                for (double& a : perfAcc) a = 0;
                timingWait3D = timingWaitWide = 0;
                perfEmu = 0;
                perfN = 0;
                perfWorst = 0;
            }
        }
        uint32_t keys = 0;
        bool touching = false;
        int touchX = 0, touchY = 0;
        MenuInput nav;              // edge-triggered menu input for this tick
        bool toggleMenu = false;
        bool kbFast = false;
        float curDX = 0, curDY = 0;
        bool curPress = false;
        PadState ps;

        int outW = opt.composeW > 0 ? opt.composeW : 1280, outH = opt.composeH > 0 ? opt.composeH : 720;

        if (!opt.headless)
        {
            SDL_Event ev;
            touchPad.NextFrame();
            // The pause menu takes taps on itself: a row is chosen and used at once (a setting
            // changes: the left part of the box back, the rest forward); a tap beside the box
            // closes the menu.
            auto menuTap = [&](float fx, float fy) {
                const int item = menu.ItemAt((int)fx, (int)fy);
                if (item == -2) nav.back = true;
                else if (item >= 0)
                {
                    menu.selected = item;
                    if (menu.items[item].value.empty() || menu.items[item].directional) nav.accept = true;
                    else if (fx < menu.drawnX + menu.drawnW / 3) nav.left = true;
                    else nav.right = true;
                }
            };
            while (SDL_PollEvent(&ev))
            {
                if (ev.type == SDL_QUIT) running = false;
#ifdef __SWITCH__
                else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP)
                {
                    int ow = 0, oh = 0;
                    SDL_GetRendererOutputSize(renderer, &ow, &oh);
                    const float fx = ev.tfinger.x * ow, fy = ev.tfinger.y * oh;
                    if (ev.type == SDL_FINGERDOWN && !switchFinger)
                    {
                        if (menu.open)
                        {
                            menuTap(fx, fy);
                            continue;
                        }
                        switchFinger = true;
                        switchFingerId = ev.tfinger.fingerId;
                    }
                    if (switchFinger && ev.tfinger.fingerId == switchFingerId)
                    {
                        switchFingerX = fx;
                        switchFingerY = fy;
                        if (ev.type == SDL_FINGERUP)
                        {
                            switchFinger = false;
                            mouseDown = false;
                        }
                    }
                }
#endif
                else if (ev.type == SDL_APP_WILLENTERBACKGROUND || ev.type == SDL_APP_TERMINATING)
                {
                    // A phone may close the app at any time once it is in the background.
                    if (!opt.noSave) FlushSave(opt.save);
                    cfg.Save(opt.config);
                }
                else if (touchPadUsed && (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP))
                {
                    int ow = 0, oh = 0;
                    SDL_GetRendererOutputSize(renderer, &ow, &oh);
                    touchPad.Layout(ow, oh);
                    float fx = ev.tfinger.x * ow, fy = ev.tfinger.y * oh;
                    // The pause menu takes taps on itself: a row is chosen and used at once (a
                    // setting changes: the left part of the box back, the rest forward); a tap
                    // beside the box, away from the buttons, closes the menu.
                    if (menu.open && ev.type == SDL_FINGERDOWN && (!touchPad.visible || touchPad.HitButton(fx, fy) == TouchPad::NONE))
                    {
                        menuTap(fx, fy);
                        continue;
                    }
                    if (ev.type == SDL_FINGERDOWN)
                    {
                        // A finger on the screen brings the controls back (that finger only
                        // touches the picture, see Down).
                        touchPad.Down(ev.tfinger.fingerId, fx, fy);
                        touchPad.visible = true;
                    }
                    else if (ev.type == SDL_FINGERMOTION) touchPad.Move(ev.tfinger.fingerId, fx, fy);
                    else touchPad.Up(ev.tfinger.fingerId);
                }
                else if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
                {
                    SDL_Keycode k = ev.key.keysym.sym;
                    const int sc = ev.key.keysym.scancode;
                    // (The action CONTROLS gives this key, if any: those keys do that and nothing else.)
                    const int act = cfg.controls.ActionOfKey(sc);
                    if (menu.open && capturing >= 0)
                    {
                        // CONTROLS waiting for a key: ESC leaves it as it was.
                        if (k != SDLK_ESCAPE && k != SDLK_AC_BACK && ctlKeyboard)
                        {
                            cfg.controls.SetKey(capturing, sc);
                            cfg.Save(opt.config);
                        }
                        capturing = -1;
                        buildMenu();
                    }
                    else if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || act == Controls::MENU)
                    {
                        if (menu.open) nav.back = true;
                        else toggleMenu = true;
                    }
                    else if (menu.open)
                    {
                        if (k == SDLK_UP || act == Controls::UP) nav.up = true;
                        else if (k == SDLK_DOWN || act == Controls::DOWN) nav.down = true;
                        else if (k == SDLK_LEFT || act == Controls::LEFT) nav.left = true;
                        else if (k == SDLK_RIGHT || act == Controls::RIGHT) nav.right = true;
                        else if (k == SDLK_RETURN || act == Controls::A) nav.accept = true;
                        else if (act == Controls::B) nav.back = true;
                    }
                    else if (act == Controls::SWAP) focus.manualFlip = !focus.manualFlip;
                    else if (act >= 0) {} // (a DS button, or fast forward: read as held, below)
                    else if (k == SDLK_TAB) layout = LayoutNext(layout);
                    else if (k == SDLK_F2) focus.mode = (focus.mode + 1) % 3;
                    else if (k == SDLK_F5) say(WriteSnapshot(*nds, quickState, frame) ? "STATE SAVED" : "SAVE STATE FAILED");
                    else if (k == SDLK_F8)
                    {
                        say(ReadSnapshot(*nds, quickState, nullptr) ? "STATE LOADED" : "NO STATE TO LOAD");
                        SetClock(*nds, false);
                    }
                    else if (k == SDLK_F11
#ifdef __APPLE__
                        || (k == SDLK_f && (ev.key.keysym.mod & KMOD_GUI) && (ev.key.keysym.mod & KMOD_CTRL))
#endif
                    )
                    {
                        cfg.fullscreen = !cfg.fullscreen;
                        SDL_SetWindowFullscreen(window, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        SDL_ShowCursor(cfg.fullscreen ? SDL_DISABLE : SDL_ENABLE);
                    }
                }
                else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    mouseDown = false;
#ifdef SSR_PC_LAUNCHER
                // (The size the window is given, kept for next time; not the screen's when fullscreen.)
                else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
                         !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
                {
                    cfg.windowW = ev.window.data1;
                    cfg.windowH = ev.window.data2;
                }
#endif
                else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) mouseDown = true;
                else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) mouseDown = false;
                // (Every joystick is announced by SDL_JOYDEVICEADDED, game controllers also
                // by SDL_CONTROLLERDEVICEADDED; Open takes each once.)
                else if (ev.type == SDL_JOYDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEADDED)
                {
                    if (pads.Open(ev.jdevice.which)) say("CONTROLLER CONNECTED");
                }
                else if (ev.type == SDL_JOYDEVICEREMOVED && pads.Close(ev.jdevice.which))
                {
                    // Pause rather than leave the game running with nobody at the controls.
                    if (pads.Empty() && !menu.open) toggleMenu = true;
                    say("CONTROLLER DISCONNECTED");
                }
            }
            SDL_GetMouseState(&mouseX, &mouseY);
            SDL_GetRendererOutputSize(renderer, &outW, &outH);
            // SDL mouse positions are window points; layout, cursor and touch use output pixels.
            // Refresh each frame for resize and movement between Retina/external displays.
            int windowW = 0, windowH = 0;
            SDL_GetWindowSize(window, &windowW, &windowH);
            if (outW <= 0 || outH <= 0 || windowW <= 0 || windowH <= 0 ||
                (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
            {
                SDL_Delay(16);
                continue;
            }
            mouseX = int(double(mouseX) * outW / windowW);
            mouseY = int(double(mouseY) * outH / windowH);
#ifdef __SWITCH__
            if (switchFinger)
            {
                mouseDown = true;
                mouseX = (int)switchFingerX;
                mouseY = (int)switchFingerY;
            }
#endif
            ps = ReadPad(pads);
            physPad = ps;
            ps = RemapPad(ps, cfg.controls, cfg.byLabel); // (CONTROLS)
            ctrlStart = ps.btn[PAD_START]; // (a controller's, before the on-screen buttons join in)
            if (touchPadUsed)
            {
                touchPad.Layout(outW, outH);
                // Using a controller hides them: a connected one is not proof enough, as
                // Android reports some built-in keys as one.
                bool padUsed = fabsf(ps.lx) > 0.5f || fabsf(ps.ly) > 0.5f;
                for (int i = 0; i < PAD_COUNT; i++) padUsed |= ps.btn[i];
                if (padUsed) touchPad.visible = false;
                // A finger on the picture touches the game's touch screen, like the mouse.
                float fx, fy;
                mouseDown = touchPad.ScreenFinger(fx, fy);
                if (mouseDown)
                {
                    mouseX = (int)fx;
                    mouseY = (int)fy;
                }
                // The on-screen buttons go by their labels: the one marked A is the DS's A.
                bool held[PAD_COUNT] = {};
                touchPad.Apply(held, touchMenuHeld);
                touchStart = held[PAD_START];
                if (!cfg.byLabel)
                {
                    std::swap(held[PAD_A], held[PAD_B]);
                    std::swap(held[PAD_X], held[PAD_Y]);
                }
                for (int i = 0; i < PAD_COUNT; i++) ps.btn[i] |= held[i];
                if (touchMenuHeld && !prevTouchMenu) toggleMenu = true;
                prevTouchMenu = touchMenuHeld;
            }

            const Uint8* kb = SDL_GetKeyboardState(nullptr);
            if (!menu.open)
            {
                keys |= KeyboardKeys(cfg.controls);
                // (The touch cursor's keys, unless CONTROLS gave one of them to something.)
                auto keyFree = [&](int s) { return kb[s] && cfg.controls.ActionOfKey(s) < 0; };
                if (keyFree(SDL_SCANCODE_J)) curDX -= 1;
                if (keyFree(SDL_SCANCODE_L)) curDX += 1;
                if (keyFree(SDL_SCANCODE_I)) curDY -= 1;
                if (keyFree(SDL_SCANCODE_K)) curDY += 1;
                if (keyFree(SDL_SCANCODE_SPACE)) curPress = true;
                if (kb[cfg.controls.Key(Controls::FAST)]) kbFast = true;
            }
        }

        // A test run (testrun.h) goes by its script alone: buttons pressed meanwhile (or a
        // controller going away, which opens the pause menu) would pause it, and it would
        // never get to its last frame and end.
        if (opt.testRun)
        {
            ps = PadState();
            keys = 0;
            toggleMenu = false;
            mouseDown = false;
        }

        // (A game that is not recompiled: its battle from its functions.)
        const bool fwBattle = funcWatchOn && (funcWatch.Ran(bw2.battle, 2) || funcWatch.Ran(pt.battle, 2));
        const bool mashing = opt.battleMash && (appRunning("Battle") || fwBattle);
        if (mashing && (frame / 12) % 2 == 0)
        {
            // A fights; if that wandered into the in-battle party or bag screen (e.g. by answering
            // Yes to "switch Pokemon?"), B backs out and declines the question that led there.
            // If B does not leave it (the lead fainted), move to the next Pokemon and send it out.
            static uint32_t submenuSeen = 0, submenuSince = 0;
            if (sectionActive("OVY_8") || (pt.on && (funcWatch.Ran(pt.battleBag, 2) || funcWatch.Ran(pt.battleParty, 2))))
            {
                if (!submenuSince || frame - submenuSeen > 24) submenuSince = frame;
                submenuSeen = frame;
            }
            if (submenuSeen == frame && frame - submenuSince > 600)
            {
                static const int cycle[4] = { KEY_B, KEY_RIGHT, KEY_A, KEY_A };
                keys |= 1 << cycle[(frame / 24) % 4];
            }
            else if (submenuSeen && frame - submenuSeen < 240)
                keys |= 1 << KEY_B;
            else if ((frame / 24) % 2 == 0)
                keys |= 1 << KEY_A;
            else
            {
                // Every other press is a touch on one of the left-hand moves (FIGHT in the command
                // menu), so that the first move running out of PP does not stall the battle.
                touching = true;
                touchX = 60;
                touchY = (frame / 48) % 2 ? 115 : 55;
            }
        }
        bool navigating = false;
        if (navState.active && opt.navStopOnBattle && navState.started && appRunning("Battle"))
        {
            printf("nav: battle started at frame %u, stopping\n", frame);
            navState.active = false;
        }
        if (navState.active && !mashing && scriptClock >= opt.navAfter)
        {
            int map, x, z;
            if (playerPos(map, x, z))
            {
                navigating = true;
                if (!navState.started) navState.started = frame;
                if (navState.map != -1 && map != navState.map)
                {
                    // Went through a door: that waypoint is done and the grid is a new one.
                    printf("nav: map %d -> %d at frame %u\n", navState.map, map, frame);
                    if (navState.idx < navState.way.size() &&
                        std::abs(navState.way[navState.idx].first - navState.fromX) +
                        std::abs(navState.way[navState.idx].second - navState.fromZ) <= 2)
                        navState.idx++;
                    navState.blocked.clear();
                    navState.dir = -1;
                    navState.pushDir = -1;
                }
                navState.map = map;
                navState.outdoor = x >= 96 || z >= 96; // interiors have their own small grids
                while (navState.idx < navState.way.size() && navState.way[navState.idx] == std::make_pair(x, z))
                {
                    printf("nav: reached %d,%d at frame %u\n", x, z, frame);
                    navState.pushDir = navState.push[navState.idx];
                    navState.idx++;
                    navState.dir = -1;
                }
                if (navState.pushDir >= 0)
                {
                    static const int pushKeys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
                    keys |= 1 << pushKeys[navState.pushDir];
                }
                else if (navState.idx >= navState.way.size() || frame - navState.started > 150000)
                {
                    printf("nav: %s at frame %u, map %d x %d z %d\n",
                        navState.idx >= navState.way.size() ? "done" : "gave up", frame, map, x, z);
                    navState.active = false;
                    navigating = false;
                }
                else
                {
                    if (navState.dir >= 0 && (x != navState.fromX || z != navState.fromZ))
                        navState.dir = -1; // moved
                    else if (navState.dir >= 0 && frame - navState.since > 60)
                    {
                        navState.blocked.insert({ x, z, navState.dir });
                        navState.dir = -1;
                    }
                    if (navState.dir < 0)
                    {
                        auto [tx, tz] = navState.way[navState.idx];
                        navState.dir = navStep(x, z, tx, tz);
                        if (navState.dir < 0)
                        {
                            navState.blocked.clear();
                            navState.dir = navStep(x, z, tx, tz);
                        }
                        navState.fromX = x;
                        navState.fromZ = z;
                        navState.since = frame;
                    }
                    static const int navKeys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
                    if (navState.dir >= 0)
                    {
                        keys |= 1 << navKeys[navState.dir];
                        navState.lastDir = navState.dir;
                    }
                    else if (navState.lastDir >= 0)
                        keys |= 1 << navKeys[navState.lastDir]; // side doors open by walking on into them
                    // With no route (e.g. mid-warp, the target is on the next map) just wait.
                    // Dismiss anything that stops the walk (signs, people talking).
                    if (frame - navState.since > 24 && (frame / 6) % 2 == 0) keys |= 1 << KEY_B;
                }
            }
        }
        const bool holdScript = mashing || navigating;
        for (const ScriptEvent& ev : script)
        {
            if (holdScript) break;
            if (scriptClock < ev.start || scriptClock > ev.end) continue;
            keys |= ev.keys;
            if (ev.touch)
            {
                touching = true;
                touchX = ev.tx;
                touchY = ev.ty;
            }
            for (int i = 0; i < PAD_COUNT; i++)
                if (ev.padButtons & (1u << i)) ps.btn[i] = true;
            if (ev.lx != 0) ps.lx = ev.lx;
            if (ev.ly != 0) ps.ly = ev.ly;
            if (ev.rx != 0) ps.rx = ev.rx;
            if (ev.ry != 0) ps.ry = ev.ry;
            if (ev.lt != 0) ps.lt = ev.lt;
            if (ev.rt != 0) ps.rt = ev.rt;
        }

        // CONTROLS waiting for a controller's button: once every button is let go, the next one
        // pressed. Nothing else happens meanwhile (and the button stays out of the menu).
        if (menu.open && capturing >= 0)
        {
            bool anyDown = physPad.lt > 0.5f || physPad.rt > 0.5f;
            for (bool b : physPad.btn) anyDown |= b;
            if (!captureReleased) captureReleased = !anyDown;
            else if (!ctlKeyboard)
            {
                const int in = NewPadInput(physPad, prevPhysPad);
                if (in >= 0)
                {
                    cfg.controls.SetPad(capturing, in, cfg.byLabel);
                    cfg.Save(opt.config);
                    capturing = -1;
                    navHoldOff = true;
                    buildMenu();
                }
            }
            if (capturing >= 0 && SDL_GetTicks() > captureUntil)
            {
                capturing = -1;
                buildMenu();
            }
            toggleMenu = false;
            nav = MenuInput();
            ps = PadState();
        }
        prevPhysPad = physPad;
        if (navHoldOff)
        {
            bool anyDown = false;
            for (bool b : ps.btn) anyDown |= b;
            if (anyDown) ps = PadState();
            else navHoldOff = false;
        }

        // Controller: the pause menu opens with Guide, or with Start and Select together.
        auto pressed = [&](int b) { return ps.btn[b] && !prevPad.btn[b]; };
        bool comboMenu = ps.btn[PAD_START] && ps.btn[PAD_BACK];
        // (With START BUTTON on PAUSE MENU, a controller's START alone; the on-screen START of a
        // phone stays the DS's, beside its own MENU button.)
        const bool startPress = cfg.startMenu && ctrlStart && !prevCtrlStart;
        if (pressed(PAD_GUIDE) || startPress || (!cfg.startMenu && comboMenu && !prevComboMenu)) toggleMenu = true;
        prevComboMenu = comboMenu;
        prevCtrlStart = ctrlStart;

        MenuInput heldNow;
        heldNow.up = ps.btn[PAD_UP] || ps.ly < -0.5f;
        heldNow.down = ps.btn[PAD_DOWN] || ps.ly > 0.5f;
        heldNow.left = ps.btn[PAD_LEFT] || ps.lx < -0.5f;
        heldNow.right = ps.btn[PAD_RIGHT] || ps.lx > 0.5f;
        if (menu.open && menuPage == PAGE_CHEATS && !opt.headless && !opt.testRun && capturing < 0)
        {
            const Uint8* kb = SDL_GetKeyboardState(nullptr);
            heldNow.up |= kb[SDL_SCANCODE_UP] || kb[cfg.controls.Key(Controls::UP)];
            heldNow.down |= kb[SDL_SCANCODE_DOWN] || kb[cfg.controls.Key(Controls::DOWN)];
        }
        if (menu.open)
        {
            nav.up |= heldNow.up && !heldNav.up;
            nav.down |= heldNow.down && !heldNav.down;
            nav.left |= heldNow.left && !heldNav.left;
            nav.right |= heldNow.right && !heldNav.right;
            nav.accept |= pressed(cfg.byLabel ? PAD_A : PAD_B);
            nav.back |= pressed(cfg.byLabel ? PAD_B : PAD_A);
        }
        const int cheatDirection = menu.open && menuPage == PAGE_CHEATS ? (int)heldNow.down - (int)heldNow.up : 0;
        if (cheatRepeat.Update(cheatDirection, SDL_GetTicks()))
        {
            nav.up |= cheatDirection < 0;
            nav.down |= cheatDirection > 0;
        }
        heldNav = heldNow;

        if (toggleMenu)
        {
            menu.open = !menu.open;
            menuPage = 0;
            capturing = -1;
            if (menu.open)
            {
                menu.selected = 0;
                buildMenu();
            }
            else swallowMenuButtons = true;
        }
        else if (menu.open && menuPage == PAGE_KEYS)
        {
            // KEYS AND BUTTONS: B goes back to CONTROLS.
            int dir = 0;
            const int hit = nav.back ? ctlRowBack : menu.Update(nav, dir);
            if (hit == 0 && ctlHasKeyboard) ctlKeyboard = !ctlKeyboard;
            else if (hit >= ctlRow0 && hit < ctlRowReset)
            {
                // (Left or right on an action, as a tap on it does: the same as choosing it.)
                capturing = hit - ctlRow0;
                captureUntil = SDL_GetTicks() + 5000;
                captureReleased = false;
            }
            else if (hit == ctlRowReset)
            {
                cfg.controls.Reset();
                cfg.Save(opt.config);
                say("CONTROLS RESET");
            }
            else if (hit == ctlRowBack) goPage(PAGE_CONTROLS, M_CONTROLS);
            if (hit >= 0 && menuPage == PAGE_KEYS) buildMenu();
        }
        else if (menu.open)
        {
            // B: from a page back to the menu (on the page's own item), from the menu back to the game.
            int backTo = -1;
            if (nav.back && menuPage == PAGE_CHEATS && menu.selected >= 0 && menu.selected < (int)cheatRows.size())
            {
                const auto row = cheatRows[menu.selected];
                const int parent = row.folder ? row.index : cheats.items[row.index].folder;
                if (parent >= 0 && cheats.folders[parent].expanded)
                {
                    collapseCheatFolder(parent);
                    nav = MenuInput();
                }
            }
            if (nav.back && menuPage != PAGE_MAIN)
            {
                backTo = menuPage == PAGE_DISPLAY ? M_DISPLAY : menuPage == PAGE_GRAPHICS ? M_GRAPHICS
                       : menuPage == PAGE_CHEATS ? M_CHEATS : menuPage == PAGE_CHEATMENU ? M_CHEATMENU : M_CONTROLMENU;
                nav = MenuInput();
            }
            else if (nav.back)
            {
                menu.open = false;
                swallowMenuButtons = true;
            }
            int dir = 0;
            int hit = menu.Update(nav, dir);
            const int id = backTo >= 0 ? M_BACK : hit >= 0 && hit < (int)menuIds.size() ? menuIds[hit] : -1;
            if (backTo >= 0) hit = 0;
            // A cheat: on or off at once, for the game as it runs (cheats.h).
            if (id >= M_CHEAT0 && id - M_CHEAT0 < (int)cheatRows.size())
            {
                const auto row = cheatRows[id - M_CHEAT0];
                if (row.folder)
                {
                    auto& f = cheats.folders[row.index];
                    f.expanded = dir < 0 ? false : dir > 0 ? true : !f.expanded;
                }
                else
                {
                    cheats.Toggle((size_t)row.index);
                    cheats.Apply(nds->AREngine);
                    fprintf(stderr, "[cheats] %s %s\n", cheats.items[row.index].key.c_str(), cheats.items[row.index].on ? "on" : "off");
                }
            }
            switch (id)
            {
            case M_CHEATS:
                for (auto& f : cheats.folders) f.expanded = false;
                goPage(PAGE_CHEATS, -1);
                hit = -1;
                break;
            case M_CHEATMENU:
                goPage(PAGE_CHEATMENU, -1);
                hit = -1;
                break;
            case M_CHEATWIDTH:
                cfg.cheatDynamicWidth = !cfg.cheatDynamicWidth;
                cfg.Save(opt.config);
                break;
            case M_CHEATTEXT:
                cfg.cheatTextSize = std::clamp(cfg.cheatTextSize + (dir < 0 ? -1 : 1), 1, 50);
                cfg.Save(opt.config);
                break;
            case M_DISPLAY:
                goPage(PAGE_DISPLAY, -1);
                hit = -1;
                break;
            case M_GRAPHICS:
                goPage(PAGE_GRAPHICS, -1);
                hit = -1;
                break;
            case M_CONTROLMENU:
                goPage(PAGE_CONTROLS, -1);
                hit = -1;
                break;
            case M_BACK:
                if (backTo < 0)
                    backTo = menuPage == PAGE_DISPLAY ? M_DISPLAY : menuPage == PAGE_GRAPHICS ? M_GRAPHICS
                           : menuPage == PAGE_CHEATS ? M_CHEATS : menuPage == PAGE_CHEATMENU ? M_CHEATMENU : M_CONTROLMENU;
                goPage(PAGE_MAIN, backTo);
                hit = -1;
                break;
            case M_RESUME:
                menu.open = false;
                swallowMenuButtons = true;
                break;
#ifndef __SWITCH__
            case M_LAYOUT:
                layout.mode = (LayoutMode)(((int)layout.mode + (dir < 0 ? 5 : 1)) % 6);
                break;
            case M_LARGE:
                focus.mode = (focus.mode + (dir < 0 ? 2 : 1)) % 3;
                focus.manualFlip = false;
                break;
#endif
            case M_BATTLEBUTTONS:
                if (pt.on) break; // (Platinum's buttons stand beside the message box, as SoulSilver's.)
                if (funcWatchOn && bw1.on) cfg.battleButtons = (cfg.battleButtons + (dir < 0 ? 2 : 1)) % 3;
                else if (funcWatchOn) cfg.battleButtons = cfg.battleButtons == 2 ? 0 : 2; // CORNER, PANEL
                bw1.battleButtons = cfg.battleButtons;
                break;
            case M_FPS60:
                cfg.fps60 = !cfg.fps60;
                nds->GPU.GPU3D.InterpolationEnabled = cfg.fps60;
                break;
#if !defined(__SWITCH__) && !defined(SSR_PHONE)
            case M_FULLSCREEN:
                cfg.fullscreen = !cfg.fullscreen;
                if (window)
                {
                    SDL_SetWindowFullscreen(window, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    SDL_ShowCursor(cfg.fullscreen ? SDL_DISABLE : SDL_ENABLE);
                }
                break;
#endif
            case M_SMOOTH:
                cfg.smooth = !cfg.smooth;
                applySmooth();
                break;
            case M_WIDE:
                cfg.wide = !cfg.wide;
                break;
            case M_WIDEMAP:
                if (bw2map.Supported()) cfg.wideMap = !cfg.wideMap;
                break;
            case M_WIDELINES:
                cfg.wideLines = detailLines[(detailIndex() + (dir < 0 ? detailCount - 1 : 1)) % detailCount];
                break;
            case M_AUTODETAIL:
                cfg.autoDetail = !cfg.autoDetail;
                break;
#if SSR_MENU_GPU3D
            case M_GPU3D:
                // (between frames: the emulation thread is idle here)
                cfg.gpu3d = !cfg.gpu3d;
                ApplyRenderer3D(*nds, cfg.gpu3d);
                if (cfg.gpu3d && !UseGpu3D) say("NOT AVAILABLE: SEE GAME.LOG");
                break;
#endif
#if SSR_WORLD_ON_GPU && !defined(__SWITCH__)
            case M_WIDEGPU:
                // (between frames: the emulation thread is idle here)
                cfg.wideGpu = !cfg.wideGpu;
                melonDS::WideWait();
                if (cfg.wideGpu)
                {
                    if (!melonDS::WideGpuCurrent && !StartWideGpu()) say("NOT AVAILABLE: SEE GAME.LOG");
                    else if (cfg.ds3dGpu && DSPictureOnGpu(opt.headless)) ApplyDSGpu(*nds, true);
                }
                else
                {
                    ApplyDSGpu(*nds, false);
                    melonDS::WideGpuStop();
                    if (!WideGpuMarker.empty()) remove(WideGpuMarker.c_str());
                    fprintf(stderr, "[widegpu] off: the processor draws the widescreen world\n");
                }
                break;
#endif
            case M_SHOWFPS:
                cfg.showFps = !cfg.showFps;
                break;
#ifndef __SWITCH__
            case M_BUTTONS:
                cfg.byLabel = !cfg.byLabel;
                break;
#endif
#if !defined(__SWITCH__) && !defined(SSR_PHONE)
            case M_PADTOUCH:
                cfg.padTouch = !cfg.padTouch;
                break;
#endif
#if SSR_MENU_EMULATION
            case M_EMULATION:
                // FAST JIT, JIT, INTERPRETER, and round again.
                if (cfg.jit && cfg.fastMem) cfg.fastMem = false;
                else if (cfg.jit) cfg.jit = false;
                else cfg.jit = cfg.fastMem = true;
                cfg.Save(opt.config);
                say("APPLIES THE NEXT TIME THE GAME STARTS");
                break;
#endif
            case M_CONTROLS:
                capturing = -1;
                // (The keyboard's keys shown first when no controller is connected.)
                ctlKeyboard = ctlHasKeyboard && pads.Empty() && !touchPadUsed;
                goPage(PAGE_KEYS, -1);
                menu.selected = ctlRow0;
                hit = -1; // (the page is built: not again below)
                break;
            case M_STARTBTN:
                cfg.startMenu = !cfg.startMenu;
                break;
            case M_FAST:
                fastLock = !fastLock;
                break;
#if !defined(__SWITCH__) && !defined(SSR_PHONE)
            case M_FFLOWRES:
                cfg.ffLowRes = !cfg.ffLowRes;
                break;
#endif
            case M_SAVESTATE:
                say(WriteSnapshot(*nds, quickState, frame) ? "STATE SAVED" : "SAVE STATE FAILED");
                menu.open = false;
                swallowMenuButtons = true;
                break;
            case M_LOADSTATE:
                say(ReadSnapshot(*nds, quickState, nullptr) ? "STATE LOADED" : "NO STATE TO LOAD");
                SetClock(*nds, false);
                menu.open = false;
                swallowMenuButtons = true;
                break;
            case M_RESET:
                nds->Reset();
                nds->SetupDirectBoot("game.nds");
                nds->Start();
                rc_reset();
                say("GAME RESET");
                menu.open = false;
                swallowMenuButtons = true;
                break;
            case M_QUIT:
#ifdef __SWITCH__
                // A game chosen from the game list goes back to the list (the program starts
                // again once this one has saved and closed); + there goes to the Homebrew Menu.
                if (SwitchPlatform::CanChangeGame()) SwitchPlatform::ChangeGame();
#endif
#ifdef SSR_PC_LAUNCHER
                // (The PC's all-games program likewise: QUIT goes back to its game list.)
                if (PcLauncher::CanChangeGame()) PcLauncher::ChangeGame();
#endif
                running = false;
                break;
            default:
                break;
            }
            if (hit >= 0) buildMenu();
        }

        bool anyHeld = false;
        for (int i = 0; i < PAD_COUNT; i++) anyHeld |= ps.btn[i];
        if (swallowMenuButtons && !anyHeld) swallowMenuButtons = false;

        bool paused = menu.open;
        int speed = 1;
        if (!paused && !swallowMenuButtons)
        {
            PadState toGame = ps;
            if (cfg.startMenu && ctrlStart) toGame.btn[PAD_START] = touchStart; // (START is the menu's)
            keys |= PadKeys(toGame, cfg.byLabel);
            // Only presses made while the command strip is up count; anything done in the bag or
            // party screens must not move it afterwards, and its next showing starts from the ROM.
            const bool stripUp = singleScreenFlag && (singleScreenFlag[1] == 1 || singleScreenFlag[1] == 2);
            if (!stripUp) { stripNav.clear(); stripPubSel = -1; }
            // (SWAP SCREENS given a button of its own in CONTROLS works without the touch cursor.)
            if (!cfg.padTouch && cfg.controls.pad[Controls::SWAP] >= 0 && pressed(PAD_RS)) focus.manualFlip = !focus.manualFlip;
            if (cfg.padTouch)
            {
                if (pressed(PAD_RS)) focus.manualFlip = !focus.manualFlip;
#ifndef __SWITCH__
                // (Not on the Switch: a left stick pressed while walking would change the view.)
                if (pressed(PAD_LS)) layout = LayoutNext(layout);
#endif
                if (fabsf(ps.rx) > 0.2f) curDX += ps.rx;
                if (fabsf(ps.ry) > 0.2f) curDY += ps.ry;
                if (ps.lt > 0.3f) curPress = true;
            }
        }
        // Fast forward (4x) while the right trigger (ZR on the Switch) or Left Shift is held, or
        // locked on from the menu.
        // Fast forward runs several frames at once and shows the last, up to 4. As many as fit
        // in about 33 ms by the last time's measure (ffFrameMs): where the emulation itself is
        // the limit (the Switch, 10 ms or more a frame) running 4 at once is no faster than 3, it
        // only shows fewer of them.
        if (!paused && (fastLock || kbFast || ps.rt > 0.5f))
            speed = ffFrameMs > 0 && !opt.headless ? std::clamp((int)(33.3 / ffFrameMs), 2, 4) : 4; // (tests: always 4)
        prevPad = ps;

        // A decompilation build with SINGLE_SCREEN puts everything on the engine of the touch
        // screen. It carries a tag, searched for in main memory once the game has unpacked itself,
        // and a flag next to it that the game sets while a scene needs both screens (the intro).
        if (!singleScreenRom && !opt.layoutGiven && frame % 30 == 15 && frame < 1800)
        {
            static const char tag[] = "SSRECOMP-SINGLE-SCREEN-1";
            const uint8_t* ram = nds->MainRAM;
            const uint8_t* end = ram + nds->MainRAMMask + 1;
            const uint8_t* hit = std::search(ram, end, tag, tag + sizeof(tag) - 1);
            if (hit != end)
            {
                singleScreenRom = true;
                singleScreenFlag = hit + 32; // the build info struct: 32 bytes of tag, then the flag
                preSingleLayout = layout.mode;
                fprintf(stderr, "single-screen build detected\n");
            }
        }
        // The ROM says per scene whether it wants both screens (the intro movie).
        if (singleScreenRom && !opt.layoutGiven)
            layout.mode = *singleScreenFlag ? preSingleLayout : LayoutMode::Merged;
        SDL_Rect rects[2];
        LayoutRects(layout, outW, outH, rects);
        const bool merged = layout.mode == LayoutMode::Merged;
        const MergedView view = MergedMakeView(outW, outH);
        // A single-screen build's visible screen is the touch screen: touches land on it directly.
        auto touchAt = [&](float bx, float by, int& tx, int& ty) {
            if (!singleView()) return MergedTouch(scene, bx, by, tx, ty);
            if (bx < 0 || bx >= 256 || by < 0 || by >= 192) return false;
            tx = (int)bx;
            ty = (int)by;
            return true;
        };
        // Widescreen 3D: on while the overworld is running in the merged view on a wide
        // output. It is drawn at the size of the picture, up to 1440 lines.
        static int wideFrames = 0;
        {
            int ww = opt.headless && opt.composeW ? opt.composeW : outW, wh = opt.headless && opt.composeH ? opt.composeH : outH;
            // Drawing it costs time in proportion to its size; cfg.wideLines caps its height
            // and the result is enlarged to the picture. Where it cannot keep up (drawn by the
            // processor: the
            // emulation waited for it more than 1.5 ms in each of the last 3 frames: a move's
            // animation full of see-through effects), fewer lines for a while, down to 192, a
            // fifth fewer each time; back up a tenth at a time after 2 seconds without waiting.
            // LOWER DETAIL WHEN BUSY in the menu (busy_detail) turns this on.
            // (Not in the tests without a window, whose pictures must not depend on timing.)
            static int adaptLines = 0, slowFrames = 0, calmFrames = 0;
            static uint64_t lastWideNs = 0;
            if (!adaptLines || adaptLines > cfg.wideLines) adaptLines = cfg.wideLines;
            if (!cfg.autoDetail) adaptLines = cfg.wideLines; // (LOWER DETAIL WHEN BUSY off)
#ifdef SSR_DEKO3D
            else if (!opt.headless && melonDS::WideGpuCurrent)
            {
                // Drawn by the graphics chip, which shares its time with SDL's drawing of each
                // frame: the measure is its own time on the widescreen pictures per frame, over
                // the last 30 frames (a game drawing its world every other frame needs half). In
                // a battle (its world drawn every frame) a 720-line picture took 15 ms of the
                // 16.7, and the frames ran at 17-18 ms; over 11 ms a frame, a tenth fewer lines,
                // down to 360; back up a tenth at a time while the picture would still take
                // under 9.5 ms a frame, after 2 seconds.
                static uint64_t lastBusy = 0, window = 0;
                static int windowFrames = 0;
                const uint64_t busy = melonDS::WideGpuBusyNs.load();
                window += busy >= lastBusy ? busy - lastBusy : 0;
                lastBusy = busy;
                // (Fast forward has a fixed lower detail, below; its frames say nothing of
                // normal play.)
                if (speed > 1) window = windowFrames = 0;
                else if (++windowFrames >= 30)
                {
                    const double perFrame = window / 1e6 / windowFrames;
                    window = 0;
                    windowFrames = 0;
                    const int floorLines = std::min(360, cfg.wideLines);
                    if (perFrame > 11.0 && adaptLines > floorLines)
                    {
                        adaptLines = std::max(floorLines, adaptLines * 9 / 10);
                        calmFrames = 0;
                    }
                    else if (adaptLines < cfg.wideLines && (calmFrames += 30) >= 120)
                    {
                        const int up = std::min(cfg.wideLines, adaptLines + std::max(8, cfg.wideLines / 10));
                        const double scale = (double)up / adaptLines;
                        if (perFrame * scale * scale < 9.5) adaptLines = up;
                        calmFrames = 0;
                    }
                }
            }
#endif
            else if (!opt.headless)
            {
                const uint64_t ns = SsrStats().WaitWideNs.load();
                const uint64_t waited = ns >= lastWideNs ? ns - lastWideNs : ns; // (the timing lines reset it)
                lastWideNs = ns;
                if (speed > 1) {} // (fast forward: see above)
                else if (waited > 1500000)
                {
                    calmFrames = 0;
                    if (++slowFrames >= 3 && adaptLines > 192)
                    {
                        adaptLines = std::max(192, adaptLines * 4 / 5);
                        slowFrames = 0;
                    }
                }
                else
                {
                    slowFrames = 0;
                    if (waited < 300000 && ++calmFrames >= 120 && adaptLines < cfg.wideLines)
                    {
                        adaptLines = std::min(cfg.wideLines, adaptLines + std::max(8, cfg.wideLines / 10));
                        calmFrames = 0;
                    }
                }
            }
            WideLinesNow = std::min(adaptLines, cfg.wideLines);
            // While fast forwarding, at most 360 lines (not in the tests without a window) with
            // FAST FORWARD LOW RES on: it shows one frame in several, and the detail comes
            // straight back when it stops.
            if (speed > 1 && !opt.headless && cfg.ffLowRes) WideLinesNow = std::min(WideLinesNow, 360);
            if (wh > WideLinesNow)
            {
                ww = ww * WideLinesNow / wh;
                wh = WideLinesNow;
            }
            // Black 2: the renderer keeps going for 5 seconds after a wide layout and through a
            // whole battle (its party and Bag pages are not wide), so that coming back to the
            // world or the arena shows it straight away instead of a few frames with bars.
            static uint32_t lastWideScene = 0;
            // (And through the menus opened from the X menu, the Bag and the party and the like,
            // which can be up for longer: going back from them to the X menu showed the
            // overworld at 4:3 for a few frames while the renderer started again. It draws
            // nothing meanwhile, the world being paused.)
            static const char* const keepNames[] = { "bag", "party", "pokedex", "pokedex-list", "trainer-card", "options",
                "summary", "town-map", "start-menu" };
            const bool fromMenu = std::any_of(std::begin(keepNames), std::end(keepNames), [&](const char* n) { return !strcmp(scene.name, n); });
            if (funcWatchOn && (scene.wide || (fromMenu && lastWideScene && frame - lastWideScene < 300))) lastWideScene = frame;
            const bool bw2KeepWide = funcWatchOn && (scene.wide || (lastWideScene && frame - lastWideScene < 300) ||
                funcWatch.Ran(bw2.battle, 2) || funcWatch.Ran(pt.battle, 2));
            const bool wantWide = merged && cfg.wide && !opt.noWide && !opt.lockstep && view.width >= 300 && (funcWatchOn ? bw2KeepWide :
                (fieldActive() || menuReturn > 0 || scene.edgeFill || (singleView() && singleFieldFrame && rc_current_frame() - singleFieldFrame < 20)));
            melonDS::Wide.Owner = wantWide ? &nds->GPU.GPU3D : nullptr;
            melonDS::Wide.ViewWidth = view.width;
            // (Only the Black games' views need polygons continued to the edges: see WideState.)
            melonDS::Wide.ExtendEdges = funcWatchOn && !pt.on;
            melonDS::Wide.Width = ww;
            melonDS::Wide.Height = wh;
            wideFrames = wantWide ? wideFrames + 1 : 0;
        }
        // Filled in after the frame has run: the renderer hands over its picture then.
        WideImage wideImage;
        bool battleRenderLater = false; // see BattleGrab
        auto takeWideImage = [&]() {
            wideImage = WideImage();
            battleRenderLater = false;
            // A battle on a wide view: the screen widened (battlewide.h) instead of the bars.
            if (scene.edgeFill && cfg.wide && !opt.noWide)
            {
                WideImage world;
                if (wideFrames > 3 && melonDS::Wide.Front)
                {
                    world.pixels = melonDS::Wide.Front;
                    world.w = melonDS::Wide.FrontW;
                    world.h = melonDS::Wide.FrontH;
                }
                const float vw = MergedMakeView(outW, outH).width;
                const int tu = topUnitOf();
                const ScreenImage topImage = { nds->GPU.Framebuffer[nds->GPU.FrontBuffer][singleView() ? 1 : 0].get(),
                    static_cast<GPU2D::SoftRenderer&>(nds->GPU.GetRenderer2D()).LayerFlags[tu] };
                const int extend = (int)std::ceil((vw - 256) / 2);
                if (vw >= 300 && pipelined && BattleSplit(battleWide, 0))
                {
                    if (BattleGrab(battleWide, *nds, topImage, extend, &world, vw, tu, 0))
                    {
                        scene.wide = true;
                        battleRenderLater = true;
                        return;
                    }
                }
                else if (vw >= 300 && battleWide.Build(*nds, topImage, extend, &world, vw, wideImage, tu))
                {
                    scene.wide = true;
                    return;
                }
            }
            else
                battleWide.Reset();
            // (Not while the screen shows no 3D at all: its display off, or the 3D layer not
            // shown. Black 2 has that for a frame between CONTINUE and the season's screen, with
            // the world already drawn: it flashed up for that frame.)
            // (The 3D is always engine A's.)
            const u32 dispA = nds->GPU.GPU2D_A.DispCnt;
            const bool show3D = ((dispA >> 16) & 3) == 1 && (dispA & (1 << 8)) && (dispA & (1 << 3));
            const auto& wideUnit = topUnitOf() == 0 ? nds->GPU.GPU2D_A : nds->GPU.GPU2D_B;
            if (wideFrames > 3 && melonDS::Wide.Front && show3D)
            {
                wideImage.pixels = melonDS::Wide.Front;
                wideImage.w = melonDS::Wide.FrontW;
                wideImage.h = melonDS::Wide.FrontH;
                // Fades: the game darkens or whitens its screen with the master brightness
                // register, or with a brightness effect on the 3D layer.
                const auto& unit = wideUnit;
                int mode = (unit.MasterBrightness >> 14) & 3, f = std::min(16, unit.MasterBrightness & 0x1F);
                if ((mode != 1 && mode != 2) || !f)
                {
                    int effect = (unit.BlendCnt >> 6) & 3;
                    if ((unit.BlendCnt & 1) && effect >= 2)
                    {
                        mode = effect == 2 ? 1 : 2;
                        f = std::min<int>(16, unit.EVY);
                    }
                    else
                        mode = 0;
                }
                wideImage.fadeMode = mode;
                wideImage.fade = f;
            }
            if (opt.logSections)
            {
                // Where the widescreen picture comes and goes, and why.
                static int last = -1;
                int now = (wideImage.pixels ? 1 : 0) | (scene.wide ? 2 : 0) | (melonDS::Wide.Owner ? 4 : 0);
                if (now != last)
                    fprintf(stderr, "[wide] frame %u: picture %d, scene %s wide %d, renderer on %d, fade %d/%d\n", frame,
                        now & 1, scene.name, (now >> 1) & 1, (now >> 2) & 1, wideImage.fadeMode, wideImage.fade);
                last = now;
            }
        };
        if (!paused)
        {
            static bool wasMouseDown = false;
            if (!opt.headless && mouseDown)
            {
                if (merged)
                {
                    float bx = (mouseX - view.originX) / view.unit, by = (mouseY - view.originY) / view.unit;
                    // (The Running Shoes button drawn beside SoulSilver's X menu: see shoesStep.)
                    if (shoesRect[2] > 0 && bx >= shoesRect[0] && bx < shoesRect[0] + shoesRect[2] && by >= shoesRect[1] &&
                        by < shoesRect[1] + shoesRect[3])
                    {
                        if (!wasMouseDown && shoesStep == 0) { shoesStep = 1; shoesStepAt = frame; }
                    }
                    else if (touchAt(bx, by, touchX, touchY)) touching = true;
                    // (Each press in game.log: where it was and what it touched, for reports of
                    // touches that did nothing.)
                    if (!wasMouseDown)
                        fprintf(stderr, "[touch] %d,%d: picture %.0f,%.0f, scene %s: %s %d,%d\n", mouseX, mouseY, bx, by,
                                scene.name, touching ? "touch screen" : "nothing", touching ? touchX : -1, touching ? touchY : -1);
                }
                else if (LayoutTouch(rects[1], mouseX, mouseY, touchX, touchY)) touching = true;
            }
            wasMouseDown = !opt.headless && mouseDown;
            if (curDX != 0 || curDY != 0 || curPress)
            {
                // Faster with a fuller stick deflection, for both precision and reach.
                float mag = sqrtf(curDX * curDX + curDY * curDY);
                float step = 1.0f + 3.5f * mag;
                cursorX += curDX * step;
                cursorY += curDY * step;
                // In the merged view the cursor can also reach pieces drawn beside the 4:3 area.
                float margin = merged ? (view.width - 256) / 2 : 0;
                if (cursorX < -margin) cursorX = -margin;
                if (cursorX > 255 + margin) cursorX = 255 + margin;
                if (cursorY < 0) cursorY = 0;
                if (cursorY > 191) cursorY = 191;
                cursorShownUntil = frame + 180;
            }
            if (curPress && !touching)
            {
                if (!merged)
                {
                    touching = true;
                    touchX = (int)cursorX;
                    touchY = (int)cursorY;
                }
                else if (touchAt(cursorX, cursorY, touchX, touchY)) touching = true;
            }
        }

        // Battle buttons (the panel in the lower right; the single-screen build's strip): the
        // arrows move the outline and A presses the chosen button by touching it on the touch
        // screen. The game's own cursor is not used (its red corners would show on the
        // touch screen's picture), so the arrows and A do not reach the game here.
        {
            static std::string stripKind;
            static uint32_t prevStripKeys = 0;
            static int stripTouch = 0, stripTouchX = 0, stripTouchY = 0;
            const bool cmd = merged && !strcmp(scene.name, "battle-cmd"), moves = merged && !strcmp(scene.name, "battle-moves");
            if (!paused && (cmd || moves))
            {
                if (stripKind != scene.name)
                {
                    stripKind = scene.name;
                    battleSel = 0;
                    prevStripKeys = keys; // a key still held from before does not count
                }
                const uint32_t edge = keys & ~prevStripKeys;
                prevStripKeys = keys;
                static const int cmdX[4] = { 128, 39, 216, 128 }, cmdY[4] = { 84, 168, 168, 172 };
                static const int movX[4] = { 64, 192, 64, 192 }, movY[4] = { 52, 52, 116, 116 };
                // Empty move slots are skipped: theirs is a plain grey where a move's name is white.
                const u32* screenImagesPrev = nds->GPU.Framebuffer[nds->GPU.FrontBuffer][1].get();
                auto usable = [&](int i) {
                    if (cmd) return !battleFightOnly || i == 0;
                    uint32_t c = screenImagesPrev[(movY[i] - 14) * 256 + movX[i]];
                    // (Platinum's moves are on a pale yellow panel.)
                    if (pt.on) return ((c >> 16) & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200 && (c & 0xFF) > 140;
                    return ((c >> 16) & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200 && (c & 0xFF) > 200;
                };
                if (singleView())
                {
                    // (The single-screen build's strip: one row.)
                    for (int dir : { (edge & (1 << KEY_RIGHT)) ? 1 : 0, (edge & (1 << KEY_LEFT)) ? -1 : 0 })
                        if (dir)
                            for (int i = battleSel + dir; i >= 0 && i < 4; i += dir)
                                if (usable(i)) { battleSel = i; break; }
                }
                else
                {
                    // The panel's arrangement. Commands (0 FIGHT, 1 BAG, 2 POKeMON, 3 RUN): FIGHT
                    // above BAG, RUN, POKeMON. Moves: two by two, an empty slot is not chosen.
                    static const int8_t cmdTo[4][4] = { // left, right, up, down
                        { 1, 2, 0, 3 }, { 1, 3, 0, 1 }, { 3, 2, 0, 2 }, { 1, 2, 0, 3 } };
                    static const int8_t movTo[4][4] = { { 0, 1, 0, 2 }, { 0, 1, 1, 3 }, { 2, 3, 0, 2 }, { 2, 3, 1, 3 } };
                    const int keyOf[4] = { KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN };
                    for (int k = 0; k < 4; k++)
                        if (edge & (1 << keyOf[k]))
                        {
                            const int to = (cmd ? cmdTo : movTo)[battleSel][k];
                            if (usable(to)) battleSel = to;
                        }
                }
                if (cmd)
                {
                    if (edge & (1 << KEY_LEFT)) stripNav += 'L';
                    if (edge & (1 << KEY_RIGHT)) stripNav += 'R';
                    if (edge & (1 << KEY_UP)) stripNav += 'U';
                    if (edge & (1 << KEY_DOWN)) stripNav += 'D';
                }
                if (opt.logSections && edge) fprintf(stderr, "[strip] frame %u: keys %03x, chosen %d\n", frame, edge, battleSel);
                if (edge & (1 << KEY_A))
                {
                    stripTouch = 4;
                    // The chosen button is the one outlined: battleSel, except on the command
                    // strip of the single-screen build, which has its own cursor (stripSel).
                    // (Taking stripSel for the panel too pressed FIGHT whatever was chosen.)
                    const int pick = cmd && singleView() ? stripSel : battleSel;
                    stripTouchX = cmd ? cmdX[pick] : movX[pick];
                    stripTouchY = cmd ? cmdY[pick] : movY[pick];
                }
                // B in the move list touches CANCEL. Given to the game as a key, the first
                // press only brought up its own cursor. On the command row it does nothing.
                if (moves && (edge & (1 << KEY_B)))
                {
                    stripTouch = 4;
                    stripTouchX = 128;
                    stripTouchY = 172;
                }
                keys &= ~((1u << KEY_LEFT) | (1u << KEY_RIGHT) | (1u << KEY_UP) | (1u << KEY_DOWN) | (1u << KEY_A) | (1u << KEY_B));
            }
            else
                stripKind.clear();
            if (!paused && stripTouch > 0)
            {
                touching = true;
                touchX = stripTouchX;
                touchY = stripTouchY;
                stripTouch--;
            }
        }

        // SoulSilver and HeartGold's Running Shoes button (see shoesPix): its picture kept while the
        // game shows it, Y beside the X menu toggling it, and the steps that do it.
        if (probeStartMenu >= 0 && !paused)
        {
            const u32* tb = nds->GPU.Framebuffer[nds->GPU.FrontBuffer][1].get();
            auto nearC = [&](int x, int y, uint32_t c) {
                const u32 p = tb[y * 256 + x];
                return std::abs((int)((p >> 16) & 255) - (int)(c >> 16)) < 14 && std::abs((int)((p >> 8) & 255) - (int)((c >> 8) & 255)) < 14 &&
                    std::abs((int)(p & 255) - (int)(c & 255)) < 14;
            };
            const bool menuOpenNow = probeActive(probeStartMenu);
            if (getenv("SSR_SHOES_DEBUG") && frame % 50 == 0)
                fprintf(stderr, "[shoes] frame %u: field %d menu %d px %06x %06x %06x scene %s\n", frame, fieldActive(), menuOpenNow,
                    tb ? tb[100 * 256 + 183] & 0xFFFFFF : 0, tb ? tb[100 * 256 + 186] & 0xFFFFFF : 0, tb ? tb[93 * 256 + 230] & 0xFFFFFF : 0, scene.name);
            // (Its dark outline, its orange rim and the orange top of its face: on or off.)
            if (tb && fieldActive() && !menuOpenNow && nearC(186, 100, 0xBA6910) && nearC(186, 120, 0xBA6910) && nearC(230, 93, 0xEBAA59))
            {
                for (int y = 0; y < SHOES_H; y++)
                    for (int x = 0; x < SHOES_W; x++) shoesPix[y * SHOES_W + x] = tb[(SHOES_Y + y) * 256 + SHOES_X + x] | 0xFF000000u;
                if (!shoesSeen) fprintf(stderr, "[shoes] frame %u: the Running Shoes button seen\n", frame);
                shoesSeen = true;
                shoesOn = nearC(200, 120, 0xBA6910); // (off: the shoes grey on the orange)
            }
            static uint32_t prevShoesKeys = 0;
            const uint32_t edge = keys & ~prevShoesKeys;
            prevShoesKeys = keys;
            if (shoesRect[2] > 0 && !strcmp(scene.name, "start-menu"))
            {
                if ((edge & (1u << KEY_Y)) && shoesStep == 0)
                {
                    shoesStep = 1;
                    shoesStepAt = frame;
                }
                keys &= ~(1u << KEY_Y); // (the game's own Y, the registered item, does nothing in the menu)
            }
            // 1: B closes the menu; 2: once the overworld is back without it, wait a moment;
            // 3: the button touched; 4: X opens the menu again. (Given up after 2 seconds.)
            const uint32_t since = frame - shoesStepAt;
            if (shoesStep == 1)
            {
                keys |= 1u << KEY_B;
                if (since >= 3) { shoesStep = 2; shoesStepAt = frame; }
            }
            else if (shoesStep == 2)
            {
                if (!menuOpenNow && fieldActive() && since >= 8) { shoesStep = 3; shoesStepAt = frame; }
                else if (since > 120) shoesStep = 0;
            }
            else if (shoesStep == 3)
            {
                touching = true;
                touchX = SHOES_X + SHOES_W / 2;
                touchY = SHOES_Y + SHOES_H / 2;
                if (since >= 4) { shoesStep = 4; shoesStepAt = frame; }
            }
            else if (shoesStep == 4)
            {
                if (since >= 10) keys |= 1u << KEY_X;
                if (since >= 13)
                {
                    shoesStep = 0;
                    fprintf(stderr, "[shoes] frame %u: running shoes %s\n", frame, shoesOn ? "on" : "off");
                }
            }
        }

        // Platinum's introduction: two of Professor Rowan's questions take only a touch, and the
        // game ignores the buttons there (PtFuncs::IntroTouch): his YES / NO ("Please touch a
        // button on the Touch Screen below"), where A touches YES and B touches NO, and "Touch the
        // button on the middle of the Poke Ball", where A touches it. So a controller can answer.
        if (pt.on && funcWatchOn && !paused && funcWatch.Ran(pt.intro, 2))
        {
            static uint32_t prevIntroKeys = 0;
            static int introTouch = 0, introX = 0, introY = 0;
            const uint32_t edge = keys & ~prevIntroKeys;
            prevIntroKeys = keys;
            const int kind = PtFuncs::IntroTouch(nds->GPU.Framebuffer[nds->GPU.FrontBuffer][1].get());
            const uint32_t answer = kind == PtFuncs::INTRO_YESNO ? (1u << KEY_A) | (1u << KEY_B) : kind ? (1u << KEY_A) : 0;
            if (answer || introTouch > 0)
            {
                if (answer && introTouch == 0 && (edge & answer))
                {
                    introTouch = 4;
                    introX = kind == PtFuncs::INTRO_YESNO ? 120 : 128;
                    introY = kind == PtFuncs::INTRO_BALL ? 96 : (edge & (1u << KEY_A)) ? 81 : 121;
                    fprintf(stderr, "[intro] frame %u: touched %s\n", frame,
                            kind == PtFuncs::INTRO_BALL ? "the Poke Ball" : introY == 81 ? "YES" : "NO");
                }
                // (Given to the game, A would only bring up "Please touch a button" or "Not that
                // button!".)
                keys &= ~answer;
            }
            if (introTouch > 0)
            {
                touching = true;
                touchX = introX;
                touchY = introY;
                introTouch--;
            }
        }

        if (ref && !paused)
        {
            TraceLog.clear();
            TracePos = 0;
            TraceFrame = frame;
            ref->SetKeyMask(~keys & 0xFFF);
            if (touching) ref->TouchScreen((u16)touchX, (u16)touchY);
            else ref->ReleaseScreen();
            ref->RunFrame();
            TracePrevR15 = nds->ARM9.R[15];
            TracePrevCPSR = nds->ARM9.CPSR;
        }

        perfMark(0);
        int got = 0;
        if (!paused)
        {
            // Black 2 asks after CONTINUE whether to launch the C-Gear's wireless
            // communications (if the C-Gear was on when the game was saved), which nothing here
            // can take part in. With cgear_start=off in the settings (the default) the question
            // is never seen: while the start menu's CONTINUE runs, the view is black and the
            // sound silent, the player's buttons are kept from it, and it is answered as fast
            // as the game takes it: NO, then YES to "some functions of the C-Gear will be
            // restricted. Is that OK?". The answer's button is tapped every other two frames: held,
            // it hurries the message along; each tap goes on past a pause or answers the
            // question once it is up. Back at the start menu later (a reset), the same again.
            cgearHidden = false;
            if (funcWatchOn && cfg.cgearOff)
            {
                if (!funcWatch.Ran(bw2.startContinue, 600))
                {
                    cgearStep = 0;
                    cgearAsked = false;
                }
                // (Black also for a moment after: CONTINUE does not run in every frame of it,
                // and the start menu fades out behind it.)
                if (funcWatch.Ran(bw2.startContinue, 30))
                {
                    cgearHidden = true;
                    keys &= ~((1u << KEY_A) | (1u << KEY_B) | (1u << KEY_UP) | (1u << KEY_DOWN) | (1u << KEY_LEFT) |
                        (1u << KEY_RIGHT) | (1u << KEY_START) | (1u << KEY_SELECT));
                }
                if (funcWatch.Ran(bw2.startContinue, 1))
                {
                    const bool asking = funcWatch.Ran(bw2.confirmDialog, 1);
                    if (cgearAsked && !asking && cgearStep < 2)
                    {
                        // (The question has gone: answered.)
                        fprintf(stderr, "[bw2] frame %u: C-Gear question answered: %s\n", frame, cgearStep == 0 ? "no" : "yes, restricted is OK");
                        cgearStep++;
                    }
                    cgearAsked = asking;
                    if (cgearStep < 2 && (frame & 3) < 2) keys |= 1u << (cgearStep == 0 ? KEY_B : KEY_A);
                }
            }
            // SoulSilver's new game starts with a menu on the touch screen alone ("Please touch any
            // topic you'd like to know more about on the lower screen": CONTROL INFO, ADVENTURE
            // INFO, NO INFO NEEDED) that the single view does not show, and A alone chooses the
            // first topic again and again. It is answered here, NO INFO NEEDED touched, as it
            // comes up: its three buttons are in the touch screen's picture (a teal 24,186,195 at
            // x 60 of rows 40, 96 and 152, and at x 200 of row 96) while the professor's overlay
            // runs but his speech has not started.
            if (soulSilverRom && !touching)
            {
                static unsigned skipFrames = 0;
                const u32* b = nds->GPU.Framebuffer[nds->GPU.FrontBuffer][singleView() ? 0 : 1].get();
                auto teal = [&](int x, int y) {
                    const u32 p = b[y * 256 + x];
                    return std::abs((int)((p >> 16) & 255) - 24) < 16 && std::abs((int)((p >> 8) & 255) - 186) < 16 &&
                        std::abs((int)(p & 255) - 195) < 16;
                };
                // (Not also waiting for the professor's application to be idle: on the JIT its probe
                // runs throughout, the menu included, since the probes found it.)
                const bool menu = sectionActive("OVY_53") && teal(60, 40) && teal(60, 96) && teal(60, 152) && teal(200, 96);
                skipFrames = menu ? skipFrames + 1 : 0;
                // (Touched for 6 frames in every 30, after it has been up for 20: a touch while it
                // is still sliding in, or held on, is not taken.)
                if (menu && skipFrames >= 20 && skipFrames % 30 < 6)
                {
                    touching = true;
                    touchX = 132;
                    touchY = 152;
                    if (skipFrames % 30 == 0) fprintf(stderr, "[ss] frame %u: NO INFO NEEDED chosen on the new game's first menu\n", frame);
                }
            }
            nds->SetKeyMask(~keys & 0xFFF);
            if (touching) nds->TouchScreen((u16)touchX, (u16)touchY);
            else nds->ReleaseScreen();

            emuSpeed = speed;
            // (No in-between frames while fast forwarding: only one frame in several is shown.)
            nds->GPU.GPU3D.InterpolationEnabled = cfg.fps60 && !opt.lockstep && speed == 1;
            if (ahead)
            {
                // Room for this frame's picture: at most two waiting to be shown.
                while (aheadCount >= 2)
                    if (!presentDue(false))
                    {
                        renderAhead();
                        if (!presentDue(false)) std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0002, secondsToTick())));
                    }
                emuThread.Kick();
                // The newest waiting picture's widened battle picture, from what its frame
                // grabbed, made now that the next frame runs (and before that grabs again),
                // into the picture's own memory.
                if (aheadCount)
                {
                    DrawJob& nj = aheadJobs[(aheadHead + aheadCount - 1) % 3];
                    nj.KeepLater();
                    if (nj.battleRender)
                    {
                        BattleRender(battleWide, nj.wide, 0);
                        nj.battleRender = false;
                        nj.KeepWide();
                    }
                }
                // The waiting pictures shown as their times come while the frame runs.
                for (;;)
                {
                    if (presentDue(false)) continue;
                    renderAhead();
                    if (presentDue(false)) continue;
                    if (emuThread.WaitFor(aheadCount ? std::max(0.0001, secondsToTick()) : 0.1)) break;
                }
                perfEmu += emuMs;
            }
            else if (pipelined && pendingJob.valid)
            {
                // The last frame is shown while this one is emulated.
#ifdef SSR_GPU3D
                // The 3D on the graphics chip is drawn on this thread, between the turns of
                // SDL's drawing (see GPU3D_GLThread.h): what the emulation handed over at the end
                // of the last frame, in one turn (switching OpenGL contexts costs), before the
                // next frame starts (it needs the DS-resolution picture at once); anything
                // handed over during a frame (several frames at a time when fast forwarding)
                // while waiting for it.
                if (auto* gl = melonDS::GLThreadRenderer::Current) gl->Service();
#endif
                emuThread.Kick();
                drawFrame(pendingJob);
                pendingJob.valid = false;
#ifdef SSR_GPU3D
                if (melonDS::GLThreadRenderer::Current)
                    emuThread.WaitServicing([] { if (auto* gl = melonDS::GLThreadRenderer::Current) gl->Service(); });
                else
#endif
                emuThread.Wait();
                perfEmu += emuMs;
            }
            else
            {
                runFrames();
                if (pipelined) perfEmu += emuMs;
            }
            got = emuGot;
            perfDsFrames += emuSpeed;
            if (emuSpeed > 1) ffFrameMs = ffFrameMs > 0 ? ffFrameMs * 0.8 + 0.2 * emuMs / emuSpeed : emuMs / emuSpeed;
            // (Silent while the C-Gear's question is answered unseen.)
            if (cgearHidden && got > 0) std::fill(audioBuf.begin(), audioBuf.begin() + (size_t)got * 2, (int16_t)0);
        }
        else if (pendingJob.valid)
        {
            drawFrame(pendingJob);
            pendingJob.valid = false;
        }
        else if (ahead && aheadCount)
        {
            // (Not emulating, as while paused: the waiting pictures shown in turn.)
            while (aheadCount)
                if (!presentDue(false)) std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0002, secondsToTick())));
        }
        perfMark(1);
        sceneMark(-1);
        frame++;
#ifdef __SWITCH__
        if (frame == 300) SwitchPlatform::JitStarted();
#endif
        if (melonDS::WideGpuCurrent && ++WideGpuFrames == 600 && !WideGpuMarker.empty())
        {
            remove(WideGpuMarker.c_str());
            fprintf(stderr, "[widegpu] 600 frames: kept on\n");
        }
#ifdef SSR_GPU3D
        if (UseGpu3D && ++Gpu3DFrames == 600 && !Gpu3DMarker.empty())
        {
            remove(Gpu3DMarker.c_str());
            Gpu3DTrace("600 frames drawn: kept on");
        }
        // Test aid: SSR_GPU3D_TOGGLE=<frame> switches the 3D renderer there, as the menu does.
        static const uint32_t gpu3dToggleAt = getenv("SSR_GPU3D_TOGGLE") ? (uint32_t)atoi(getenv("SSR_GPU3D_TOGGLE")) : 0;
        if (gpu3dToggleAt && frame == gpu3dToggleAt) ApplyRenderer3D(*nds, !UseGpu3D);
#endif
        if (!holdScript) scriptClock++;

        // Test aid: SSR_PEEK="name=0xADDR>off>off;..." prints, every SSR_PEEK_EVERY frames (60), a
        // word of the console's memory, following pointers (each > reads the word at the address
        // so far and adds the offset after it), for test scripts that follow the game's state.
        static const char* peekSpec = getenv("SSR_PEEK");
        if (peekSpec && frame % (getenv("SSR_PEEK_EVERY") ? (uint32_t)atoi(getenv("SSR_PEEK_EVERY")) : 60u) == 0)
        {
            auto word = [&](uint32_t a) -> uint32_t {
                if (a < 0x02000000 || a >= 0x03000000) return 0;
                uint32_t v;
                memcpy(&v, &nds->MainRAM[a & nds->MainRAMMask], 4);
                return v;
            };
            std::string line = "[peek] " + std::to_string(frame);
            std::stringstream specs(peekSpec);
            std::string item;
            while (std::getline(specs, item, ';'))
            {
                const size_t eq = item.find('=');
                std::string name = eq == std::string::npos ? item : item.substr(0, eq);
                std::string expr = eq == std::string::npos ? item : item.substr(eq + 1);
                std::stringstream parts(expr);
                std::string part;
                uint32_t addr = 0;
                bool first = true;
                while (std::getline(parts, part, '>'))
                {
                    const uint32_t v = (uint32_t)strtoul(part.c_str(), nullptr, 0);
                    addr = first ? v : word(addr) + v;
                    first = false;
                }
                line += " " + name + "=" + std::to_string((int32_t)word(addr));
            }
            fprintf(stderr, "%s\n", line.c_str());
        }
        if (opt.traceFuncsAt.count(frame))
        {
            char name[64];
            snprintf(name, sizeof(name), "/funcs_%06u.txt", frame);
            std::ofstream out((opt.shotDir.empty() ? std::string(".") : opt.shotDir) + name);
            unsigned now = rc_current_frame();
            for (auto& tp : traceProbes)
                if (rc_probe_last_frame(tp.first) + 1 >= now) out << tp.second << "\n";
            if (funcWatchOn)
            {
                out.close();
                // SSR_FUNCS_WINDOW=<frames> widens the window (test aid: functions that run once a step).
                const char* window = getenv("SSR_FUNCS_WINDOW");
                funcWatch.Dump((opt.shotDir.empty() ? std::string(".") : opt.shotDir) + name, window ? (unsigned)atoi(window) : 1);
            }
        }

        if (opt.logSections)
        {
            static std::string lastActive;
            std::string active;
            for (u32 i = 0; i < rc_num_sections; i++)
                if (rc_section_last_frame[i] == rc_frame && strcmp(rc_sections[i].name, "main") && strcmp(rc_sections[i].name, "ITCM") &&
                    strcmp(rc_sections[i].name, "bios9"))
                    active += std::string(" ") + rc_sections[i].name;
            if (active != lastActive)
            {
                fprintf(stderr, "[sections] frame %u:%s\n", frame, active.c_str());
                lastActive = active;
            }
        }

        if (ref && !paused)
        {
            if (!TraceFailed && TracePos != TraceLog.size())
            {
                fprintf(stderr, "LOCKSTEP MISMATCH frame %u: recomp ran %zu instructions, interpreter %zu\n",
                    frame - 1, TracePos, TraceLog.size());
                TraceFailed = true;
            }
            if (TraceFailed) break;
            if (frame % 300 == 0)
                fprintf(stderr, "[lockstep] %u frames identical (%zu instructions in the last one)\n", frame, TraceLog.size());
        }

        const u32* top = nds->GPU.Framebuffer[nds->GPU.FrontBuffer][singleView() ? 1 : 0].get();
        const u32* bot = nds->GPU.Framebuffer[nds->GPU.FrontBuffer][singleView() ? 0 : 1].get();
        // The two screens with their per-pixel layer flags, for the merged view.
        auto& renderer2d = static_cast<GPU2D::SoftRenderer&>(nds->GPU.GetRenderer2D());
        const int topUnit = topUnitOf();
        ScreenImage screenImages[2] = {
            { top, renderer2d.LayerFlags[topUnit], renderer2d.EdgeColour[topUnit], renderer2d.EdgeBlend[topUnit], renderer2d.EdgeFlat[topUnit] },
            { bot, renderer2d.LayerFlags[1 - topUnit], renderer2d.EdgeColour[1 - topUnit], renderer2d.EdgeBlend[1 - topUnit], renderer2d.EdgeFlat[1 - topUnit] },
        };
        const bool fieldNow = rc_probe_last_frame(probeFieldMap) + 1 >= rc_current_frame() && rc_probe_last_frame(probeFieldMap);
        for (u32 sec = 0; sec < rc_num_sections; sec++)
        {
            // (A section that misses a frame or two still counts as running; only a gap of
            // more than three frames starts the count again.)
            const unsigned last = rc_section_last_frame[sec];
            if (!last || last + 3 < rc_current_frame()) sectionStreak[sec] = 0;
            else if (last == rc_current_frame()) sectionStreak[sec]++;
        }
        bool appBottom = false;
        for (const RcApp* app : focus.bottomApps)
            if (appActive(app->name)) appBottom = true;
        appBottomNow = appBottom;
        if (top && bot && !paused) focus.Update(top, bot, fieldNow, appBottom);
        sceneMark(0);
        layout.swapped = focus.Swapped();
        // While the touch cursor is in use the bottom screen has to be big enough to aim at.
        if (!merged && focus.mode == 0 && frame < cursorShownUntil) layout.swapped = true;
        LayoutRects(layout, outW, outH, rects);
        if (!paused)
        {
            // A new layout has to be asked for a few frames in a row before it replaces the
            // one on screen, so that the frames of a transition in which the game is between
            // two states do not flash a third layout. The battle panel stays longer: the
            // game stops polling it while its buttons animate between menus.
            const bool writing = saveWriting();
            fieldHeldBySave = writing && fieldActive();
            Scene picked = funcWatchOn ? pickSceneBw2(view.width, screenImages)
                                       : pickScene(frame < cursorShownUntil, view.width, screenImages);
            if (funcWatchOn) bw1.Adjust(picked, view.width, screenImages); // Pokemon Black and White
            // A whole screen shown alone whose sides are one colour, not black (a fade to white:
            // the starter chosen, whiting out and its message on white) fills the view with that
            // colour instead of a 4:3 square with black bars beside it.
            // (SoulSilver and HeartGold: the Black games have layouts of their own for theirs.)
            if (!funcWatchOn && picked.items.size() == 1 && !picked.wide && picked.bgScreen < 0)
            {
                const Element& el = picked.items[0];
                if (el.screen >= 0 && el.sx == 0 && el.sy == 0 && el.sw == 256 && el.sh == 192 && screenImages[el.screen].pixels)
                {
                    const uint32_t* px = screenImages[el.screen].pixels;
                    const uint32_t c0 = px[0] & 0xFFFFFF;
                    bool uniform = c0 != 0;
                    for (int y = 0; y < 192 && uniform; y++)
                        for (int x : { 0, 1, 2, 3, 252, 253, 254, 255 })
                            if ((px[y * 256 + x] & 0xFFFFFF) != c0)
                            {
                                uniform = false;
                                break;
                            }
                    if (uniform)
                    {
                        picked.bgScreen = el.screen;
                        picked.bgX = 0;
                        picked.bgY = 0;
                    }
                }
            }
            // An application's own layout stays while it fades out: the game is still ending it
            // (and its picture is still what is on the screens) when only a general layout
            // would be chosen, until the overworld runs again or a moment has passed.
            {
                static Scene lastApp;
                static uint32_t lastAppFrame = 0;
                static const char* const appScenes[] = { "bag", "summary", "phone", "pc-box", "map", "pokedex",
                    "pokedex-list", "pokedex-page", "mart", "naming" };
                // (Platinum's own: frontend/platinum.h.)
                const bool isApp = std::any_of(std::begin(appScenes), std::end(appScenes),
                    [&](const char* n) { return !strcmp(picked.name, n); }) || (pt.on && PtFuncs::AppScene(picked.name));
                const bool generic = !strcmp(picked.name, "top") || !strcmp(picked.name, "bottom") || !strcmp(picked.name, "spread");
                if (isApp)
                {
                    lastApp = picked;
                    lastAppFrame = frame;
                }
                else if (generic && lastAppFrame && frame - lastAppFrame < 90 && !fieldNow && !appBottomNow)
                    // (Not when another application has started, e.g. the party after a summary.)
                    picked = lastApp;
                else if (!generic)
                    lastAppFrame = 0;
            }
            // With the X menu open the game covers the bottom of the overworld with a strip
            // that points at the other screen. It means nothing here, so the layers it is
            // drawn on are left out and the overworld shows through.
            {
                int topUnit = topUnitOf();
                // The strip of the X menu on the top screen: 2D across the full width of its
                // lower rows, with the 3D world above it.
                const uint8_t* tf = screenImages[0].flags;
                auto is2D = [&](int x, int y) { uint8_t c = LayerClass(tf[y * 256 + x]); return c != LAYER_3D && c != LAYER_BACKDROP; };
                // Only while the overworld's code is what is running: a battle's opening has
                // the same shape on screen.
                const bool strip = tf && sectionActive("field") && !sectionActive("OVY_12") &&
                    is2D(0, 152) && is2D(255, 152) && is2D(0, 189) && is2D(255, 189) && LayerClass(tf[170 * 256 + 128]) == LAYER_OBJ &&
                    LayerClass(tf[60 * 256 + 128]) == LAYER_3D && LayerClass(tf[100 * 256 + 20]) == LAYER_3D;
                if (probeActive(probeStartMenu) && fieldActive()) menuReturn = 0;
                else if (strip && merged) menuReturn = 30;
                else if (menuReturn > 0) menuReturn--;
                bool menuOpen = merged && ((probeActive(probeStartMenu) && fieldActive()) || menuReturn > 0);
                rc_hide_layers[topUnit] = menuOpen ? 0x18 : 0;
                rc_hide_layers[1 - topUnit] = 0;
            }
            // The start menu, the layouts of the applications opened from it, and the plain
            // bottom screen (the party).
            auto menuScene = [&](const char* n) {
                static const char* const names[] = { "start-menu", "bottom", "bag", "summary", "phone", "pc-box", "map",
                    "pokedex", "pokedex-list", "pokedex-page", "mart", "naming" };
                return std::any_of(std::begin(names), std::end(names), [&](const char* m) { return !strcmp(n, m); }) ||
                    (pt.on && PtFuncs::AppScene(n));
            };
            static std::string shownName, pendingName;
            static int pending = 0;
            if (writing && !shownName.empty())
                // The layout on screen stays until the save is written (see probeSaveWrite).
                pending = 0;
            else if (shownName.empty() || shownName == picked.name ||
                // (Back from a menu to the X menu: at once, not the menu's layout over the X menu's
                // screens for a few frames.)
                (!strcmp(picked.name, "start-menu") && menuScene(shownName.c_str())) ||
                // Between menus, with both screens black nothing on screen changes, so the new
                // layout can take over at once and is in place when the picture fades in.
                // (Not around battles: there the timing of the layouts decides which buttons
                // are on screen.)
                (AutoFocus::Blank(top) && AutoFocus::Blank(bot) && !sectionActive("OVY_12") && !sectionActive("OVY_8") &&
                 (menuScene(picked.name) || menuScene(shownName.c_str()))))
            {
                scene = picked;
                shownName = picked.name;
                pending = 0;
            }
            else
            {
                if (pendingName != picked.name) pending = 0;
                pendingName = picked.name;
                int need = shownName == "bottom" && fieldNow ? 1 :
                    shownName == "battle-menu" && (pendingName == "top" || pendingName == "battle") ? 40 :
                    (shownName == "starter" || shownName == "intro") && pendingName == "top" ? 16 : 4;
                if (++pending >= need)
                {
                    scene = picked;
                    shownName = picked.name;
                    pending = 0;
                }
                else if (pendingName.compare(0, 6, "battle") != 0)
                {
                    // Held while the next layout (not another of the battle's) waits its turn:
                    // without its holes and the pieces drawn elsewhere (Black 2's HP boxes), which
                    // belong to the screen they were made for. A battle fading out into an
                    // evolution showed the evolution's world through the holes.
                    for (Element& el : scene.items) el.holeCount = 0;
                    scene.items.erase(std::remove_if(scene.items.begin(), scene.items.end(),
                        [](const Element& el) { return el.wideOnly; }), scene.items.end());
                }
            }
        }
        sceneMark(1);
        // Black 2: a picture in 2D covering the world (the Surf and Fly cut-ins) is shown at 4:3:
        // continuing it beside the screen would only make streaks.
        // (Back to wide only after 10 frames without one: the cut-in's flashes come in between.)
        if (funcWatchOn)
        {
            static uint32_t pictureFrame = 0;
            if (scene.wide && MergedEdgesArePicture(scene, screenImages)) pictureFrame = frame ? frame : 1;
            // Screens drawn on below their last line (Scene::extraRows): copies with the rows
            // added, two of each in turn (the pipeline shows one while the next is made).
            for (int s = 0; s < 2; s++)
            {
                const int extra = std::max(scene.extraRows[s], 0), skip = scene.redrawSkip[s];
                if ((extra <= 0 && skip < 0) || !screenImages[s].pixels) continue;
                static std::vector<uint32_t> tallPx[2][2];
                static std::vector<uint8_t> tallFlags[2][2];
                std::vector<uint32_t>& px = tallPx[s][frame & 1];
                std::vector<uint8_t>& fl = tallFlags[s][frame & 1];
                px.assign((size_t)256 * (192 + extra), 0);
                fl.assign((size_t)256 * (192 + extra), 0);
                memcpy(px.data(), screenImages[s].pixels, 256 * 192 * 4);
                if (screenImages[s].flags) memcpy(fl.data(), screenImages[s].flags, 256 * 192);
                const int unitNum = s == 0 ? topUnit : 1 - topUnit;
                if (skip >= 0)
                {
                    // Drawn again without the layers left out (and the list's scroll undone);
                    // the sprites kept as they were (moved with the scroll).
                    int unscroll = 0;
                    if (scene.redrawUnscroll[s] >= 0)
                    {
                        auto& r2d = static_cast<GPU2D::SoftRenderer&>(nds->GPU.GetRenderer2D());
                        unscroll = r2d.LineScroll[unitNum][0][scene.redrawUnscroll[s] & 3][1] & 511;
                        if (unscroll >= 256) unscroll -= 512;
                    }
                    std::vector<uint32_t> again((size_t)256 * (192 + extra));
                    std::vector<uint8_t> againFlags((size_t)256 * (192 + extra));
                    if (RenderScreenRows(*nds, unitNum, 0, 192 + extra, again.data(), againFlags.data(), skip, unscroll))
                    {
                        const size_t n = again.size();
                        // The sprites seen at each place of the unscrolled list (the saved game's
                        // character on Black's card, scrolled off the top of the screen as the
                        // cursor goes down the list, is shown as it was last seen).
                        static std::vector<uint32_t> spriteSeen[2];
                        static std::vector<uint8_t> spriteSeenFlags[2];
                        if (spriteSeen[s].size() != n)
                        {
                            spriteSeen[s].assign(n, 0);
                            spriteSeenFlags[s].assign(n, 0);
                        }
                        for (size_t i = 0; i < n; i++)
                        {
                            // (A sprite drawn at row y shows at row y + unscroll.)
                            const long src = (long)i - (long)unscroll * 256;
                            if (src >= 0 && src < 256 * 192)
                            {
                                const bool sprite = LayerClass(fl[src]) == LAYER_OBJ;
                                spriteSeenFlags[s][i] = sprite ? fl[src] : 0;
                                spriteSeen[s][i] = px[src];
                                if (!sprite) continue;
                                again[i] = px[src];
                                againFlags[i] = fl[src];
                            }
                            else if (spriteSeenFlags[s][i])
                            {
                                again[i] = spriteSeen[s][i];
                                againFlags[i] = spriteSeenFlags[s][i];
                            }
                        }
                        for (size_t i = 0; i < n; i++)
                        {
                            px[i] = again[i];
                            fl[i] = againFlags[i];
                        }
                    }
                }
                else if (!RenderBelowScreen(*nds, unitNum, extra, px.data() + 256 * 192, fl.data() + 256 * 192))
                    for (int r = 0; r < extra; r++) // (the last line, repeated)
                        memcpy(px.data() + 256 * (192 + r), screenImages[s].pixels + 256 * 191, 256 * 4);
                screenImages[s].pixels = px.data();
                screenImages[s].flags = fl.data();
                screenImages[s].edge = nullptr;
                screenImages[s].edgeBlend = nullptr;
                screenImages[s].edgeFlat = nullptr;
            }
            scene.narrow = scene.wide && pictureFrame && frame - pictureFrame < 10;
        }
        if (opt.logSections)
        {
            static bool lastNarrow = false;
            if (scene.narrow != lastNarrow) fprintf(stderr, "[wide] frame %u: %s\n", frame, scene.narrow ? "4:3 (a 2D picture covers the edges)" : "wide again");
            lastNarrow = scene.narrow;
            static std::string lastScene;
            if (lastScene != scene.name)
            {
                lastScene = scene.name;
                fprintf(stderr, "[scene] frame %u: %s\n", frame, lastScene.c_str());
                g_sceneShots.insert(frame);
                g_sceneShots.insert(frame + 2);
                g_sceneShots.insert(frame + 20);
            }
        }

        if (audio && got > 0)
        {
            // How much longer than a DS frame the frames are taking: above full speed's 1 the
            // sound is stretched to match (see AudioStretch), steered towards 100 ms of sound
            // waiting in the queue. Averaged over about a second, so that frames that come
            // quicker and slower in turn do not make the music speed up and slow down with
            // them (the queue takes up the difference).
            const int queued = (int)SDL_GetQueuedAudioSize(audio) / 4;
            const uint64_t t = SDL_GetPerformanceCounter();
            if (audioTick)
            {
                const double dt = (double)(t - audioTick) / perfFreq;
                if (dt < 0.25) audioSlow += (std::min(dt / frameTime, 4.0) - audioSlow) * 0.03;
            }
            audioTick = t;
            double slowdown = speed == 1 ? audioSlow : 1.0;
            if (audioStretch.Stretching() && speed == 1) slowdown *= 1.0 + 0.5 * (4800 - queued) / 4800.0;
            audioOut.clear();
            audioStretch.Process(audioBuf.data(), got, slowdown, audioOut);
            // Drop audio rather than let latency grow if the queue backs up.
            if (queued < 48000 / 4 && !audioOut.empty())
                SDL_QueueAudio(audio, audioOut.data(), (Uint32)(audioOut.size() * 2));
        }
        if (opt.audioStats && got > 0)
        {
            for (int i = 0; i < got * 2; i++)
            {
                int v = abs(audioBuf[i]);
                if (v > AudioPeak) AudioPeak = v;
                AudioSumSq += (double)audioBuf[i] * audioBuf[i];
            }
            AudioSamples += (uint64_t)got * 2;
        }

        if (opt.countChanges && top)
        {
            static uint64_t lastTop = 0;
            uint64_t h = Fnv1a(top, 256 * 192 * 4);
            if (h != lastTop) TopChanges++;
            lastTop = h;
        }

        if (opt.posEvery && frame % opt.posEvery == 0)
        {
            // sFieldSysPtr -> location (map id) and playerAvatar -> mapObject -> current tile.
            auto rd = [&](uint32_t a) -> uint32_t {
                if (a < 0x02000000 || a >= 0x02400000) return 0;
                uint32_t v;
                memcpy(&v, nds->MainRAM + (a & nds->MainRAMMask), 4);
                return v;
            };
            uint32_t fs = rd(0x021D4158), loc = rd(fs + 0x20), obj = rd(rd(fs + 0x40) + 0x30);
            printf("pos %u map %u x %u z %u\n", frame, rd(loc), rd(obj + 0x64), rd(obj + 0x6C));
        }
        if (opt.hashEvery && frame % opt.hashEvery == 0)
        {
            uint64_t fb = Fnv1a(top, 256 * 192 * 4);
            fb = Fnv1a(bot, 256 * 192 * 4, fb);
            uint64_t ram = Fnv1a(nds->MainRAM, nds->MainRAMMask + 1);
            printf("frame %u fb %016llx ram %016llx pc9 %08x pc7 %08x\n", frame,
                (unsigned long long)fb, (unsigned long long)ram, nds->ARM9.R[15], nds->ARM7.R[15]);
        }

        sceneMark(2);
        // Overlay: touch cursor, status message, pause menu.
        bool overlayUsed = false;
        canvas.Resize(outW, outH);
        canvas.Clear();
        const SDL_Rect cursorRect = merged
            ? SDL_Rect { (int)view.originX, (int)view.originY, (int)(256 * view.unit), (int)(192 * view.unit) }
            : rects[1];
        if (frame < cursorShownUntil && cursorRect.w > 0 && !paused)
        {
            int cx = cursorRect.x + (int)(cursorX * cursorRect.w / 256);
            int cy = cursorRect.y + (int)(cursorY * cursorRect.h / 192);
            int arm = cursorRect.w / 40 + 3;
            int t = outH / 360 + 1;
            canvas.Fill(cx - arm - 1, cy - t - 1, arm * 2 + 3, t * 2 + 3, 0xFF000000);
            canvas.Fill(cx - t - 1, cy - arm - 1, t * 2 + 3, arm * 2 + 3, 0xFF000000);
            canvas.Fill(cx - arm, cy - t, arm * 2 + 1, t * 2 + 1, curPress ? 0xFFFFD75E : 0xFFFFFFFF);
            canvas.Fill(cx - t, cy - arm, t * 2 + 1, arm * 2 + 1, curPress ? 0xFFFFD75E : 0xFFFFFFFF);
            overlayUsed = true;
        }
        // The top right corner, below the on-screen R button when those controls are shown
        // (their top row ends at 12% of the height).
        int cornerY = touchPadUsed && touchPad.visible ? outH * 13 / 100 : 0;
        if (cfg.showFps && !opt.headless)
        {
            int sc = outH / 240 < 1 ? 1 : outH / 240;
            const std::string text = std::to_string(shownFps) + " FPS";
            const int tw = Canvas::TextWidth(text, sc) + 8 * sc;
            canvas.Fill(outW - tw, cornerY, tw, 13 * sc, 0xC0000000);
            canvas.Text(outW - tw + 4 * sc, cornerY + 3 * sc, text, sc, 0xFFFFFFFF);
            cornerY += 13 * sc;
            overlayUsed = true;
        }
        if (speed > 1)
        {
            int sc = outH / 240 < 1 ? 1 : outH / 240;
            canvas.Text(outW - Canvas::TextWidth(">>", sc) - 4 * sc, cornerY + 4 * sc, ">>", sc, 0xFFFFFFFF);
            overlayUsed = true;
        }
#if defined(__ANDROID__) && !defined(SSR_PC_LAUNCHER)
        if (!updateSaid && UpdateCheck::g_done)
        {
            updateSaid = true;
            if (!UpdateCheck::g_newer.empty())
            {
                say("VERSION " + UpdateCheck::g_newer + " IS OUT - REBUILD THE APP ON YOUR PC");
                toastUntil = frame + 600;
            }
        }
#endif
        // SoulSilver and HeartGold: the Running Shoes button beside the X menu (see shoesPix),
        // left of the menu's panel at its foot, its button named under it.
        shoesRect[2] = 0;
        if (merged && shoesSeen && !paused && !strcmp(scene.name, "start-menu") && scene.items.size() >= 2)
        {
            const Element& panel = scene.items.back();
            const float sc = panel.scale;
            const float w = SHOES_W * sc, h = SHOES_H * sc;
            const float x = panel.dx - w - 6, y = panel.dy + 192 * sc - h - 14;
            shoesRect[0] = x; shoesRect[1] = y; shoesRect[2] = w; shoesRect[3] = h;
            const int ox = (int)(view.originX + x * view.unit), oy = (int)(view.originY + y * view.unit);
            const int ow = (int)(w * view.unit), oh = (int)(h * view.unit);
            for (int py = 0; py < oh; py++)
            {
                const int sy = std::min(SHOES_H - 1, py * SHOES_H / std::max(1, oh));
                for (int px = 0; px < ow; px++)
                {
                    const int sx = std::min(SHOES_W - 1, px * SHOES_W / std::max(1, ow));
                    canvas.Fill(ox + px, oy + py, 1, 1, shoesPix[sy * SHOES_W + sx]);
                }
            }
            const int tsc = std::max(1, outH / 360);
            const std::string label = std::string(pads.Empty() ? Controls::KeyName(cfg.controls.Key(Controls::Y))
                : Controls::InputName(cfg.controls.Pad(Controls::Y, cfg.byLabel), Controls::StyleOf(pads.controllers.empty() ? nullptr : pads.controllers[0])))
                + ": RUN " + (shoesOn ? "ON" : "OFF");
            const int tw = Canvas::TextWidth(label, tsc);
            canvas.Fill(ox + (ow - tw) / 2 - 3 * tsc, oy + oh + 2 * tsc, tw + 6 * tsc, 11 * tsc, 0xC0000000);
            canvas.Text(ox + (ow - tw) / 2, oy + oh + 4 * tsc, label, tsc, 0xFFFFFFFF);
            overlayUsed = true;
        }
        if (frame < toastUntil)
        {
            int sc = outH / 240 < 1 ? 1 : outH / 240;
            const int tw = Canvas::TextWidth(toast, sc) + 8 * sc;
            // With on-screen controls, centred below their top row (L, MENU and R end at
            // 12% of the height) instead of under the L button.
            const bool controls = touchPadUsed && touchPad.visible;
            const int tx = controls ? (outW - tw) / 2 : 0, ty = controls ? outH * 13 / 100 : 0;
            canvas.Fill(tx, ty, tw, 13 * sc, 0xC0000000);
            canvas.Text(tx + 4 * sc, ty + 3 * sc, toast, sc, 0xFFFFFFFF);
            overlayUsed = true;
        }
        if (menu.open)
        {
            // The buttons named as the controller in use has them (PlayStation's CROSS and CIRCLE,
            // ...), and as CONTROLS has them; with no controller, the keyboard's keys.
            const bool kbOnly = pads.Empty() && !(touchPadUsed && touchPad.visible);
            auto btn = [&](int a) {
                return kbOnly ? std::string(a == Controls::A ? "ENTER" : "ESC")
                              : std::string(Controls::InputName(cfg.controls.Pad(a, cfg.byLabel),
                                    Controls::StyleOf(pads.controllers.empty() ? nullptr : pads.controllers[0])));
            };
            std::string hint = touchPadUsed && touchPad.visible ? "TAP: CHOOSE   OUTSIDE: BACK"
                               : btn(Controls::A) + ": SELECT   " + btn(Controls::B) + ": BACK";
            const int selId = menu.selected >= 0 && menu.selected < (int)menuIds.size() ? menuIds[menu.selected] : -1;
            static const char* pageTitle[] = { "", "KEYS AND BUTTONS", "DISPLAY", "GRAPHICS", "CONTROLS", "CHEATS", "CHEAT UI" };
            if (menuPage == PAGE_KEYS)
            {
                if (!(touchPadUsed && touchPad.visible)) hint = btn(Controls::A) + ": CHANGE   " + btn(Controls::B) + ": BACK";
                if (capturing >= 0) hint = ctlKeyboard ? "ESC: LEAVE IT AS IT WAS" : "OR WAIT 5 SECONDS: AS IT WAS";
                else if (menu.selected == 0 && ctlHasKeyboard) hint = "CHANGE: KEYBOARD OR CONTROLLER";
                else if (menu.selected == ctlRow0 + Controls::SWAP) hint = "SWAPS WHICH SCREEN IS LARGE";
#ifdef __SWITCH__
                else if (menu.selected == ctlRow0 + Controls::MENU && cfg.controls.pad[Controls::MENU] < 0)
                    hint = cfg.startMenu ? "+ OPENS IT (START BUTTON)" : "+ AND - OPEN IT";
#endif
                menu.Draw(canvas, pageTitle[PAGE_KEYS], hint);
            }
            else
            {
#if !defined(__SWITCH__) && !defined(SSR_PHONE)
            // (What FAST FORWARD LOW RES is for, while it is the one chosen.)
            if (selId == M_FFLOWRES) hint = "ON: MORE FPS IN FAST FORWARD";
#endif
            // (What START BUTTON does, while it is the one chosen.)
            if (selId == M_STARTBTN) hint = cfg.startMenu ? "START OPENS THIS MENU" : "START IS THE DS START";
            if (selId == M_CONTROLS) hint = "GIVE ANY BUTTON ANOTHER KEY OR BUTTON";
            if (selId == M_CHEATS) hint = cheats.items.empty() ? "NONE FOUND: SEE THE README" : "ACTION REPLAY CODES";
            if (selId == M_CHEATMENU) hint = "DYNAMIC WIDTH / TEXT SIZE";
            if (selId == M_CHEATWIDTH) hint = "ON: FITS THE CHEAT NAMES TO THE WINDOW";
            if (selId == M_CHEATTEXT) hint = "0.1X TO 5X IN 0.1X STEPS";
            if (selId == M_NOTHING) hint = "GET USRCHEAT.DAT: SEE THE README";
            std::string note;
            if (selId >= M_CHEAT0 && selId - M_CHEAT0 < (int)cheatRows.size())
            {
                const auto& row = cheatRows[selId - M_CHEAT0];
                note = row.folder ? cheats.folders[row.index].note : cheats.items[row.index].note;
                if (!(touchPadUsed && touchPad.visible)) hint = btn(Controls::A) + (row.folder ? ": OPEN / CLOSE   " : ": ON / OFF   ") + btn(Controls::B) + ": BACK";
            }
            const MenuStyle style = menuPage == PAGE_CHEATS ? MenuStyle{ cfg.cheatTextSize, cfg.cheatDynamicWidth, true } : MenuStyle{};
            if (menuPage == PAGE_CHEATS && cheatLayout.hint.empty())
                cheatLayout.hint = btn(Controls::A) + ": OPEN / CLOSE   " + btn(Controls::B) + ": BACK";
            menu.Draw(canvas, pageTitle[menuPage], hint, note, style, menuPage == PAGE_CHEATS ? &cheatLayout : nullptr);
            }
            overlayUsed = true;
        }

        bool wantShot = !opt.shotDir.empty() &&
            (opt.shotAt.count(frame) || g_sceneShots.count(frame) || (opt.shotEvery && frame % opt.shotEvery == 0));
        if (wantShot) timingShot = true;
        if (wantShot && opt.shotLayers)
        {
            // BG0 red, BG1 green, BG2 blue, BG3 yellow, backdrop black, 3D magenta, sprites white.
            auto& r2d = static_cast<GPU2D::SoftRenderer&>(nds->GPU.GetRenderer2D());
            int topUnit = topUnitOf();
            std::vector<uint32_t> img(256 * 384);
            for (int scr = 0; scr < 2; scr++)
            {
                const u8* flags = r2d.LayerFlags[scr == 0 ? topUnit : 1 - topUnit];
                const u8* prio = r2d.ObjPrio[scr == 0 ? topUnit : 1 - topUnit];
                for (int i = 0; i < 256 * 192; i++)
                {
                    u8 f = flags[i];
                    uint32_t c = 0x000000;
                    switch (LayerClass(f))
                    {
                    // (sprites: white at priority 0, darker greys at 1-3)
                    case LAYER_OBJ: c = prio[i] == 0 ? 0xFFFFFF : prio[i] == 1 ? 0xC0C0C0 : prio[i] == 2 ? 0x909090 : 0x606060; break;
                    case LAYER_3D: c = 0xFF00FF; break;
                    case LAYER_BG0: c = 0xFF0000; break;
                    case LAYER_BG1: c = 0x00C000; break;
                    case LAYER_BG2: c = 0x0000FF; break;
                    case LAYER_BG3: c = 0xFFFF00; break;
                    default: break;
                    }
                    img[scr * 256 * 192 + i] = c;
                }
            }
            for (int scr = 0; scr < 2; scr++)
            {
                const u8* flags = r2d.LayerFlags[scr == 0 ? topUnit : 1 - topUnit];
                int hist[256] = {};
                for (int i = 0; i < 256 * 192; i++) hist[flags[i]]++;
                fprintf(stderr, "[layers] frame %u %s:", frame, scr == 0 ? "top" : "bottom");
                for (int v = 0; v < 256; v++)
                    if (hist[v]) fprintf(stderr, " %02x=%d", v, hist[v]);
                fprintf(stderr, "\n");
            }
            char lname[64];
            snprintf(lname, sizeof(lname), "/layers_%06u.png", frame);
            WritePNG(opt.shotDir + lname, img.data(), 256, 384);
        }
        if (wantShot && top && bot)
        {
            char name[64];
            snprintf(name, sizeof(name), "/frame_%06u.png", frame);
            if (opt.composeW > 0 && opt.composeH > 0)
            {
                if (merged)
                {
                    takeWideImage();
                    // (A widened battle picture to be made beside the next frame: made now, for
                    // the picture; the drawing job then shows the same.)
                    if (battleRenderLater && BattleRender(battleWide, wideImage, 0)) battleRenderLater = false;
                    MergedCompose(scene, screenImages, opt.composeW, opt.composeH, shot, cfg.smooth, &wideImage);
                }
                else Compose(layout, opt.composeW, opt.composeH, top, bot, shot);
                BlendCanvas(canvas, shot);
                WritePNG(opt.shotDir + name, shot.data(), opt.composeW, opt.composeH);
            }
            else
            {
                shot.resize(256 * 384);
                memcpy(shot.data(), top, 256 * 192 * 4);
                memcpy(shot.data() + 256 * 192, bot, 256 * 192 * 4);
                WritePNG(opt.shotDir + name, shot.data(), 256, 384);
            }
        }

        perfMark(2);
        if (opt.headless && opt.benchCompose && merged && top && bot && opt.composeW)
        {
            takeWideImage();
            MergedCompose(scene, screenImages, opt.composeW, opt.composeH, mergedImage, cfg.smooth, &wideImage);
            perfMark(3);
        }

        if (!opt.headless && top && bot)
        {
            // What the picture needs; drawn now, or with the pipeline while the next frame is
            // emulated (from copies of what that emulation overwrites).
            sceneMark(3);
            if (merged) takeWideImage();
            sceneMark(4);
            DrawJob& job = ahead ? aheadJobs[(aheadHead + aheadCount) % 3] : pipelined ? pendingJob : nowJob;
            job.merged = merged;
            job.overlayUsed = overlayUsed;
            job.wantShot = wantShot && opt.windowShots;
            job.frameNo = frame;
            job.outW = outW;
            job.outH = outH;
            job.rects[0] = rects[0];
            job.rects[1] = rects[1];
            job.top = top;
            job.bot = bot;
            job.screens[0] = screenImages[0];
            job.screens[1] = screenImages[1];
            job.scene = scene;
            job.wide = wideImage;
            job.battleRender = battleRenderLater;
            if (ahead)
            {
                job.Keep(false); // (the layer flags and edges: drawn over line by line)
                job.keepLater = true;
                job.valid = true;
                aheadCount++;
                perfMark(2);
            }
            else if (pipelined)
            {
                job.Keep(speed > 1);
                job.valid = true;
                perfMark(2);
            }
            else
                drawFrame(job);
        }

        if (!opt.stateSave.empty() && scriptClock == opt.stateSaveAt && !holdScript)
        {
            if (!WriteSnapshot(*nds, opt.stateSave, scriptClock)) fprintf(stderr, "snapshot failed\n");
        }

        if (!opt.noSave && frame % 120 == 0) FlushSave(opt.save);
        if (opt.frames && scriptClock >= opt.frames) running = false;
        if (opt.stopAt && frame >= opt.stopAt)
        {
            if (!opt.stateSave.empty() && !WriteSnapshot(*nds, opt.stateSave, frame)) fprintf(stderr, "snapshot failed\n");
            running = false;
        }
    }

    // (The emulation thread is idle between frames: ended before anything else.)
    emuThread.Stop();
    if (timingFile) fclose(timingFile);

    // Shutting down. The save and the settings are written first, before anything that could
    // go wrong; each step is logged, so that a log shows how far it got if it ever stops.
    auto exitStep = [](const char* what) {
        fprintf(stderr, "[exit] %s\n", what);
        fflush(stderr);
    };
    exitStep("saving");
    if (!opt.noSave) FlushSave(opt.save);
    if (!opt.headless)
    {
        if (!singleScreenRom) cfg.layout = LayoutName(layout.mode); // the automatic choice is not the player's setting
        cfg.focusMode = focus.mode;
        cfg.Save(opt.config);
    }
    Sampler::Stop();
    // The 3D render thread must be finished before anything it draws from is torn down.
    exitStep("3D render thread");
    if (auto* soft = dynamic_cast<melonDS::SoftRenderer*>(&nds->GPU.GetRenderer3D()))
        soft->SetThreaded(false, nds->GPU);
    // The widescreen picture may still be being drawn in the background: its threads read
    // data that is destroyed when the program ends (this crashed on exit now and then). Its
    // threads are ended here too, rather than by the program's own clean-up at the very end.
    exitStep("widescreen threads");
    ApplyDSGpu(*nds, false);
    melonDS::WideWait();
    melonDS::WideShutdown();
    melonDS::WideGpuStop();
    if (!WideGpuMarker.empty()) remove(WideGpuMarker.c_str());
    melonDS::Wide.Owner = nullptr;
#ifdef SSR_GPU3D
    // The graphics chip's renderer ends its thread and its OpenGL objects while EGL is still
    // up (SDL_Quit below can take it down).
    if (UseGpu3D)
    {
        exitStep("3D on the graphics chip");
        if (!Gpu3DMarker.empty()) remove(Gpu3DMarker.c_str());
        nds->GPU.GPU3D.SetCurrentRenderer(std::make_unique<melonDS::SoftRenderer>());
        UseGpu3D = false;
    }
#endif
    if (nds->GPU.GPU3D.InterpolationEnabled)
        fprintf(stderr, "[60fps] %u in-between frames drawn in %u frames\n", nds->GPU.GPU3D.InterpolatedFrames, frame);
    if (opt.countChanges) fprintf(stderr, "[changes] top screen changed on %u of %u frames\n", TopChanges, frame);
    if (opt.audioStats && AudioSamples)
        fprintf(stderr, "[audio] %llu samples, peak %d, rms %.1f\n", (unsigned long long)AudioSamples, AudioPeak,
            sqrt(AudioSumSq / AudioSamples));
    if (nds->ARM9.RecompEnable) rc_print_stats();
#ifdef __SWITCH__
    // Closing the sound has now and then never returned on the Switch (the game hung at
    // "[exit] audio", in a test run with nothing else going on), and QUIT after a save has
    // frozen the console with the log ending in a clean "[exit] done", so in what the
    // program's end does after that (the console is taken down here instead, under the
    // watch). Everything that matters is saved by then, so if
    // this does not finish within five seconds the program ends without the rest (back to the
    // HOME Menu rather than the Homebrew Menu).
    std::atomic<bool> sdlClosed { false };
    std::thread exitWatch([&sdlClosed] {
        for (int i = 0; i < 500 && !sdlClosed; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (sdlClosed) return;
        fprintf(stderr, "[exit] closing down did not finish within 5 seconds: ending the program without the rest\n");
        SwitchPlatform::Finish();
        SwitchPlatform::ForceExit();
    });
#endif
    if (!opt.headless)
    {
        if (audio)
        {
            exitStep("audio");
            SDL_PauseAudioDevice(audio, 1);
            SDL_ClearQueuedAudio(audio);
            SDL_CloseAudioDevice(audio);
        }
        exitStep("SDL");
        SDL_Quit();
    }
#ifdef __SWITCH__
    exitStep("console");
    nds.reset();
    sdlClosed = true;
    exitWatch.join();
#endif
#ifdef __SWITCH__
    exitStep("finish");
    SwitchPlatform::Finish();
#endif
    exitStep("done");
#ifdef SSR_PC_LAUNCHER
    PcLauncher::Finish();
#endif
    if (opt.lockstep)
    {
        if (TraceFailed) return 1;
        fprintf(stderr, "[lockstep] PASS: %u frames, every ARM9 instruction matched the interpreter\n", frame);
    }
    return 0;
}
