// The PC all-games program's game list. See pc_launcher.h.
#ifdef SSR_PC_LAUNCHER
#include "pc_launcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
#if !defined(__ANDROID__) && !defined(__SWITCH__)
#include "shortcuts.h"
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
    for (const char* code : { "IREO", "IRDO", "IRBO", "IRAO", "CPUE" })
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
    if (!g.known) return false;
    if (g.code == "IPGE" || g.code == "IPKE") return rc_num_chunks > 0;
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
    Launcher::Sort(games);
    return games;
}

void OpenFolder(const std::string& path)
{
    std::string url = "file://";
    if (path.empty() || path[0] != '/') url += "/"; // (C:/... on Windows)
    for (char c : path) url += c == '\\' ? '/' : c;
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
    SDL_Window* window = SDL_CreateWindow("Pokemon DS Widescreen", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH,
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
    int first = 0, result = -1, cols = 3;
    std::string note;
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
    auto toggleGuide = [&]() {
        guide = !guide;
        if (guide) page.Open(fo.cfg);
    };
    auto change = [&](int d) {
        const char* key = page.Change(d);
#if !defined(__ANDROID__) && !defined(__SWITCH__)
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
        if (guide)
        {
            change(1);
            return;
        }
#ifdef __ANDROID__
        if (needFolder || games.empty())
        {
            JavaVoid("chooseFolder");
            return;
        }
#endif
        if (games.empty()) return;
        if (Playable(fo, games[selected]))
        {
            result = selected;
            quit = true;
        }
        else
            note = games[selected].known ? "THIS DOWNLOAD DOES NOT HAVE THIS GAME" : "THIS GAME IS NOT ONE THAT THIS PLAYS";
    };
    auto move = [&](int dir) { // 1 left, 2 right, 3 up, 4 down
        if (guide)
        {
            if (dir == 1 || dir == 2) change(dir == 1 ? -1 : 1);
            else page.Move(dir == 3 ? -1 : 1);
            return;
        }
        if (games.empty()) return;
        int to = selected;
        if (dir == 1) to = selected - 1;
        if (dir == 2) to = selected + 1;
        if (dir == 3) to = selected - cols;
        if (dir == 4) to = std::min(selected + cols, (int)games.size() - 1);
        if (to != selected && to >= 0 && to < (int)games.size())
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
            switch (e.type)
            {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameController* p = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(p);
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym)
                {
                case SDLK_LEFT: case SDLK_a: move(1); break;
                case SDLK_RIGHT: case SDLK_d: move(2); break;
                case SDLK_UP: case SDLK_w: move(3); break;
                case SDLK_DOWN: case SDLK_s: move(4); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (!e.key.repeat) choose(); break;
                case SDLK_ESCAPE: case SDLK_AC_BACK: if (guide) guide = false; else quit = true; break;
                case SDLK_h: case SDLK_F1: toggleGuide(); break;
                case SDLK_o: OpenFolder(fo.roms); break;
                case SDLK_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    SetConfigFullscreen(fo.cfg, fullscreen);
                    if (guide) page.Open(fo.cfg, true); // (the page shows the file as it now is)
                    break;
                default: break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button)
                {
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: choose(); break;
                case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: if (guide) guide = false; else quit = true; break;
                case SDL_CONTROLLER_BUTTON_Y: toggleGuide(); break;
                default: break;
                }
                break;
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
                    if (guide && mx < canvas.w / 4) guide = false;
                    else if (!guide && mx > canvas.w * 3 / 4) toggleGuide();
#ifdef __ANDROID__
                    else if (!guide && mx < canvas.w / 4) JavaVoid("chooseFolder");
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
                const int i = Launcher::TileAt(canvas, (int)games.size(), first, mx, my);
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
                if (i >= 0 && i != selected && (e.type == SDL_MOUSEBUTTONDOWN || (e.motion.xrel || e.motion.yrel)))
                {
                    selected = i;
                    note.clear();
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
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) dir = 1;
            else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) dir = 2;
            else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) dir = 3;
            else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) dir = 4;
            if (dir) break;
        }
        held = dir && dir == heldDir ? held + 1 : dir ? 1 : 0;
        heldDir = dir;
        if (held == 1 || (held > 8 && held % 3 == 0)) move(dir);

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
            const std::string current = games.empty() ? std::string() : games[selected].rom;
            games = FindGames(fo, artCache);
            selected = 0;
            for (size_t i = 0; i < games.size(); i++)
                if (games[i].rom == current) selected = (int)i;
        }

        int ow = 1280, oh = 720;
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
        if (ow != canvas.w || oh != canvas.h || !texture)
        {
            canvas.Resize(ow, oh);
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
        }
#ifdef __ANDROID__
        const std::string hint = games.empty() ? "TAP OR PRESS A TO CHOOSE YOUR ROMS FOLDER    BACK: QUIT"
                                               : "TAP A GAME TWICE OR PRESS A TO PLAY    Y: SETTINGS    BACK: QUIT";
        const char* pageHint = "TAP AN OPTION THEN TAP AGAIN TO CHANGE    DRAG TO SCROLL    BACK: DONE";
#else
        const std::string hint = games.empty() ? "O: OPEN THE ROMS FOLDER    H: SETTINGS    ESC: QUIT"
                                               : "ENTER OR A: PLAY    H OR Y: SETTINGS    O: ROMS FOLDER    ESC: QUIT";
        const char* pageHint = "UP / DOWN: CHOOSE    LEFT / RIGHT / ENTER: CHANGE    ESC OR B: BACK";
#endif
        static const Launcher::Theme theme;
        if (guide) page.Draw(canvas, pageHint, theme);
        else cols = Launcher::Draw(canvas, games, selected, first, note, hint, theme);
        {
            // The title band's buttons (for touch screens and the mouse): SETTINGS, or BACK in
            // the settings; on Android FOLDER, to choose the ROM folder again.
            const int s = std::max(1, canvas.h / 240);
            if (guide) canvas.Text(8 * s, 7 * s, "< BACK", s, theme.hint);
            else
            {
                canvas.Text(canvas.w - 8 * s - Canvas::TextWidth("SETTINGS", s), 7 * s, "SETTINGS", s, theme.hint);
#ifdef __ANDROID__
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
                                    "IT PLAYS POKEMON BLACK / WHITE / BLACK 2 / WHITE 2 (ENGLISH)",
                                    "SOULSILVER / HEARTGOLD / PLATINUM (US)" };
            const char* heading = lines[0];
#else
            const char* lines[] = { "NO GAMES FOUND", "",
                                    "PUT YOUR ROMS IN THE ROMS FOLDER BESIDE THIS PROGRAM.", "THEY SHOW UP HERE BY THEMSELVES.", "",
                                    "IT PLAYS POKEMON BLACK / WHITE / BLACK 2 / WHITE 2 (ENGLISH)",
                                    "SOULSILVER / HEARTGOLD / PLATINUM (US)" };
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
    }
    for (SDL_GameController* p : pads) SDL_GameControllerClose(p);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#ifndef __ANDROID__
    // (Not on Android: SDL stays up for the game, which opens its own window in the same
    // activity.)
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
    fo.roms = fo.dir + "/roms";
    fo.saves = fo.dir + "/saves";
    fo.funcs = fo.dir + "/data/funcs";
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
    chosen = directGame >= 0 ? directGame : PickGame(fo, games, chosen, fullscreen, winW, winH);
    if (chosen < 0) return false;
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
               "--config", fo.cfg, "--jit" };
    // SoulSilver and HeartGold take their layouts from the program's own tables and must not be
    // given a function table (one switches to the Black games' layouts); the others take theirs.
    if (game.code != "IPGE" && game.code != "IPKE")
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
    // (No /proc on macOS: the program's own path, inside its .app, from dyld.)
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
