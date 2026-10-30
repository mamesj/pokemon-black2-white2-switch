// The PC all-games program's game list. See pc_launcher.h.
#ifdef SSR_PC_LAUNCHER
#include "pc_launcher.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <string>
#include <vector>
#include <sys/stat.h>

#ifdef __APPLE__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

// (stb_image's code here, for the list's pictures: only JPEG and PNG.)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "thirdparty/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION
#include "launcher.h"
#include "controls.h"
#include "achievements.h"
#include "ssr_os.h"
#if !defined(SSR_PHONE) && !defined(__SWITCH__)
#include "shortcuts.h"
#endif
#ifdef SSR_GBA
#include "gba_games.h" // Pokemon GBA Widescreen, built in: its GBA games (CMakeLists.txt, SSR_GBA_DIR)
#endif

// How many chunks the program's SoulSilver tables have (none in a build without them).
extern const uint32_t rc_num_chunks;

namespace PcLauncher
{
#ifdef SSR_PC_ART
// The game list's pictures built into the program (pc_art.cpp, made by CMakeLists.txt from the
// builder's SSR_PC_ART folder): the JPEG or PNG for a game code, or null.
const unsigned char* BuiltInArt(const char* code, size_t& size);
#endif
namespace
{
std::vector<std::string> g_args;
std::vector<char*> g_argv;
bool g_canChange = false, g_change = false;
std::string g_name;

bool Exists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void MakeDir(const std::string& path)
{
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0777);
#endif
}

// The program's own folder, without the slash at its end. (On Windows the program is built
// with a manifest that makes the C library's file names UTF-8, so that folders with any
// letters in their names work.)
std::string ProgramDir()
{
    std::string dir = ".";
    if (char* base = SDL_GetBasePath())
    {
        dir = base;
        SDL_free(base);
    }
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir;
}

// The settings file's fullscreen and window lines, so that the list opens as the game last was.
bool ConfigFullscreen(const std::string& cfg, int& w, int& h)
{
    bool on = true; // (borderless fullscreen unless the settings say otherwise, as the game)
    if (FILE* f = fopen(cfg.c_str(), "r"))
    {
        char line[256];
        while (fgets(line, sizeof line, f))
        {
            if (!strncmp(line, "fullscreen=", 11)) on = line[11] == '1';
            int ww = 0, wh = 0;
            if (sscanf(line, "window=%dx%d", &ww, &wh) == 2 && ww >= 320 && wh >= 240 && ww <= 16384 && wh <= 16384)
            {
                w = ww;
                h = wh;
            }
        }
        fclose(f);
    }
    return on;
}

// F11 in the list: the game then starts the same way (the settings file says so).
// Lines of the settings file set (key=value) or, for an empty value, taken out; the others kept.
void SetConfigValues(const std::string& cfg, const std::vector<std::pair<std::string, std::string>>& values)
{
    std::vector<std::string> lines;
    if (FILE* f = fopen(cfg.c_str(), "r"))
    {
        char line[1024];
        while (fgets(line, sizeof line, f))
        {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            bool set = false;
            for (const auto& kv : values) set |= l.size() > kv.first.size() && !l.compare(0, kv.first.size(), kv.first) && l[kv.first.size()] == '=';
            if (!set) lines.push_back(l);
        }
        fclose(f);
    }
    for (const auto& kv : values)
        if (!kv.second.empty()) lines.push_back(kv.first + "=" + kv.second);
    if (FILE* f = fopen(cfg.c_str(), "w"))
    {
        for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
}

void SetConfigFullscreen(const std::string& cfg, bool on)
{
    std::vector<std::string> lines;
    bool found = false;
    if (FILE* f = fopen(cfg.c_str(), "r"))
    {
        char line[1024];
        while (fgets(line, sizeof line, f))
        {
            std::string l = line;
            if (!strncmp(line, "fullscreen=", 11))
            {
                l = std::string("fullscreen=") + (on ? "1" : "0") + "\n";
                found = true;
            }
            lines.push_back(l);
        }
        fclose(f);
    }
    if (!found) lines.push_back(std::string("fullscreen=") + (on ? "1" : "0") + "\n");
    if (FILE* f = fopen(cfg.c_str(), "w"))
    {
        for (const std::string& l : lines) fputs(l.c_str(), f);
        fclose(f);
    }
}

struct Folders
{
    std::string dir, roms, saves, funcs, cfg;
    std::string art; // where the list's own pictures are (icons/IREO.jpg ...)
};

#ifdef __ANDROID__
// The Android app's activity (tools/android/allgames/.../GameActivity.java): its static
// methods, called through JNI.
jclass ActivityClass(JNIEnv* env)
{
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!activity) return nullptr;
    jclass c = env->GetObjectClass(activity);
    env->DeleteLocalRef(activity);
    return c;
}

template <class F>
void CallActivity(const char* method, const char* signature, F call)
{
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;
    jclass c = ActivityClass(env);
    if (!c) return;
    jmethodID m = env->GetStaticMethodID(c, method, signature);
    if (m) call(env, c, m);
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(c);
}

std::string JavaString(const char* method)
{
    std::string out;
    CallActivity(method, "()Ljava/lang/String;", [&](JNIEnv* env, jclass c, jmethodID m) {
        if (jstring str = (jstring)env->CallStaticObjectMethod(c, m))
        {
            const char* utf = env->GetStringUTFChars(str, nullptr);
            if (utf) out = utf;
            env->ReleaseStringUTFChars(str, utf);
            env->DeleteLocalRef(str);
        }
    });
    return out;
}

bool JavaBool(const char* method)
{
    bool out = false;
    CallActivity(method, "()Z", [&](JNIEnv* env, jclass c, jmethodID m) { out = env->CallStaticBooleanMethod(c, m); });
    return out;
}

void JavaVoid(const char* method)
{
    CallActivity(method, "()V", [&](JNIEnv* env, jclass c, jmethodID m) { env->CallStaticVoidMethod(c, m); });
}

// The ROM folder the player chose (GameActivity.romFolder), with the saves in a folder inside
// it; until one is chosen, the app's own folder.
void SetRomFolder(Folders& fo)
{
    fo.roms = JavaString("romFolder");
    fo.saves = (fo.roms.empty() ? fo.dir : fo.roms) + "/saves";
    fo.art = fo.roms.empty() ? fo.dir : fo.roms;
}

// The function tables, packed in the APK (assets/funcs), copied out to the app's folder for
// the emulator, which reads them by file name.
void ExtractFuncs(const Folders& fo)
{
    for (const char* code : { "IREO", "IRDO", "IRBO", "IRAO", "CPUE", "IREF", "IRDF", "CPUF", "IRBF", "IRAF" })
    {
        SDL_RWops* rw = SDL_RWFromFile((std::string("funcs/") + code + ".txt").c_str(), "rb");
        if (!rw) continue;
        std::string data;
        char buf[65536];
        size_t n;
        while ((n = SDL_RWread(rw, buf, 1, sizeof buf)) > 0) data.append(buf, n);
        SDL_RWclose(rw);
        const std::string out = fo.funcs + "/" + code + ".txt";
        struct stat st;
        if (stat(out.c_str(), &st) == 0 && (size_t)st.st_size == data.size()) continue;
        if (FILE* f = fopen(out.c_str(), "wb"))
        {
            fwrite(data.data(), 1, data.size(), f);
            fclose(f);
        }
    }
}
#endif

// Whether this program plays a game: SoulSilver and HeartGold need the program's own tables,
// the others their function table in data/funcs.
bool Playable(const Folders& fo, const Launcher::Game& g)
{
    if (g.platform == Launcher::GBA) return true; // (any GBA game plays, with widescreen or without)
    if (!g.known) return false;
    if (g.code == "IPGE" || g.code == "IPKE" || g.code == "IPGF" || g.code == "IPKF") return rc_num_chunks > 0;
    return Exists(fo.funcs + "/" + g.code + ".txt");
}

// The ROMs in the roms folder (and any left beside the program), their saves in saves/.
std::vector<Launcher::Game> FindGames(const Folders& fo, std::map<std::string, std::vector<uint32_t>>& artCache)
{
    std::vector<Launcher::Game> games;
    for (const std::string& dir : { fo.roms, fo.dir })
    {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (Launcher::EndsWithNds(e->d_name)) names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names)
        {
            Launcher::Game g;
            if (!Launcher::ReadGame(dir + "/" + n, fo.saves, g)) continue;
            // A picture of the player's own (icons/IREO.jpg, or Black 2.jpg beside Black 2.nds),
            // read once.
            auto it = artCache.find(g.rom);
            if (it == artCache.end())
            {
                Launcher::FindArt(g, fo.art);
#ifdef SSR_PC_ART
                // Without one, the picture built into the program (SSR_PC_ART), if any.
                size_t size = 0;
                if (g.art.empty())
                    if (const unsigned char* data = BuiltInArt(g.code.c_str(), size)) Launcher::LoadArt(data, size, g.art);
#endif
                it = artCache.emplace(g.rom, g.art).first;
            }
            g.art = it->second;
            games.push_back(std::move(g));
        }
    }
#ifdef SSR_GBA
    // The GBA games in the same roms folder, their saves in saves/ too (emerald.sav ...); and in a
    // GBA folder inside it, with that folder's saves.
    const size_t ds = games.size();
    GbaGames::AddWithGbaFolder(fo.roms, fo.saves, games, artCache);
    static size_t gbaSeen = (size_t)-1; // (said when it changes: the folder is looked at every 1.5 s)
    if (games.size() - ds != gbaSeen)
    {
        gbaSeen = games.size() - ds;
        fprintf(stderr, "[launcher] GBA games: %zu in %s\n", gbaSeen, fo.roms.c_str());
    }
#endif
    Launcher::AddComingSoon(games);
    {
        Launcher::Game& g = games.back();
        auto it = artCache.find("coming-soon:" + g.code);
        if (it == artCache.end())
        {
            Launcher::FindArt(g, fo.art);
#ifdef SSR_PC_ART
            size_t size = 0;
            if (g.art.empty())
                if (const unsigned char* data = BuiltInArt(g.code.c_str(), size)) Launcher::LoadArt(data, size, g.art);
#endif
            it = artCache.emplace("coming-soon:" + g.code, g.art).first;
        }
        g.art = it->second;
    }
    Launcher::Sort(games);
    return games;
}

void OpenFolder(const std::string& path)
{
#ifdef SSR_IOS
    // (iOS: the Files app, opened at the folder; the app's folder shows there as On My iPhone >
    // Pokémon Fulldex, the app's name.)
    std::string url = "shareddocuments://";
#else
    std::string url = "file://";
#endif
    if (path.empty() || path[0] != '/') url += "/"; // (C:/... on Windows)
    // (Escaped as a URL: a Mac opens none with a space in it, as in "Application Support".)
    static const char hex[] = "0123456789ABCDEF";
    for (char ch : path)
    {
        const unsigned char c = (unsigned char)(ch == '\\' ? '/' : ch);
        if (isalnum(c) || strchr("/-._~:", c) || c >= 0x80) url += (char)c;
        else { url += '%'; url += hex[c >> 4]; url += hex[c & 15]; }
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_OpenURL(url.c_str()) != 0) fprintf(stderr, "[launcher] could not open %s: %s\n", url.c_str(), SDL_GetError());
#endif
}

// The list in its own window until a game is chosen: its index in games (which is updated as
// ROMs come and go), or -1 when the window is closed.
int PickGame(Folders& fo, std::vector<Launcher::Game>& games, int selected, bool& fullscreen, int winW, int winH)
{
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1"); // (taps come as clicks here; the game takes fingers one by one)
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    fullscreen = true;
    // No ROM folder yet, or no access to files: the list asks for them (GameActivity).
    bool needFolder = fo.roms.empty() || !JavaBool("canUseFiles");
    bool wasPicking = false;
    Uint32 lastFolderCheck = 0;
#elif defined(SSR_IOS)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1"); // (taps come as clicks here; the game takes fingers one by one)
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    fullscreen = true;
#endif
    #ifdef _WIN32
    // The program's own icon (resource 1, when it was built with one) for its windows.
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    #endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "[launcher] SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Window* window = SDL_CreateWindow("Pokémon Fulldex", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer)
    {
        fprintf(stderr, "[launcher] no window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    SDL_Texture* texture = nullptr;
    Canvas canvas;
    std::map<std::string, std::vector<uint32_t>> artCache;
    std::vector<SDL_GameController*> pads;
    std::vector<SDL_Joystick*> joys; // (gamepads SDL has no layout for)
    // The tab shown (Launcher::TABS: ALL first, the list opens on it; back from a GBA game, the
    // tab it was chosen on) and how far down it is scrolled. selected: a game in that tab, or -1
    // when it has none.
    static int lastTab = 0;
    int tab = lastTab, scroll = 0, result = -1;
    // false while the list is where a finger (or the mouse, held) dragged it, rather than where
    // the chosen game is; a key, button or tap that chooses a game makes it follow again.
    bool follow = true;
    std::string note;
    auto view = [&]() { return Launcher::MakeView(canvas, games, tab); };
    // Tests, without anyone there: SSR_LIST_TAB=n opens on tab n; SSR_LIST_PICK=a game code chooses
    // that game the first time the list is shown; SSR_LIST_SHOT=file.bmp keeps the list as drawn
    // (after the pick, the next time) in that picture and closes it.
    static int listsShown = 0;
    listsShown++;
    const char* testShot = getenv("SSR_LIST_SHOT");
    const char* testPick = listsShown == 1 ? getenv("SSR_LIST_PICK") : nullptr;
    if (const char* t = getenv("SSR_LIST_TAB")) tab = std::max(0, std::min(Launcher::TABS - 1, atoi(t)));
    // (The game chosen last, if it is in that tab; else the tab's first. The games are in the
    // view's order.)
    if (selected < 0 || selected >= (int)games.size() || !Launcher::InTab(games[selected].platform, tab))
    {
        selected = -1;
        for (size_t i = 0; i < games.size() && selected < 0; i++)
            if (Launcher::InTab(games[i].platform, tab)) selected = (int)i;
    }
    // L and R (Q and E, or a controller's shoulder buttons; a tap on a tab): the next tab, keeping
    // the chosen game if it is in it.
    // (why: said in game.log, with the tab.)
    bool onTabs = false; // the tabs chosen by the arrows (up from the first row of games)
    auto setTab = [&](int t, const char* why) {
        tab = (t + Launcher::TABS) % Launcher::TABS;
        fprintf(stderr, "[launcher] tab %s (%s)\n", Launcher::TabName(tab), why);
        scroll = 0;
        follow = true;
        const Launcher::View v = view();
        if (v.SpotOf(selected) < 0) selected = Launcher::FirstIn(v);
        // (A tab without games: the arrows stay on the tabs, to go on to the next.)
        if (Launcher::FirstIn(v) < 0) onTabs = true;
        note.clear();
    };
    // Held directions repeat, as on the Switch.
    int held = 0, heldDir = 0;
    Uint32 lastScan = SDL_GetTicks();
    bool quit = false;
    // The settings page (Launcher::SettingsPage): H, F1 or a controller's Y; Esc or B back.
    // (Touch: a finger pressed at downY, dragged far enough to scroll the page.)
    int downY = 0, downScroll = 0;
    bool pressed = false, dragging = false;
    int tapPlay = -1; // (the game a finger is on that was already chosen: played when it lifts)
    bool guide = false;
    Launcher::SettingsPage page;
    // The keys and buttons changed in a game's CONTROLS (controls.h): A plays, B goes back, Y opens
    // the settings, the directions move, as they do in the game. Keys and buttons not changed
    // keep doing what they always did here.
    Controls::Bindings ctl;
    ctl.Load(fo.cfg);
    float trig[2] = {}; // (the triggers as last seen: one pushed past half way is a press)
    // The title band's SETTINGS (and BACK on the settings page) can be chosen like a game: up
    // from the top row (the first option), then A.
    bool onSettings = false, onBack = false; // (the title band's SETTINGS / BACK: by the D-pad)
    auto toggleGuide = [&]() {
        guide = !guide;
        onBack = false;
        if (guide) page.Open(fo.cfg);
    };
    // KEYS AND BUTTONS (the settings page's CONTROLS): each action's key or controller button,
    // changed as in the pause menu (main.cpp): choose one, then press what it should be (ESC or 5
    // seconds: as it was). Kept in the settings file's map_ lines (Controls::Bindings::SaveTo).
    bool keysOpen = false, keysKeyboard = false;
    int keysCapture = -1;
    Uint32 keysUntil = 0;
    Menu keysMenu;
    const int keysFirst = 1, keysReset = keysFirst + Controls::COUNT, keysBack = keysReset + 1;
    auto keysStyle = [&]() { return Controls::StyleOf(pads.empty() ? nullptr : pads[0]); };
    auto keysBuild = [&]() {
        keysMenu.items.clear();
        keysMenu.items.push_back({ "CHANGING", keysKeyboard ? "KEYBOARD" : "CONTROLLER" });
        for (int a = 0; a < Controls::COUNT; a++)
            keysMenu.items.push_back({ Controls::ActionName(a),
                keysCapture == a ? (keysKeyboard ? "PRESS A KEY" : "PRESS A BUTTON")
                : keysKeyboard ? Controls::KeyName(ctl.Key(a)) : Controls::InputName(ctl.Pad(a, ctl.byLabel), keysStyle()) });
        keysMenu.items.push_back({ "RESET ALL", "" });
        keysMenu.items.push_back({ "BACK", "" });
    };
    auto keysSaved = [&]() {
        ctl.SaveTo(fo.cfg);
        page.Open(fo.cfg, true); // (the settings page's copy of the file, so it keeps these)
        keysBuild();
    };
    auto keysActivate = [&](int row) {
        if (row == 0) keysKeyboard = !keysKeyboard;
        else if (row >= keysFirst && row < keysReset)
        {
            keysCapture = row - keysFirst;
            keysUntil = SDL_GetTicks() + 5000;
        }
        else if (row == keysReset)
        {
            ctl.Reset();
            keysSaved();
        }
        else if (row == keysBack) keysOpen = false;
        keysBuild();
    };
    // What is pressed while KEYS AND BUTTONS is up: a key or button to give (in = Controls::IN_*,
    // or -1 for a key), else moving through it. True if it was taken.
    auto keysInput = [&](int sc, int in) {
        if (!keysOpen) return false;
        if (keysCapture >= 0)
        {
            if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_AC_BACK) keysCapture = -1;
            else if (sc > 0 && keysKeyboard) { ctl.SetKey(keysCapture, sc); keysCapture = -1; keysSaved(); }
            else if (in >= 0 && !keysKeyboard) { ctl.SetPad(keysCapture, in, ctl.byLabel); keysCapture = -1; keysSaved(); }
            keysBuild();
            return true;
        }
        MenuInput nav;
        const int act = in >= 0 ? (ctl.PadCustom() ? ctl.ActionOfPad(in, ctl.byLabel)
                                   : in == Controls::IN_A ? Controls::A : in == Controls::IN_B ? Controls::B
                                   : in == Controls::IN_UP ? Controls::UP : in == Controls::IN_DOWN ? Controls::DOWN
                                   : in == Controls::IN_LEFT ? Controls::LEFT : in == Controls::IN_RIGHT ? Controls::RIGHT : -1)
                      : -1;
        nav.up = sc == SDL_SCANCODE_UP || act == Controls::UP;
        nav.down = sc == SDL_SCANCODE_DOWN || act == Controls::DOWN;
        nav.left = sc == SDL_SCANCODE_LEFT || act == Controls::LEFT;
        nav.right = sc == SDL_SCANCODE_RIGHT || act == Controls::RIGHT;
        nav.accept = sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || act == Controls::A || act == Controls::START;
        nav.back = sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_AC_BACK || act == Controls::B || act == Controls::SELECT;
        if (nav.back) { keysOpen = false; return true; }
        int dir = 0;
        const int hit = keysMenu.Update(nav, dir);
        if (hit >= 0) keysActivate(hit);
        return true;
    };
    // RETROACHIEVEMENTS' ACCOUNT (the settings page): LOG IN typed here as in the pause menu (the
    // user name, then the password, shown only as a count), sent on a thread of its own
    // (Achievements::LoginForToken); the name and token kept in the settings file as the pause
    // menu keeps them (ra_user=, ra_token=), with ra=1. Chosen when logged in: LOG OUT.
    struct RaLogin
    {
        std::atomic<bool> busy { false }, done { false };
        bool ok = false;
        std::string user, token, error;
    };
    std::shared_ptr<RaLogin> raState = std::make_shared<RaLogin>();
    bool raOpen = false;
    int raField = 0; // 0 the user name, 1 the password
    std::string raUser, raPass, raNote;
    Menu raMenu;
    auto raBuild = [&]() {
        raMenu.items.clear();
        std::string u = raUser.size() > 18 ? raUser.substr(0, 18) : raUser;
        raMenu.items.push_back({ "USER NAME", (u.empty() ? std::string("...") : u) + (raField == 0 ? "_" : ""), true });
        raMenu.items.push_back({ "PASSWORD", std::to_string(raPass.size()) + " CHARACTERS" + (raField == 1 ? "_" : ""), true });
        raMenu.items.push_back({ raState->busy ? "LOGGING IN..." : "LOG IN", "" });
        raMenu.items.push_back({ "CANCEL", "" });
        if (!raState->busy) raMenu.selected = std::min(raMenu.selected, 3);
    };
    auto raAccountShown = [&]() -> std::string {
        if (raState->busy) return "LOGGING IN...";
        const std::string user = Launcher::ConfigValue(fo.cfg, "ra_user");
        if (!Launcher::ConfigValue(fo.cfg, "ra_token").empty())
        {
            std::string u = user.size() > 14 ? user.substr(0, 14) : user;
            for (char& c : u) c = (char)toupper((unsigned char)c);
            return "LOG OUT " + u;
        }
        return "LOG IN";
    };
    auto raClose = [&]() {
        raOpen = false;
        std::fill(raPass.begin(), raPass.end(), '\0');
        raPass.clear();
        SDL_StopTextInput();
    };
    auto raSubmit = [&]() {
        if (raState->busy) return;
        if (raUser.empty()) { raField = 0; raNote = "TYPE YOUR USER NAME"; return; }
        if (raPass.empty()) { raField = 1; raNote = "TYPE YOUR PASSWORD"; return; }
        raNote = "LOGGING IN...";
        raState = std::make_shared<RaLogin>();
        raState->busy = true;
        std::shared_ptr<RaLogin> st = raState;
        const std::string user = raUser, pass = raPass;
        std::fill(raPass.begin(), raPass.end(), '\0');
        raPass.clear();
        std::thread([st, user, pass]() {
            std::string error;
            st->ok = Achievements::LoginForToken(user, pass, std::string("PokemonDSWidescreen/") + SSR_RA_VERSION, st->user, st->token, st->error);
            st->busy = false;
            st->done = true;
        }).detach();
        raBuild();
    };
    // The login's answer, looked at each pass.
    auto raPoll = [&]() {
        if (!raState->done) return;
        raState->done = false;
        if (raState->ok)
        {
            SetConfigValues(fo.cfg, { { "ra", "1" }, { "ra_user", raState->user }, { "ra_token", raState->token } });
            page.Open(fo.cfg, true);
            raNote.clear();
            raClose();
        }
        else
        {
            std::string err = raState->error;
            for (char& c : err) c = (char)toupper((unsigned char)c);
            raNote = "LOGIN FAILED: " + err;
            raField = 1;
            raBuild();
        }
    };
    auto raActivate = [&](int row) {
        if (row == 0 || row == 1) raField = row;
        else if (row == 2) raSubmit();
        else if (row == 3) raClose();
        if (raOpen) raBuild();
    };
    // The list's look (GAME LIST on the settings page: COLOUR, BACKGROUND), read again when one
    // of them changes or the ROM folder (where a phone's picture is) does.
    Launcher::Theme theme;
    Launcher::Background background;
    Launcher::LoadLook(fo.cfg, fo.art, theme, background);
    auto change = [&](int d) {
        const char* key = page.Change(d);
        if (key && !strncmp(key, "list_", 5)) Launcher::LoadLook(fo.cfg, fo.art, theme, background);
        if (key && !strcmp(key, "!ra_login"))
        {
            if (raState->busy) return;
            if (!Launcher::ConfigValue(fo.cfg, "ra_token").empty())
            {
                // LOG OUT: the token forgotten here (the pause menu's LOG OUT does the same).
                SetConfigValues(fo.cfg, { { "ra_token", "" } });
                page.Open(fo.cfg, true);
                return;
            }
            raOpen = true;
            raField = 0;
            raUser = Launcher::ConfigValue(fo.cfg, "ra_user");
            raPass.clear();
            raNote.clear();
            raMenu.selected = raUser.empty() ? 0 : 1;
            if (!raUser.empty()) raField = 1;
            SDL_StartTextInput();
            raBuild();
            return;
        }
        if (key && !strcmp(key, "!keys"))
        {
            ctl.Load(fo.cfg);
            keysOpen = true;
            keysCapture = -1;
            keysKeyboard = pads.empty() && joys.empty();
            keysMenu.selected = keysFirst;
            keysMenu.open = true;
            keysBuild();
            return;
        }
#if !defined(SSR_PHONE) && !defined(__SWITCH__)
        // DESKTOP SHORTCUTS: made at once; what came of it shown as the option's value.
        if (key && !strcmp(key, "!shortcuts"))
        {
            std::string where;
            const int made = Shortcuts::Make(fo.dir, games, where);
            page.items[page.selected].values[0].second =
                games.empty() ? "NO GAMES YET" : made > 0 ? std::to_string(made) + " MADE " + where : "FAILED: " + where;
            return;
        }
#endif
        // (BORDERLESS FULLSCREEN applies to this window too, as F11 does.)
        if (key && !strcmp(key, "fullscreen"))
        {
            int w = 0, h = 0;
            fullscreen = ConfigFullscreen(fo.cfg, w, h);
            SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
    };
    auto choose = [&]() {
        if (guide && onBack)
        {
            // (BACK: from a group to the groups, from the groups to the list.)
            if (page.Back()) onBack = false;
            else toggleGuide();
            return;
        }
        if (guide)
        {
            change(1);
            return;
        }
        if (onSettings)
        {
            toggleGuide();
            return;
        }
        if (onTabs)
        {
            // (A on a tab: down into its games.)
            if (Launcher::FirstIn(view()) >= 0)
            {
                onTabs = false;
                if (view().SpotOf(selected) < 0) selected = Launcher::FirstIn(view());
            }
            return;
        }
#ifdef __ANDROID__
        if (needFolder || games.empty())
        {
            JavaVoid("chooseFolder");
            return;
        }
#endif
        if (games.empty() || selected < 0 || selected >= (int)games.size()) return;
        if (Playable(fo, games[selected]))
        {
            result = selected;
            quit = true;
        }
        else
            note = games[selected].comingSoon ? games[selected].name + ": COMING SOON"
                 : games[selected].known ? "THIS DOWNLOAD DOES NOT HAVE THIS GAME" : "THIS GAME IS NOT ONE THAT THIS PLAYS";
    };
    auto move = [&](int dir) { // 1 left, 2 right, 3 up, 4 down
        if (guide)
        {
            if (onBack)
            {
                if (dir == 4) onBack = false;
                return;
            }
            if (dir == 1 || dir == 2) change(dir == 1 ? -1 : 1);
            else
            {
                const int before = page.selected;
                page.Move(dir == 3 ? -1 : 1);
                if (dir == 3 && page.selected == before) onBack = true; // (up from the first option)
            }
            return;
        }
        if (onSettings)
        {
            if (dir == 4)
            {
                onSettings = false;
                onTabs = true;
            }
            return;
        }
        // The tabs: left and right change the tab, down goes into its games, up to SETTINGS.
        if (onTabs)
        {
            if (dir == 1 || dir == 2) setTab(tab + (dir == 2 ? 1 : -1), "the arrows");
            else if (dir == 3)
            {
                onTabs = false;
                onSettings = true;
            }
            else if (dir == 4 && Launcher::FirstIn(view()) >= 0)
            {
                onTabs = false;
                if (view().SpotOf(selected) < 0) selected = Launcher::FirstIn(view());
            }
            return;
        }
        // Through the tab's games, across its headings (Launcher::Step); up from the top row: the
        // tabs (and up again, the title band's SETTINGS).
        follow = true;
        const Launcher::View v = view();
        const int to = v.spots.empty() ? -1 : Launcher::Step(v, selected, dir);
        if (to < 0)
        {
            if (dir == 3) onTabs = true;
            return;
        }
        if (to != selected)
        {
            selected = to;
            note.clear();
        }
    };
    while (!quit)
    {
        SDL_Event e;
        bool any = SDL_WaitEventTimeout(&e, 50);
        while (any)
        {
            // (RETROACHIEVEMENTS' LOG IN takes the keyboard, the buttons and taps while it is up.)
            if (raOpen && e.type != SDL_QUIT)
            {
                if (e.type == SDL_TEXTINPUT && !raState->busy && raField <= 1)
                {
                    std::string& f = raField == 0 ? raUser : raPass;
                    for (const char* p = e.text.text; *p; p++)
                        if ((unsigned char)*p >= 32 && f.size() < 64) f += *p;
                    raNote.clear();
                    raBuild();
                }
                else if (e.type == SDL_KEYDOWN)
                {
                    const SDL_Scancode sc = e.key.keysym.scancode;
                    if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_AC_BACK) raClose();
                    else if (sc == SDL_SCANCODE_BACKSPACE && raField <= 1)
                    {
                        std::string& f = raField == 0 ? raUser : raPass;
                        if (!f.empty()) f.pop_back();
                        raBuild();
                    }
                    else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
                    {
                        if (raField == 0 && !raUser.empty()) { raField = 1; raMenu.selected = 1; raBuild(); }
                        else raSubmit();
                    }
                    else if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_UP)
                    {
                        raMenu.selected = (raMenu.selected + (sc == SDL_SCANCODE_UP ? 3 : 1)) % 4;
                        if (raMenu.selected <= 1) raField = raMenu.selected;
                        raBuild();
                    }
                }
                else if (e.type == SDL_CONTROLLERBUTTONDOWN)
                {
                    const int b = e.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_BACK) raClose();
                    else if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) raActivate(raMenu.selected);
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                    {
                        raMenu.selected = (raMenu.selected + (b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 3 : 1)) % 4;
                        if (raMenu.selected <= 1) raField = raMenu.selected;
                        raBuild();
                    }
                }
                else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT)
                {
                    int ww = 1, wh = 1;
                    SDL_GetWindowSize(window, &ww, &wh);
                    const int row = raMenu.ItemAt(e.button.x * canvas.w / std::max(1, ww), e.button.y * canvas.h / std::max(1, wh));
                    if (row >= 0)
                    {
                        raMenu.selected = row;
                        raActivate(row);
                        if (raOpen && row <= 1) SDL_StartTextInput(); // (a phone's keyboard, again)
                    }
                }
                else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED || e.type == SDL_JOYDEVICEADDED ||
                         e.type == SDL_JOYDEVICEREMOVED || e.type == SDL_WINDOWEVENT)
                    goto normal;
                any = SDL_PollEvent(&e);
                continue;
            }
            // (KEYS AND BUTTONS takes the keys and buttons while it is up.)
            if (keysOpen && e.type != SDL_QUIT)
            {
                if (e.type == SDL_KEYDOWN && !e.key.repeat) keysInput(e.key.keysym.scancode, -1);
                else if (e.type == SDL_CONTROLLERBUTTONDOWN) keysInput(0, e.cbutton.button);
                else if (e.type == SDL_CONTROLLERAXISMOTION)
                {
                    const int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? 1 : -1;
                    if (t >= 0)
                    {
                        const float v = e.caxis.value / 32767.0f;
                        if (v > 0.5f && trig[t] <= 0.5f) keysInput(0, t ? Controls::IN_RT : Controls::IN_LT);
                        trig[t] = v;
                    }
                }
                else if (e.type == SDL_JOYBUTTONDOWN)
                {
                    bool generic = false;
                    for (SDL_Joystick* j : joys) generic |= SDL_JoystickInstanceID(j) == e.jbutton.which;
                    if (generic) keysInput(0, Controls::JoyButtonInput(e.jbutton.button));
                }
                else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && keysCapture < 0)
                {
                    // (A click or tap on a row: chosen and used.)
                    int ww = 1, wh = 1;
                    SDL_GetWindowSize(window, &ww, &wh);
                    const int row = keysMenu.ItemAt(e.button.x * canvas.w / std::max(1, ww), e.button.y * canvas.h / std::max(1, wh));
                    if (row >= 0) { keysMenu.selected = row; keysActivate(row); }
                    else if (row == -2) keysOpen = false;
                }
                else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED || e.type == SDL_JOYDEVICEADDED ||
                         e.type == SDL_JOYDEVICEREMOVED || e.type == SDL_WINDOWEVENT)
                    goto normal; // (controllers coming and going are still followed)
                any = SDL_PollEvent(&e);
                continue;
            }
        normal:
            switch (e.type)
            {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameController* p = SDL_GameControllerOpen(e.cdevice.which))
                {
                    pads.push_back(p);
                    const char* name = SDL_GameControllerName(p);
                    fprintf(stderr, "[launcher] controller: %s\n", name ? name : "?");
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                for (size_t i = 0; i < pads.size(); i++)
                    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which)
                    {
                        SDL_GameControllerClose(pads[i]);
                        pads.erase(pads.begin() + i);
                        break;
                    }
                break;
            case SDL_JOYDEVICEREMOVED:
                for (size_t i = 0; i < joys.size(); i++)
                    if (SDL_JoystickInstanceID(joys[i]) == e.jdevice.which)
                    {
                        SDL_JoystickClose(joys[i]);
                        joys.erase(joys.begin() + i);
                        break;
                    }
                break;
            case SDL_JOYDEVICEADDED:
                // Any other gamepad (SDL has no layout for it), as the games take it (main.cpp, Pads).
                if (!SDL_IsGameController(e.jdevice.which))
                {
                    SDL_Joystick* j = SDL_JoystickOpen(e.jdevice.which);
                    if (j && SDL_JoystickNumButtons(j) >= 2)
                    {
                        joys.push_back(j);
                        const char* name = SDL_JoystickName(j);
                        fprintf(stderr, "[launcher] joystick without a known layout: %s (%d buttons, %d axes, %d hats)\n",
                            name ? name : "?", SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j));
                    }
                    else if (j) SDL_JoystickClose(j);
                }
                break;
            case SDL_KEYDOWN:
            {
                int act = -1;
                for (int a = 0; a < Controls::COUNT; a++)
                    if (ctl.KeyCustom(a) && ctl.key[a] == e.key.keysym.scancode) act = a;
                if (act >= 0)
                {
                    if (act == Controls::L || act == Controls::R) { if (!guide) setTab(tab + (act == Controls::R ? 1 : -1), "a key"); }
                    else if (act == Controls::A || act == Controls::START) { if (!e.key.repeat) choose(); }
                    else if (act == Controls::B || act == Controls::MENU) { if (guide) { if (!page.Back()) guide = false; } else quit = true; }
                    else if (act == Controls::Y) toggleGuide();
                    else if (act >= Controls::UP && act <= Controls::RIGHT)
                        move(act == Controls::LEFT ? 1 : act == Controls::RIGHT ? 2 : act == Controls::UP ? 3 : 4);
                    break;
                }
                switch (e.key.keysym.sym)
                {
                case SDLK_LEFT: case SDLK_a: move(1); break;
                case SDLK_RIGHT: case SDLK_d: move(2); break;
                case SDLK_UP: case SDLK_w: move(3); break;
                case SDLK_DOWN: case SDLK_s: move(4); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (!e.key.repeat) choose(); break;
                case SDLK_ESCAPE: case SDLK_AC_BACK: if (guide) { if (!page.Back()) guide = false; } else quit = true; break;
                case SDLK_h: case SDLK_F1: toggleGuide(); break;
                case SDLK_o: OpenFolder(fo.roms); break;
                case SDLK_q: case SDLK_PAGEUP: if (!guide) setTab(tab - 1, "a key"); break;
                case SDLK_e: case SDLK_PAGEDOWN: case SDLK_TAB: if (!guide) setTab(tab + 1, "a key"); break;
                case SDLK_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    SetConfigFullscreen(fo.cfg, fullscreen);
                    if (guide) page.Open(fo.cfg, true); // (the page shows the file as it now is)
                    break;
                default: break;
                }
                break;
            }
            case SDL_CONTROLLERAXISMOTION:
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_JOYBUTTONDOWN:
            {
                // A controller's button (Controls::IN_*): a trigger counts once pushed past half
                // way; a joystick SDL has no layout for goes by Controls::JoyButtonInput (a known
                // controller's own joystick events are left to its controller events).
                int in = -1;
                if (e.type == SDL_CONTROLLERAXISMOTION)
                {
                    const int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? 1 : -1;
                    if (t < 0) break;
                    const float v = e.caxis.value / 32767.0f;
                    if (v > 0.5f && trig[t] <= 0.5f) in = t ? Controls::IN_RT : Controls::IN_LT;
                    trig[t] = v;
                }
                else if (e.type == SDL_JOYBUTTONDOWN)
                {
                    bool generic = false;
                    for (SDL_Joystick* j : joys) generic |= SDL_JoystickInstanceID(j) == e.jbutton.which;
                    if (!generic) break;
                    in = Controls::JoyButtonInput(e.jbutton.button);
                }
                else in = e.cbutton.button;
                if (in < 0) break;
                // The buttons changed in CONTROLS do their actions; otherwise A (or START) plays, B
                // (or BACK) goes back and Y opens the settings.
                const int act = ctl.PadCustom() ? ctl.ActionOfPad(in, ctl.byLabel)
                              : in == Controls::IN_A || in == Controls::IN_START ? Controls::A
                              : in == Controls::IN_B || in == Controls::IN_BACK ? Controls::B
                              : in == Controls::IN_Y ? Controls::Y
                              : in == Controls::IN_LB ? Controls::L : in == Controls::IN_RB ? Controls::R : -1;
                if (act == Controls::L || act == Controls::R)
                {
                    char why[64];
                    snprintf(why, sizeof why, "a controller's input %d, event %u", in, (unsigned)e.type);
                    if (!guide) setTab(tab + (act == Controls::R ? 1 : -1), why);
                }
                else if (act == Controls::A || act == Controls::START) choose();
                else if (act == Controls::B || act == Controls::SELECT) { if (guide) { if (!page.Back()) guide = false; } else quit = true; }
                else if (act == Controls::Y) toggleGuide();
                break;
            }
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            {
                // (Window points to the canvas's pixels, which are more on a high-DPI screen.)
                int ww = 1, wh = 1;
                SDL_GetWindowSize(window, &ww, &wh);
                const int mx = (e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x) * canvas.w / std::max(1, ww);
                const int my = (e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y) * canvas.h / std::max(1, wh);
                const int s = std::max(1, canvas.h / 240);
                // The buttons in the title band (touch screens): SETTINGS at the right, and in the
                // settings BACK at the left; on Android FOLDER at the left of the list.
                if (e.type == SDL_MOUSEBUTTONUP && my < 22 * s && !dragging)
                {
                    if (guide && mx < canvas.w / 4) { if (!page.Back()) guide = false; }
                    else if (!guide && mx > canvas.w * 3 / 4) toggleGuide();
#ifdef __ANDROID__
                    else if (!guide && mx < canvas.w / 4) JavaVoid("chooseFolder");
#elif defined(SSR_IOS)
                    else if (!guide && mx < canvas.w / 4) OpenFolder(fo.roms);
#endif
                    break;
                }
                if (guide)
                {
                    // The settings: a finger dragged scrolls; a tap chooses an option, a second
                    // tap on it changes it.
                    if (e.type == SDL_MOUSEBUTTONDOWN)
                    {
                        downY = my;
                        downScroll = page.scroll;
                        dragging = false;
                        pressed = true;
                    }
                    else if (e.type == SDL_MOUSEMOTION && pressed)
                    {
                        if (std::abs(my - downY) > page.lineH / 2) dragging = true;
                        if (dragging)
                        {
                            page.follow = false;
                            page.scroll = downScroll - (my - downY) / std::max(1, page.lineH);
                        }
                    }
                    else if (e.type == SDL_MOUSEBUTTONUP)
                    {
                        if (pressed && !dragging)
                        {
                            const int item = page.ItemAt(my);
                            if (item >= 0 && page.items[item].key)
                            {
                                if (item == page.selected) change(1);
                                else page.selected = item;
                                page.follow = false;
                            }
                        }
                        pressed = dragging = false;
                    }
                    break;
                }
                // A tab: shown.
                if (e.type == SDL_MOUSEBUTTONUP && !dragging)
                {
                    const int t = Launcher::TabAt(canvas, mx, my);
                    if (t >= 0)
                    {
                        if (t != tab) setTab(t, "a tap");
                        pressed = false;
                        break;
                    }
                }
                // A finger (or the mouse, held) dragged up or down scrolls the list; it chooses
                // nothing and plays nothing.
                const bool leftDown = e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT;
                if (leftDown && my >= 22 * s)
                {
                    downY = my;
                    downScroll = scroll;
                    pressed = true;
                    dragging = false;
                }
                else if (e.type == SDL_MOUSEMOTION && pressed)
                {
                    if (!dragging && std::abs(my - downY) > 8 * s) dragging = true;
                    if (dragging)
                    {
                        follow = false;
                        tapPlay = -1;
                        scroll = std::max(0, downScroll - (my - downY));
                        break;
                    }
                }
                if (e.type == SDL_MOUSEBUTTONUP && dragging)
                {
                    dragging = pressed = false;
                    break;
                }
                const int i = Launcher::TileAt(view(), scroll, mx, my);
                if (e.type == SDL_MOUSEBUTTONUP)
                {
                    // A tap on the chosen game plays it when the finger lifts (not as it lands:
                    // the finger would still be on the screen when the game starts, and touch it).
                    const bool play = tapPlay >= 0 && i == tapPlay && e.button.which == SDL_TOUCH_MOUSEID;
                    tapPlay = -1;
                    dragging = pressed = false;
                    if (play) choose();
                    break;
                }
                // A tap on another game chooses that one; a double click plays.
                const bool touch = e.type == SDL_MOUSEBUTTONDOWN && e.button.which == SDL_TOUCH_MOUSEID;
                if (touch && i >= 0 && i == selected)
                {
                    tapPlay = i;
                    break;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && i >= 0 && e.button.clicks >= 2)
                {
                    choose();
                    break;
                }
                if (i >= 0 && (i != selected || onSettings) && (e.type == SDL_MOUSEBUTTONDOWN || (e.motion.xrel || e.motion.yrel)))
                {
                    selected = i;
                    onSettings = onTabs = false;
                    note.clear();
                    // (A tap: the list follows it, unless the finger goes on to drag. A mouse
                    // passing over a tile leaves the list where it is.)
                    if (e.type == SDL_MOUSEBUTTONDOWN) follow = true;
                }
#ifdef __ANDROID__
                // (Asking for the ROM folder: a tap anywhere.)
                if (e.type == SDL_MOUSEBUTTONDOWN && i < 0 && my >= 22 * s && (needFolder || games.empty())) choose();
#endif
                break;
            }
            case SDL_MOUSEWHEEL:
                move(e.wheel.y > 0 ? 3 : 4);
                break;
            default:
                break;
            }
            any = SDL_PollEvent(&e);
        }
        if (quit) break;
        // Held directions on any controller (the D-pad or the left stick) repeat.
        int dir = 0;
        for (SDL_GameController* p : pads)
        {
            const int ax = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
            // (The buttons the directions have in CONTROLS: the D-pad unless changed.)
            auto down = [&](int a) {
                const int in = ctl.Pad(a, ctl.byLabel);
                if (in == Controls::IN_LT || in == Controls::IN_RT)
                    return SDL_GameControllerGetAxis(p, in == Controls::IN_LT ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
                return SDL_GameControllerGetButton(p, (SDL_GameControllerButton)in) != 0;
            };
            if (down(Controls::LEFT) || ax < -16000) dir = 1;
            else if (down(Controls::RIGHT) || ax > 16000) dir = 2;
            else if (down(Controls::UP) || ay < -16000) dir = 3;
            else if (down(Controls::DOWN) || ay > 16000) dir = 4;
            if (dir) break;
        }
        for (SDL_Joystick* j : joys)
        {
            if (dir) break;
            const Uint8 h = SDL_JoystickNumHats(j) > 0 ? SDL_JoystickGetHat(j, 0) : 0;
            const int ax = SDL_JoystickNumAxes(j) >= 2 ? SDL_JoystickGetAxis(j, 0) : 0, ay = SDL_JoystickNumAxes(j) >= 2 ? SDL_JoystickGetAxis(j, 1) : 0;
            if ((h & SDL_HAT_LEFT) || ax < -16000) dir = 1;
            else if ((h & SDL_HAT_RIGHT) || ax > 16000) dir = 2;
            else if ((h & SDL_HAT_UP) || ay < -16000) dir = 3;
            else if ((h & SDL_HAT_DOWN) || ay > 16000) dir = 4;
        }
        held = dir && dir == heldDir ? held + 1 : dir ? 1 : 0;
        heldDir = dir;
        if ((held == 1 || (held > 8 && held % 3 == 0)) && !keysOpen) move(dir);
        // (KEYS AND BUTTONS: the capture's 5 seconds.)
        if (keysOpen && keysCapture >= 0 && SDL_GetTicks() > keysUntil)
        {
            keysCapture = -1;
            keysBuild();
        }

#ifdef __ANDROID__
        // Back from Android's folder picker (or its "All files access" page): the folder it
        // gave, and the games in it at once.
        const bool pickingNow = JavaBool("picking");
        if ((wasPicking && !pickingNow) || (needFolder && SDL_GetTicks() - lastFolderCheck > 1000))
        {
            lastFolderCheck = SDL_GetTicks();
            SetRomFolder(fo);
            needFolder = fo.roms.empty() || !JavaBool("canUseFiles");
            if (!needFolder)
            {
                MakeDir(fo.saves);
                Launcher::LoadLook(fo.cfg, fo.art, theme, background);
                lastScan = 0;
                fprintf(stderr, "[launcher] ROM folder %s\n", fo.roms.c_str());
            }
        }
        wasPicking = pickingNow;
#endif
        // ROMs put in the folder (or taken out) show up by themselves.
        if (SDL_GetTicks() - lastScan > 1500)
        {
            lastScan = SDL_GetTicks();
            const std::string current = selected < 0 || selected >= (int)games.size() ? std::string() : games[selected].rom;
            games = FindGames(fo, artCache);
            selected = -1;
            for (size_t i = 0; i < games.size(); i++)
                if (games[i].rom == current) selected = (int)i;
            if (selected < 0 || view().SpotOf(selected) < 0) selected = Launcher::FirstIn(view());
        }

        int ow = 1280, oh = 720;
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
        if (ow != canvas.w || oh != canvas.h || !texture)
        {
            canvas.Resize(ow, oh);
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
        }
        // A controller's buttons named as it has them (PlayStation's CROSS, TRIANGLE, ...) and as
        // CONTROLS has them, while one is connected.
        const bool padIn = !pads.empty() || !joys.empty();
        auto btn = [&](int a) {
            const int def = a == Controls::A ? Controls::IN_A : a == Controls::B ? Controls::IN_B
                          : a == Controls::L ? Controls::IN_LB : a == Controls::R ? Controls::IN_RB : Controls::IN_Y;
            return std::string(Controls::InputName(ctl.PadCustom() ? ctl.Pad(a, ctl.byLabel) : def,
                Controls::StyleOf(pads.empty() ? nullptr : pads[0])));
        };
#ifdef SSR_IOS
        const std::string hint = games.empty() ? std::string("FOLDER: YOUR ROMS FOLDER IN THE FILES APP")
                               : padIn ? "TAP A GAME TWICE OR PRESS " + btn(Controls::A) + " TO PLAY    " + btn(Controls::Y) + ": SETTINGS"
                                       : std::string("TAP A GAME TWICE TO PLAY    SETTINGS: AT THE TOP");
        const std::string pageHint = padIn ? "UP / DOWN: CHOOSE    " + btn(Controls::A) + ": CHANGE    " + btn(Controls::B) + ": BACK"
                                           : std::string("TAP AN OPTION THEN TAP AGAIN TO CHANGE    DRAG TO SCROLL");
#elif defined(__ANDROID__)
        const std::string hint = games.empty() ? "TAP OR PRESS " + (padIn ? btn(Controls::A) : std::string("A")) + " TO CHOOSE YOUR ROMS FOLDER    BACK: QUIT"
                               : padIn ? "TAP A GAME TWICE OR PRESS " + btn(Controls::A) + " TO PLAY    " + btn(Controls::L) + " / " + btn(Controls::R) + ": TABS    BACK: QUIT"
                                       : std::string("TAP A GAME TWICE TO PLAY    SETTINGS: AT THE TOP    BACK: QUIT");
        const std::string pageHint = padIn ? "UP / DOWN: CHOOSE    " + btn(Controls::A) + ": CHANGE    " + btn(Controls::B) + ": BACK"
                                           : std::string("TAP AN OPTION THEN TAP AGAIN TO CHANGE    DRAG TO SCROLL    BACK: DONE");
#else
        const std::string hint = games.empty() ? std::string("O: OPEN THE ROMS FOLDER    H: SETTINGS    ESC: QUIT")
                               : padIn ? btn(Controls::A) + ": PLAY    " + btn(Controls::L) + " / " + btn(Controls::R) + ": TABS    " + btn(Controls::Y) + ": SETTINGS    " + btn(Controls::B) + ": QUIT"
                                       : std::string("ENTER: PLAY    Q / E: TABS    H: SETTINGS    O: ROMS    ESC: QUIT");
        const std::string pageHint = page.group < 0 ? (padIn ? "UP / DOWN: CHOOSE    " + btn(Controls::A) + ": OPEN    " + btn(Controls::B) + ": BACK"
                                                             : std::string("UP / DOWN: CHOOSE    ENTER: OPEN    ESC: BACK"))
                                   : padIn ? "UP / DOWN: CHOOSE    LEFT / RIGHT / " + btn(Controls::A) + ": CHANGE    " + btn(Controls::B) + ": BACK"
                                           : std::string("UP / DOWN: CHOOSE    LEFT / RIGHT / ENTER: CHANGE    ESC: BACK");
#endif
        if (guide)
        {
            raPoll();
            for (Launcher::Setting& it : page.items)
                if (it.key && !strcmp(it.key, "!ra_login") && !it.values.empty()) it.values[0].second = raAccountShown();
            page.Draw(canvas, pageHint, theme);
            if (raOpen)
                raMenu.Draw(canvas, "RETROACHIEVEMENTS: LOG IN",
                            raField == 0 ? "TYPE YOUR USER NAME, THEN ENTER" : raField == 1 ? "TYPE YOUR PASSWORD, THEN ENTER" : "ENTER: LOG IN   ESC: CANCEL",
                            raNote);
            if (keysOpen)
            {
                std::string kh = keysCapture >= 0 ? (keysKeyboard ? "ESC: LEAVE IT AS IT WAS" : "OR WAIT 5 SECONDS: AS IT WAS")
                               : padIn ? btn(Controls::A) + ": CHANGE   " + btn(Controls::B) + ": BACK" : std::string("ENTER: CHANGE   ESC: BACK");
                if (keysCapture < 0 && keysMenu.selected == 0) kh = "CHANGE: KEYBOARD OR CONTROLLER";
                keysMenu.Draw(canvas, "KEYS AND BUTTONS", kh);
            }
        }
        else Launcher::Draw(canvas, games, tab, onSettings || onTabs ? -1 : selected, scroll, note, hint, theme, follow, onTabs);
        {
            // The title band's buttons (for touch screens and the mouse): SETTINGS, or BACK in
            // the settings; on Android FOLDER, to choose the ROM folder again.
            const int s = std::max(1, canvas.h / 240);
            // (The chosen one, by the D-pad: framed as a game is.)
            auto button = [&](int x, const char* text, bool on) {
                if (on) canvas.Fill(x - 3 * s, 4 * s, Canvas::TextWidth(text, s) + 5 * s, 13 * s, theme.frame);
                canvas.Text(x, 7 * s, text, s, on ? theme.title : theme.hint);
            };
            if (guide) button(8 * s, "< BACK", onBack);
            else
            {
                button(canvas.w - 8 * s - Canvas::TextWidth("SETTINGS", s), "SETTINGS", onSettings);
#ifdef SSR_PHONE
                canvas.Text(8 * s, 7 * s, "FOLDER", s, theme.hint);
#endif
            }
        }
        if (games.empty() && !guide)
        {
            const int s = std::max(1, canvas.h / 240);
#ifdef __ANDROID__
            const bool asking = needFolder;
            const char* lines[] = { asking ? "CHOOSE YOUR ROMS FOLDER" : "NO GAMES FOUND", "",
                                    asking ? "TAP ANYWHERE: ALLOW ACCESS TO FILES, THEN PICK THE FOLDER" : "NO .NDS FILES IN",
                                    asking ? "WITH YOUR .NDS FILES. SAVES GO IN A SAVES FOLDER INSIDE IT." : fo.roms.c_str(),
                                    "",
                                    "IT PLAYS POKEMON BLACK / WHITE / BLACK 2 / WHITE 2,",
                                    "SOULSILVER / HEARTGOLD / PLATINUM (ENGLISH OR FRENCH)" };
            const char* heading = lines[0];
#elif defined(SSR_IOS)
            const char* lines[] = { "NO GAMES FOUND", "",
                                    "PUT YOUR .NDS FILES IN THE FILES APP: ON MY IPHONE (OR IPAD)",
                                    "> POKEMON DS WIDESCREEN > ROMS. TAP FOLDER AT THE TOP TO GO THERE.",
                                    "THEY SHOW UP HERE BY THEMSELVES.", "",
                                    "IT PLAYS POKEMON BLACK / WHITE / BLACK 2 / WHITE 2,",
                                    "SOULSILVER / HEARTGOLD / PLATINUM (ENGLISH OR FRENCH)" };
            const char* heading = lines[0];
#else
            const char* lines[] = { "NO GAMES FOUND", "",
                                    "PUT YOUR ROMS IN THE ROMS FOLDER BESIDE THIS PROGRAM.", "THEY SHOW UP HERE BY THEMSELVES.", "",
                                    "IT PLAYS POKEMON BLACK / WHITE / BLACK 2 / WHITE 2,",
                                    "SOULSILVER / HEARTGOLD / PLATINUM (ENGLISH OR FRENCH)" };
            const char* heading = lines[0];
#endif
            int y = canvas.h / 3;
            for (const char* l : lines)
            {
                const int ls = l == heading ? s + 1 : s;
                int ts = ls;
                while (ts > 1 && Canvas::TextWidth(l, ts) > canvas.w - 8) ts--;
                canvas.Text((canvas.w - Canvas::TextWidth(l, ts)) / 2, y, l, ts, ls > s ? theme.title : theme.name);
                y += 12 * s;
            }
        }
        SDL_UpdateTexture(texture, nullptr, canvas.px.data(), canvas.w * 4);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        if (testPick && *testPick)
        {
            for (size_t i = 0; i < games.size(); i++)
                if (games[i].code == testPick) result = (int)i;
            fprintf(stderr, "[launcher] test: %s chosen\n", testPick);
            break;
        }
        if (testShot && *testShot)
        {
            if (SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(canvas.px.data(), canvas.w, canvas.h, 32, canvas.w * 4, SDL_PIXELFORMAT_ARGB8888))
            {
                SDL_SaveBMP(sf, testShot);
                SDL_FreeSurface(sf);
            }
            fprintf(stderr, "[launcher] test: the list in %s\n", testShot);
            result = -1;
            break;
        }
    }
    lastTab = tab;
    for (SDL_GameController* p : pads) SDL_GameControllerClose(p);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#ifndef SSR_PHONE
    // (Not on a phone: SDL stays up for the game, which opens its own window in the same
    // activity (Android) or app (iOS).)
    SDL_Quit();
#endif
    return result;
}
}

bool Start(int& argc, char**& argv)
{
    // "--game IREO" (the desktop shortcuts, shortcuts.h): that game at once, without the list.
    // Other arguments (a ROM given by hand, the test harness): the program runs as it always has.
    std::string direct;
    if (argc == 3 && !strcmp(argv[1], "--game"))
    {
        direct = argv[2];
        for (char& c : direct) c = (char)toupper((unsigned char)c);
    }
    else if (argc >= 2) return true;
#ifdef _WIN32
    Shortcuts::SetAppId(direct);
#endif
    Folders fo;
#ifdef __ANDROID__
    // The app's own folder on the shared storage (Android/data/io.github.mamesj.pokemonds/
    // files: settings, game.log, the function tables), which a PC over USB can open; the
    // ROMs and saves in the folder the player chooses (GameActivity).
    {
        const char* ext = SDL_AndroidGetExternalStoragePath();
        const char* own = SDL_AndroidGetInternalStoragePath();
        fo.dir = ext ? ext : own ? own : ".";
    }
    fo.funcs = fo.dir + "/data/funcs";
    fo.cfg = fo.dir + "/settings.cfg";
    MakeDir(fo.dir);
    MakeDir(fo.dir + "/data");
    MakeDir(fo.funcs);
    ExtractFuncs(fo);
    SetRomFolder(fo);
    if (!fo.roms.empty()) MakeDir(fo.saves);
#else
    fo.dir = ProgramDir();
    fo.funcs = fo.dir + "/data/funcs";
#if defined(SSR_IOS)
    // iOS: the function tables in the app (read-only); the ROMs, saves, settings and game.log in
    // its Documents folder, which the Files app shows as On My iPhone > Pokémon Fulldex
    // (Info.plist: UIFileSharingEnabled), so ROMs can be put there and logs taken out.
    {
        const char* home = getenv("HOME");
        fo.dir = std::string(home ? home : ".") + "/Documents";
        MakeDir(fo.dir);
    }
#elif defined(__APPLE__)
    // The Mac app: the program's folder is inside the app (Contents/Resources, with the function
    // tables), which macOS may keep read-only, so the ROMs, saves and settings are in the user's
    // Application Support folder (O in the list opens its roms folder in the Finder).
    if (fo.dir.find(".app/Contents/") != std::string::npos)
    {
        const char* home = getenv("HOME");
        // (The program's name before it was Pokémon Fulldex: kept, so the ROMs, saves and
        // settings already there stay where they are.)
        fo.dir = std::string(home ? home : ".") + "/Library/Application Support/Pokemon DS Widescreen";
        for (size_t i = 1; i <= fo.dir.size(); i++) // (each folder on the way, as mkdir -p)
            if (i == fo.dir.size() || fo.dir[i] == '/') MakeDir(fo.dir.substr(0, i));
    }
#elif !defined(_WIN32)
    // From an AppImage (the Linux download for Gear Lever and the like): the program's folder is
    // the AppImage's own, read-only, so the ROMs, saves and settings are in the user's data
    // folder (~/.local/share/pokemon-ds-wide, O in the list opens its roms folder); the function
    // tables stay in the AppImage.
    if (const char* appImage = getenv("APPIMAGE"); appImage && *appImage)
    {
        const char* xdg = getenv("XDG_DATA_HOME");
        const char* home = getenv("HOME");
        const std::string base = xdg && *xdg ? xdg : std::string(home ? home : ".") + "/.local/share";
        fo.dir = base + "/pokemon-ds-wide";
        for (size_t i = 1; i <= fo.dir.size(); i++) // (each folder on the way, as mkdir -p)
            if (i == fo.dir.size() || fo.dir[i] == '/') MakeDir(fo.dir.substr(0, i));
    }
#endif
    fo.roms = fo.dir + "/roms";
    fo.saves = fo.dir + "/saves";
    fo.cfg = fo.dir + "/settings.cfg";
    fo.art = fo.dir;
    MakeDir(fo.roms);
    MakeDir(fo.saves);
#endif

    // The log goes to game.log beside the program (the last game's is kept as game-previous.log:
    // QUIT starts the program again for the list, whose few lines would otherwise replace it).
    const std::string logPath = fo.dir + "/game.log";
    struct stat ls;
    if (stat(logPath.c_str(), &ls) == 0 && ls.st_size > 1024)
    {
        const std::string prev = fo.dir + "/game-previous.log";
        remove(prev.c_str());
        rename(logPath.c_str(), prev.c_str());
    }
    if (freopen(logPath.c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IOLBF, 0);
    fprintf(stderr, "[launcher] folder %s\n", fo.dir.c_str());

    std::map<std::string, std::vector<uint32_t>> artCache;
    std::vector<Launcher::Game> games = FindGames(fo, artCache);
    // The list starts on the game played last.
    const std::string lastPath = fo.dir + "/last-game.txt";
    int chosen = 0;
    if (FILE* f = fopen(lastPath.c_str(), "r"))
    {
        char line[2048] = {};
        if (fgets(line, sizeof line, f))
        {
            std::string last = line;
            while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) last.pop_back();
            for (size_t i = 0; i < games.size(); i++)
                if (games[i].rom == last) chosen = (int)i;
        }
        fclose(f);
    }
    int winW = 1280, winH = 720;
    bool fullscreen = ConfigFullscreen(fo.cfg, winW, winH);
    int directGame = -1;
    for (size_t i = 0; i < games.size() && !direct.empty(); i++)
        if (games[i].code == direct && Playable(fo, games[i]))
        {
            directGame = (int)i;
            break;
        }
    if (!direct.empty() && directGame < 0) fprintf(stderr, "[launcher] --game %s: not in the list\n", direct.c_str());
    for (;;)
    {
        chosen = directGame >= 0 ? directGame : PickGame(fo, games, chosen, fullscreen, winW, winH);
        if (chosen < 0) return false;
#ifdef SSR_GBA
        // A GBA game: played here (Pokemon GBA Widescreen, built in), then the list again.
        if (games[chosen].platform == Launcher::GBA)
        {
            const std::string rom = games[chosen].rom;
            if (FILE* f = fopen(lastPath.c_str(), "w"))
            {
                fprintf(f, "%s\n", rom.c_str());
                fclose(f);
            }
            GbaWide::Options o;
            o.roms = rom.substr(0, rom.find_last_of("/\\"));
            o.saves = games[chosen].folder;
            o.data = fo.dir;
            o.fullscreen = ConfigFullscreen(fo.cfg, o.w, o.h);
            fprintf(stderr, "[launcher] GBA: %s\n", rom.c_str());
            GbaWide::Play(rom, o);
            directGame = -1;
            games = FindGames(fo, artCache);
            chosen = 0;
            for (size_t i = 0; i < games.size(); i++)
                if (games[i].rom == rom) chosen = (int)i;
            continue;
        }
#endif
        break;
    }
    g_canChange = true;
    const Launcher::Game& game = games[chosen];
    if (const Launcher::KnownGame* k = Launcher::Known(game.code)) g_name = k->name;
    if (FILE* f = fopen(lastPath.c_str(), "w"))
    {
        fprintf(f, "%s\n", game.rom.c_str());
        fclose(f);
    }
    fprintf(stderr, "[launcher] %s (game code %s), save %s/%s\n", game.rom.c_str(), game.code.c_str(), fo.saves.c_str(),
        game.saveName.c_str());
    g_args = { argc > 0 && argv[0] ? argv[0] : "pokemon-ds-wide", game.rom, "--save", fo.saves + "/" + game.saveName,
               "--config", fo.cfg };
#ifndef SSR_IOS
    // (melonDS's JIT; iOS lets no app make code as it runs, so there the interpreter plays.)
    g_args.push_back("--jit");
#endif
    // SoulSilver and HeartGold take their layouts from the program's own tables and must not be
    // given a function table (one switches to the Black games' layouts); the others take theirs.
    if (game.code != "IPGE" && game.code != "IPKE" && game.code != "IPGF" && game.code != "IPKF")
    {
        g_args.push_back("--funcs");
        g_args.push_back(fo.funcs + "/" + game.code + ".txt");
    }
    for (auto& a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
    argc = (int)g_args.size();
    argv = g_argv.data();
    return true;
}

bool CanChangeGame() { return g_canChange; }
const char* GameName() { return g_name.c_str(); }
void ChangeGame() { g_change = true; }

void Finish()
{
    if (!g_change) return;
    fflush(stderr);
    // The program again, without arguments: the list.
#if defined(__ANDROID__)
    JavaVoid("restart"); // (GameActivity: a new process, which opens on the list)
#elif defined(_WIN32)
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    if (n == 0 || n >= 32768) return;
    // (game.log let go first: the new program keeps it as game-previous.log, which Windows does
    // not allow while this one still has it open, and the game's log was lost.)
    fflush(stderr);
    freopen("NUL", "w", stderr);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(self, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#elif defined(__APPLE__)
    char self[4096];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) return;
    execl(self, self, (char*)nullptr);
#else
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return;
    self[n] = 0;
    execl(self, self, (char*)nullptr);
#endif
}
}
#endif
