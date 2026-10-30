// Nintendo Switch (homebrew, libnx) parts of the frontend. See switch_platform.h.
#ifdef __SWITCH__
#include "switch_platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "testrun.h"
// (stb_image's code here, for the game list's pictures: only JPEG and PNG.)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "thirdparty/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION
#include "launcher.h"
#include "controls.h"
#ifdef SSR_GBA
#include <map>
#include "gba_games.h" // Pokemon GBA Widescreen, built in: its GBA games (CMakeLists.txt, SSR_GBA_DIR)
#endif
#ifdef SSR_ALL_GAMES
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "thirdparty/stb_image_write.h"
#include "switch_forwarder.h"
#endif
#ifdef SSR_PC_ART
// The game list's pictures built into the program (pc_art.cpp, CMakeLists.txt: SSR_PC_ART).
namespace PcLauncher { const unsigned char* BuiltInArt(const char* code, size_t& size); }
#endif

// How many chunks the program's SoulSilver tables have: none in a build for the Black games only.
extern const uint32_t rc_num_chunks;

namespace SwitchPlatform
{
namespace
{
std::vector<std::string> g_args;
std::vector<char*> g_argv;
// A test run from test.txt (testrun.h), and the .nro to start again for the next one.
TestRun::State g_test;
std::string g_self;

// Shows a message on the console's screen until + is pressed. Used only before SDL starts.
void ShowMessage(const std::string& text)
{
    fprintf(stderr, "%s\n", text.c_str());
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("\n  Pokemon DS widescreen\n\n%s\n\n  Press + to return to the Homebrew Menu.\n", text.c_str());
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

// The program's romfs (the Black games' function tables), mounted once.
bool RomfsReady()
{
    static const bool ready = R_SUCCEEDED(romfsInit());
    return ready;
}

// Whether this build plays a game: SoulSilver and HeartGold need the program's own tables, the
// Black games and Platinum a function table (the player's own funcs.txt beside the game, or the
// build's).
bool Playable(const Launcher::Game& g)
{
    if (g.platform == Launcher::GBA) return true; // (any GBA game plays, with widescreen or without)
    if (!g.known) return false;
    if (g.code == "IPGE" || g.code == "IPKE" || g.code == "IPGF" || g.code == "IPKF") return rc_num_chunks > 0;
    struct stat st;
    if (stat((g.folder + "/funcs.txt").c_str(), &st) == 0) return true;
    return RomfsReady() && stat(("romfs:/funcs/" + g.code + ".txt").c_str(), &st) == 0;
}

// The settings: settings.cfg in the program's folder (ssrecomp.cfg before; an old one is renamed).
std::string SettingsPath(const std::string& dir)
{
    const std::string cfgPath = dir + "/settings.cfg", oldCfg = dir + "/ssrecomp.cfg";
    struct stat cst;
    if (stat(cfgPath.c_str(), &cst) != 0 && stat(oldCfg.c_str(), &cst) == 0) rename(oldCfg.c_str(), cfgPath.c_str());
    return cfgPath;
}

#ifdef SSR_ALL_GAMES
// The game's picture as the HOME Menu's icon: a 256x256 JPEG (over the list's dark grey where
// the picture is see-through, as a DS icon is). Empty without a picture.
std::vector<uint8_t> IconJpeg(const Launcher::Game& g)
{
    const std::vector<uint32_t> px = Launcher::GamePicture(g, 256);
    if (px.empty()) return {};
    std::vector<uint8_t> rgb((size_t)256 * 256 * 3);
    for (size_t i = 0; i < px.size(); i++)
    {
        const uint32_t p = px[i], a = p >> 24;
        for (int c = 0; c < 3; c++)
        {
            const uint32_t v = (p >> (16 - 8 * c)) & 255;
            rgb[i * 3 + c] = (uint8_t)((v * a + 0x1c * (255 - a)) / 255);
        }
    }
    std::vector<uint8_t> jpeg;
    stbi_write_jpg_to_func([](void* ctx, void* data, int size) {
        auto* out = (std::vector<uint8_t>*)ctx;
        out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
    }, &jpeg, 256, 256, 3, rgb.data(), 92);
    return jpeg;
}

// HOME MENU ICONS (the settings page): one HOME Menu entry per game in the list, each starting
// this program with "--game CODE" (switch_forwarder.cpp). What came of it, for the page.
std::string InstallHomeIcons(const std::vector<Launcher::Game>& games)
{
    if (g_self.empty()) return "FAILED: NO PROGRAM PATH";
    std::vector<SwitchForwarder::Entry> entries;
    std::vector<std::string> done;
    for (const Launcher::Game& g : games)
    {
        if (!Playable(g) || std::find(done.begin(), done.end(), g.code) != done.end()) continue;
        done.push_back(g.code);
        SwitchForwarder::Entry e;
        const Launcher::KnownGame* k = Launcher::Known(g.code);
        e.name = k ? k->name : Launcher::TitleCase(g.name);
        e.args = "--game " + g.code;
        // The icon: the player's own picture in icons/ if there is one, else the box art built
        // into the program, else the list's picture (a one-game program's icon can be a placeholder).
        Launcher::Game pic = g;
        const std::string dir = g_self.substr(0, g_self.find_last_of('/'));
        struct stat is;
        const bool own = stat((dir + "/icons/" + g.code + ".jpg").c_str(), &is) == 0 || stat((dir + "/icons/" + g.code + ".png").c_str(), &is) == 0;
#ifdef SSR_PC_ART
        size_t size = 0;
        std::vector<uint32_t> art;
        if (!own)
            if (const unsigned char* data = PcLauncher::BuiltInArt(g.code.c_str(), size))
                if (Launcher::LoadArt(data, size, art)) pic.art = art;
#else
        (void)own;
#endif
        e.icon = IconJpeg(pic);
        if (e.icon.empty()) continue;
        // (For checking: the icons also written to a forwarder-debug folder beside the program, if there.)
        const std::string debugDir = g_self.substr(0, g_self.find_last_of('/')) + "/forwarder-debug";
        struct stat ds;
        if (stat(debugDir.c_str(), &ds) == 0)
            if (FILE* f = fopen((debugDir + "/" + g.code + ".jpg").c_str(), "wb"))
            {
                fwrite(e.icon.data(), 1, e.icon.size(), f);
                fclose(f);
            }
        entries.push_back(std::move(e));
    }
    if (entries.empty()) return "NO GAMES TO ADD";
    {
        const std::string debugDir = g_self.substr(0, g_self.find_last_of('/')) + "/forwarder-debug";
        struct stat ds;
        SwitchForwarder::SetDebugFolder(stat(debugDir.c_str(), &ds) == 0 ? debugDir : "");
    }
    // The forwarder's parts are in the romfs, mounted the first time something needs it: the list
    // mounts it only to look for a game's function table, which it did not need with only
    // SoulSilver and HeartGold, or games with their own funcs.txt (then: THIS BUILD HAS NO
    // FORWARDER).
    if (!RomfsReady()) fprintf(stderr, "[switch] the program's romfs could not be mounted\n");
    std::string error;
    const int made = SwitchForwarder::Install(g_self, entries, error);
    fprintf(stderr, "[switch] HOME Menu icons: %d of %d%s%s\n", made, (int)entries.size(), error.empty() ? "" : ", ", error.c_str());
    if (made == (int)entries.size()) return std::to_string(made) + " ON THE HOME MENU";
    if (made > 0) return std::to_string(made) + " OF " + std::to_string(entries.size()) + " (" + error + ")";
    return "FAILED: " + error;
}
#endif

// The game list (launcher.h) on the screen until a game is chosen: its index, or -1 for +.
// Shown with libnx's framebuffer, before the game's own window exists.
// autoFrames > 0: the list is shown that many frames and the selected game played (test runs:
// "arg --show-list" in test.txt, to time a game started after the list).
// cfgPath: the settings file, for the settings page (Y).
int PickGame(const std::vector<Launcher::Game>& games, int selected, const std::string& cfgPath, int autoFrames = 0)
{
    Framebuffer fb;
    if (R_FAILED(framebufferCreate(&fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2))) return selected;
    framebufferMakeLinear(&fb);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    Canvas canvas;
    canvas.Resize(1280, 720);
    // The touch screen (handheld): SETTINGS at the right of the title band, BACK at its left in
    // the settings, as on a PC or a phone (pc_launcher.cpp). A tap counts as the finger lifts.
    hidInitializeTouchScreen();
    bool touching = false;
    int touchX = 0, touchY = 0;
    // SETTINGS or the tabs chosen with the D-pad: up from the first row of games the tabs (left
    // and right change the tab), up again SETTINGS.
    bool onSettings = false, onTabs = false;
    // The tab shown (ALL first, the list opens on it), how far down it is scrolled; selected: a
    // game in that tab, or -1 when it has none (Launcher::MakeView, Step).
    int tab = 0, scroll = 0, held = 0, result = -1;
    auto view = [&]() { return Launcher::MakeView(canvas, games, tab); };
    bool guide = false; // the settings page (Launcher::SettingsPage)
    Launcher::SettingsPage page;
    // The buttons changed in KEYS AND BUTTONS (controls.h): the list follows them (A plays, Y the
    // settings, B leaves), and the settings page's CONTROLS changes them here too. A controller's
    // buttons by their labels, as main.cpp reads them on the Switch (Controls::IN_*).
    Controls::Bindings ctl;
    ctl.Load(cfgPath);
    auto mask = [](int in) -> u64 {
        static const u64 m[Controls::IN_COUNT] = { HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
            HidNpadButton_Minus, 0, HidNpadButton_Plus, HidNpadButton_StickL, HidNpadButton_StickR, HidNpadButton_L,
            HidNpadButton_R, HidNpadButton_Up, HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right,
            HidNpadButton_ZL, HidNpadButton_ZR };
        return in >= 0 && in < Controls::IN_COUNT ? m[in] : 0;
    };
    auto action = [&](int a) { return mask(ctl.Pad(a, true)); };
    bool keysOpen = false;
    int keysCapture = -1;
    u64 keysUntil = 0;
    Menu keysMenu;
    auto keysBuild = [&]() {
        keysMenu.items.clear();
        for (int a = 0; a < Controls::COUNT; a++)
            keysMenu.items.push_back({ Controls::ActionName(a), keysCapture == a ? "PRESS A BUTTON" : Controls::InputName(ctl.Pad(a, true)) });
        keysMenu.items.push_back({ "RESET ALL", "" });
        keysMenu.items.push_back({ "BACK", "" });
    };
    std::string note;
    int shown = 0;
#ifdef SSR_ALL_GAMES
    bool installIcons = false; // (HOME MENU ICONS chosen: done after INSTALLING is on the screen)
#endif
    // The list's look (GAME LIST on the settings page: COLOUR, and BACKGROUND, a picture in this
    // folder), read again when one of them changes.
    const std::string listDir = cfgPath.substr(0, cfgPath.find_last_of('/'));
    Launcher::Theme theme;
    Launcher::Background background;
    Launcher::LoadLook(cfgPath, listDir, theme, background);
    auto change = [&](int d) {
        const char* key = page.Change(d);
        if (key && !strncmp(key, "list_", 5)) Launcher::LoadLook(cfgPath, listDir, theme, background);
        if (key && !strcmp(key, "!keys"))
        {
            ctl.Load(cfgPath);
            keysOpen = true;
            keysCapture = -1;
            keysMenu.selected = 0;
            keysMenu.open = true;
            keysBuild();
            return;
        }
#ifdef SSR_ALL_GAMES
        if (key && !strcmp(key, "!forwarders") && !installIcons)
        {
            page.items[page.selected].values[0].second = "INSTALLING...";
            installIcons = true;
        }
#else
        (void)key;
#endif
    };
    while (appletMainLoop())
    {
#ifdef SSR_ALL_GAMES
        // (Set during the last pass, which then showed INSTALLING... on the screen.)
        if (installIcons)
        {
            page.items[page.selected].values[0].second = InstallHomeIcons(games);
            installIcons = false;
        }
#endif
        if (autoFrames > 0 && ++shown > autoFrames)
        {
            result = selected;
            break;
        }
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad), now = padGetButtons(&pad);
        int tapX = -1, tapY = -1;
        {
            HidTouchScreenState ts = {};
            if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0)
            {
                touching = true;
                touchX = (int)ts.touches[0].x;
                touchY = (int)ts.touches[0].y;
            }
            else if (touching)
            {
                touching = false;
                tapX = touchX;
                tapY = touchY;
            }
        }
        const int bandH = 22 * std::max(1, canvas.h / 240);
        const bool tapSettings = tapY >= 0 && tapY < bandH && tapX > canvas.w * 3 / 4;
        const bool tapBack = tapY >= 0 && tapY < bandH && tapX < canvas.w / 4;
        // The arrows (or a stick) move through the grid, and keep moving while held.
        const bool up = now & HidNpadButton_AnyUp, dn = now & HidNpadButton_AnyDown;
        const bool lf = now & HidNpadButton_AnyLeft, rt = now & HidNpadButton_AnyRight;
        held = (up || dn || lf || rt) ? held + 1 : 0;
        // KEYS AND BUTTONS: choose one with the D-pad and A, then press the button it should be
        // (5 seconds without one: as it was); B goes back to the settings page.
        if (guide && keysOpen)
        {
            if (keysCapture >= 0)
            {
                for (int in = 0; in < Controls::IN_COUNT; in++)
                    if (mask(in) && (down & mask(in)))
                    {
                        ctl.SetPad(keysCapture, in, true);
                        ctl.SaveTo(cfgPath);
                        page.Open(cfgPath, true);
                        keysCapture = -1;
                        break;
                    }
                if (keysCapture >= 0 && armGetSystemTick() > keysUntil) keysCapture = -1;
                keysBuild();
            }
            else
            {
                MenuInput nav;
                const bool step = held == 1 || (held > 20 && held % 6 == 0);
                nav.up = step && up;
                nav.down = step && dn;
                nav.accept = (down & HidNpadButton_A) != 0;
                if (down & HidNpadButton_B) keysOpen = false;
                int dir = 0;
                const int hit = keysMenu.Update(nav, dir);
                if (hit >= 0 && hit < Controls::COUNT)
                {
                    keysCapture = hit;
                    keysUntil = armGetSystemTick() + armNsToTicks(5000000000ULL);
                }
                else if (hit == Controls::COUNT)
                {
                    ctl.Reset();
                    ctl.SaveTo(cfgPath);
                    page.Open(cfgPath, true);
                }
                else if (hit == Controls::COUNT + 1) keysOpen = false;
                keysBuild();
            }
        }
        // Y: the settings page: up and down choose, left, right and A change, B or Y go back.
        else if (guide)
        {
            const bool step = held == 1 || (held > 20 && held % 6 == 0);
            if (step && (up || dn)) page.Move(up ? -1 : 1);
            if (step && (lf || rt)) change(lf ? -1 : 1);
            if (down & HidNpadButton_A) change(1);
            // (B: from a group back to the groups first; Y closes the page.)
            if (((down & HidNpadButton_B) || tapBack) && !page.Back()) guide = false;
            if (down & HidNpadButton_Y) guide = false;
            if (down & HidNpadButton_Plus) break;
        }
        else if ((down & (ctl.PadCustom() ? action(Controls::Y) : (u64)HidNpadButton_Y)) || tapSettings ||
                 (onSettings && (down & (ctl.PadCustom() ? action(Controls::A) : (u64)HidNpadButton_A))))
        {
            guide = true;
            onSettings = false;
            page.Open(cfgPath);
            held = 0;
        }
        else
        {
            // L and R (or the buttons CONTROLS gave them): the tabs, keeping the chosen game if
            // it is in the new one.
            const u64 tl = ctl.PadCustom() ? action(Controls::L) : (u64)HidNpadButton_L;
            const u64 tr = ctl.PadCustom() ? action(Controls::R) : (u64)HidNpadButton_R;
            auto setTab = [&](int by) {
                tab = (tab + by + Launcher::TABS) % Launcher::TABS;
                scroll = 0;
                const Launcher::View v = view();
                if (v.SpotOf(selected) < 0) selected = Launcher::FirstIn(v);
                if (Launcher::FirstIn(v) < 0) onTabs = true; // (no games: left and right go on)
                note.clear();
            };
            if (down & (tl | tr)) setTab((down & tr) ? 1 : -1);
            const bool step = held == 1 || (held > 20 && held % 6 == 0);
            const int dir = !step ? 0 : lf ? 1 : rt ? 2 : up ? 3 : dn ? 4 : 0;
            const bool aDown = (down & (ctl.PadCustom() ? action(Controls::A) : (u64)HidNpadButton_A)) != 0;
            const bool wasNav = onSettings || onTabs; // (A there is not a game's: it is not played as well)
            if (onSettings)
            {
                // (Down from SETTINGS: the tabs.)
                if (dir == 4)
                {
                    onSettings = false;
                    onTabs = true;
                }
            }
            else if (onTabs)
            {
                // Left and right: the tabs; down (or A): into the tab's games; up: SETTINGS.
                if (dir == 1 || dir == 2) setTab(dir == 2 ? 1 : -1);
                else if (dir == 3)
                {
                    onTabs = false;
                    onSettings = true;
                }
                else if ((dir == 4 || aDown) && Launcher::FirstIn(view()) >= 0)
                {
                    onTabs = false;
                    if (view().SpotOf(selected) < 0) selected = Launcher::FirstIn(view());
                }
            }
            else if (dir)
            {
                const int to = Launcher::Step(view(), selected, dir);
                if (to < 0 && dir == 3) onTabs = true; // (up from the first row)
                else if (to >= 0 && to != selected)
                {
                    selected = to;
                    note.clear();
                }
            }
            if (!wasNav && !onSettings && !onTabs && selected >= 0 && (down & (ctl.PadCustom() ? action(Controls::A) : (u64)HidNpadButton_A)))
            {
                if (Playable(games[selected]))
                {
                    result = selected;
                    break;
                }
                note = games[selected].comingSoon ? games[selected].name + ": COMING SOON"
                     : games[selected].known ? "THIS DOWNLOAD DOES NOT HAVE THIS GAME" : "THIS GAME IS NOT ONE THAT THIS PLAYS";
            }
            if (down & (HidNpadButton_Plus | (ctl.PadCustom() ? action(Controls::B) : (u64)HidNpadButton_B))) break;
        }
        // (The game list only when the settings page is not open: drawn over it, the page was
        // hidden while it took the buttons, and the list seemed frozen.)
        if (guide)
        {
            page.Draw(canvas, page.group < 0 ? "UP / DOWN: CHOOSE    A: OPEN    B: BACK"
                                             : "UP / DOWN: CHOOSE    LEFT / RIGHT / A: CHANGE    B: BACK", theme);
            if (keysOpen) keysMenu.Draw(canvas, "KEYS AND BUTTONS", keysCapture >= 0 ? "OR WAIT 5 SECONDS: AS IT WAS" : "A: CHANGE   B: BACK");
        }
        else Launcher::Draw(canvas, games, tab, onSettings || onTabs ? -1 : selected, scroll, note, std::string(), theme, true, onTabs);
        {
            // The title band's buttons, as pc_launcher.cpp draws them (the chosen one framed).
            const int s = std::max(1, canvas.h / 240);
            const Launcher::Theme& th = theme;
            auto button = [&](int x, const char* text, bool on) {
                if (on) canvas.Fill(x - 3 * s, 4 * s, Canvas::TextWidth(text, s) + 5 * s, 13 * s, th.frame);
                canvas.Text(x, 7 * s, text, s, on ? th.title : th.hint);
            };
            if (guide && !keysOpen) button(8 * s, "< BACK", false);
            else if (!guide) button(canvas.w - 8 * s - Canvas::TextWidth("SETTINGS", s), "SETTINGS", onSettings);
        }
        u32 stride;
        u32* out = (u32*)framebufferBegin(&fb, &stride);
        for (int y = 0; y < 720; y++)
        {
            const uint32_t* src = &canvas.px[(size_t)y * 1280];
            u32* dst = out + (size_t)y * (stride / 4);
            // (ARGB to the framebuffer's RGBA bytes.)
            for (int x = 0; x < 1280; x++)
            {
                const uint32_t p = src[x];
                dst[x] = 0xFF000000u | (p & 0xFF) << 16 | (p & 0xFF00) | (p >> 16 & 0xFF);
            }
        }
        framebufferEnd(&fb);
    }
    framebufferClose(&fb);
    return result;
}

// The games to choose from: rom.nds, or the ROMs in the program's folder (and its roms/ folder);
// in the all-games program also those in the other folders of /switch (the one-game programs'
// folders), each played with the save in its own folder, so that the two kinds of download share
// a game's save.
std::vector<Launcher::Game> FindGames(const std::string& dir, bool& ownFolder)
{
    std::vector<Launcher::Game> games;
    Launcher::Game g;
    ownFolder = true;
    if (Launcher::ReadGame(dir + "/rom.nds", dir, g)) return { g };
#ifndef SSR_ALL_GAMES
    // (A one-game program: its own folder only, as always.)
    Launcher::ScanFolder(dir, games, false);
#else
    Launcher::ScanFolder(dir, games);
    {
        const size_t own = games.size();
        if (DIR* d = opendir("sdmc:/switch"))
        {
            while (dirent* e = readdir(d))
            {
                const std::string folder = std::string("sdmc:/switch/") + e->d_name;
                if (e->d_name[0] == '.' || folder == dir) continue;
                struct stat st;
                if (stat(folder.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) Launcher::ScanFolder(folder, games);
            }
            closedir(d);
        }
        ownFolder = own > 0;
    }
#ifdef SSR_GBA
    // The GBA games: in this folder's roms (their saves beside the DS games') and a GBA folder in
    // it (with that folder's saves), and in Pokemon GBA Widescreen's own folder if it is on the
    // card too (with its saves, so they carry on).
    {
        std::map<std::string, std::vector<uint32_t>> art;
        const size_t before = games.size();
        GbaGames::AddWithGbaFolder(dir + "/roms", dir, games, art);
        const std::string gbaApp = "sdmc:/switch/pokemon-gba-wide";
        struct stat st;
        if (dir != gbaApp && stat((gbaApp + "/roms").c_str(), &st) == 0)
        {
            const size_t mine = games.size();
            GbaGames::Add(gbaApp + "/roms", gbaApp + "/saves", games, art);
            for (size_t i = mine; i < games.size(); i++)
                for (size_t j = before; j < mine; j++)
                    if (games[j].code == games[i].code) games[i].where = "IN GBA-WIDE";
        }
        ownFolder |= games.size() > before;
    }
#endif
#endif
    Launcher::AddComingSoon(games);
    Launcher::Sort(games);
    // The same game in two folders (two ROMs, two saves): each says where it is.
    for (size_t i = 0; i < games.size(); i++)
        for (size_t j = 0; j < games.size(); j++)
            if (i != j && games[i].platform == Launcher::DS && games[i].code == games[j].code && games[i].folder != dir)
            {
                // (pokemon-black2-wide: IN BLACK2)
                std::string name = games[i].folder.substr(games[i].folder.find_last_of('/') + 1);
                if (name.rfind("pokemon-", 0) == 0) name = name.substr(8);
                if (name.size() > 5 && name.compare(name.size() - 5, 5, "-wide") == 0) name.resize(name.size() - 5);
                games[i].where = "IN " + name.substr(0, 14);
            }
    return games;
}

// Set when the game list was shown: the pause menu can then go back to it.
bool g_canChange = false, g_changeGame = false;
}

// Set while a start with the JIT (and with its fast memory) has not yet run for a few
// seconds (see JitStarted).
static std::string g_jitMarker, g_fastMemMarker;

bool Start(int& argc, char**& argv)
{
    // The folder the .nro was started from (the Homebrew Menu passes its path), or the usual one.
    std::string dir = "sdmc:/switch/ssrecomp";
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0)
    {
        std::string self = argv[0];
        size_t slash = self.find_last_of('/');
        if (slash != std::string::npos && slash > 5) dir = self.substr(0, slash);
    }
    mkdir(dir.c_str(), 0777);
    chdir(dir.c_str());
    if (argc > 0 && argv[0]) g_self = argv[0];
    // A test run (testrun.h) from test.txt, if there is one to do: its log goes to its folder.
    std::vector<std::string> testArgs;
    std::string testNotes;
    // (Not when started with arguments, which are then used as they are.)
    const bool testRun = argc < 2 && TestRun::Setup(dir, testArgs, g_test, testNotes);
    const std::string logPath = testRun ? g_test.results + "/game.log" : dir + "/game.log";

    // Messages go to game.log beside the game, as run.sh does on a PC, with the frame timing,
    // so that a log can be sent with a bug report. The last session's log is kept as
    // game-previous.log: QUIT to the game list starts the program again, which would otherwise
    // replace the log of the game just played with the list's few lines. (Only a log longer
    // than the list's is kept, so going to the list twice keeps the game's.)
    if (!testRun)
    {
        struct stat ls;
        if (stat(logPath.c_str(), &ls) == 0 && ls.st_size > 1024)
        {
            const std::string prev = dir + "/game-previous.log";
            remove(prev.c_str());
            rename(logPath.c_str(), prev.c_str());
        }
    }
    if (!freopen(logPath.c_str(), "w", stderr)) freopen("game.log", "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    setenv("SSR_PERF", "1", 0);
    fprintf(stderr, "[switch] started from %s\n", argc > 0 && argv[0] ? argv[0] : "?");
    fputs(testNotes.c_str(), stderr);

    // Started from the album (applet mode) a homebrew program only gets a few hundred MB of
    // memory; the game's code alone is larger. Started in place of a game (holding R) it gets
    // nearly all of it.
    const AppletType type = appletGetAppletType();
    fprintf(stderr, "[switch] applet type %d\n", (int)type);
    if (type != AppletType_Application && type != AppletType_SystemApplication)
    {
        ShowMessage("  This needs the console's full memory.\n"
                    "  Start it from a game instead of the album:\n"
                    "  hold R while starting any game, then choose this game\n"
                    "  (or start it from its forwarder on the HOME Menu).");
        return false;
    }
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(stderr, "[switch] memory: %llu MB in use of %llu MB\n", (unsigned long long)(used >> 20),
        (unsigned long long)(total >> 20));

    // "--game IREO" (the HOME Menu icons: switch_forwarder.cpp): that game at once, without the
    // list. Other arguments (e.g. sent with nxlink while testing) are used as they are.
    std::string direct;
#ifdef SSR_ALL_GAMES
    if (argc == 3 && !strcmp(argv[1], "--game"))
    {
        direct = argv[2];
        for (char& c : direct) c = (char)toupper((unsigned char)c);
    }
    else
#endif
    if (argc >= 2) return true;

    // The game: the only one there is, or the one chosen from the list (launcher.h).
    bool ownFolder = true;
    std::vector<Launcher::Game> games = FindGames(dir, ownFolder);
#ifdef SSR_ALL_GAMES
    // The all-games download comes with an empty roms folder; made here too, in case it was lost
    // in copying, so that the message below points somewhere that is there.
    mkdir((dir + "/roms").c_str(), 0777);
    const std::string romsDir = dir + "/roms/";
#else
    const std::string romsDir = dir + "/";
#endif
    if (games.empty())
    {
        ShowMessage("  No game found. Copy your ROMs (Pokemon Black, White, Black 2 or White 2 in\n"
                    "  English; SoulSilver, HeartGold or Platinum US) into\n  " + romsDir);
        return false;
    }
    int chosen = 0;
#ifndef SSR_ALL_GAMES
    // The one-game programs have no game list (only the all-games download does).
    if (games.size() > 1)
    {
        ShowMessage("  More than one .nds file in\n  " + dir + "\n  Rename the one to play to rom.nds.");
        return false;
    }
#else
    int directGame = -1;
    for (size_t i = 0; i < games.size() && !direct.empty(); i++)
        if (games[i].code == direct && Playable(games[i]))
        {
            directGame = (int)i;
            break;
        }
    if (!direct.empty() && directGame < 0) fprintf(stderr, "[switch] --game %s: not in the list\n", direct.c_str());
    if (directGame >= 0)
    {
        // (Noted as the game played last, so that the list starts on it after QUIT.)
        chosen = directGame;
        if (FILE* f = fopen((dir + "/last-game.txt").c_str(), "w"))
        {
            fprintf(f, "%s\n", games[chosen].rom.c_str());
            fclose(f);
        }
        fprintf(stderr, "[switch] --game %s: %s\n", direct.c_str(), games[chosen].rom.c_str());
        g_canChange = true;
    }
    else if (games.size() > 1)
    {
        // The list starts on the game played last.
        const std::string lastPath = dir + "/last-game.txt";
        std::string last;
        if (FILE* f = fopen(lastPath.c_str(), "r"))
        {
            char line[768] = {};
            if (fgets(line, sizeof(line), f)) last = line;
            fclose(f);
            while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) last.pop_back();
        }
        for (size_t i = 0; i < games.size(); i++)
            if (games[i].rom == last) chosen = (int)i;
        const auto showList = std::find(testArgs.begin(), testArgs.end(), "--show-list");
        if (testRun && showList != testArgs.end())
        {
            // (Shown for 90 frames, then the game played last, as if chosen.)
            testArgs.erase(showList);
            for (Launcher::Game& g : games) Launcher::FindArt(g, dir);
            chosen = PickGame(games, chosen, SettingsPath(dir), 90);
            fprintf(stderr, "[switch] test run: %s, after the game list\n", games[chosen].rom.c_str());
        }
        else if (testRun)
        {
            // (A test run, test.txt, plays the game played last without the list: no one is there.)
            fprintf(stderr, "[switch] test run: %s, without the game list\n", games[chosen].rom.c_str());
        }
        else
        {
            // A game chosen from the list is played in a fresh start of the program: the list
            // leaves something behind in the graphics (not the window's settings: those put back
            // made no difference) that slowed the game drawn after it, putting each picture
            // together taking HeartGold 12 ms a frame on the Switch instead of 4, and a start
            // after it was as fast as ever (test runs, "arg --show-list"). So the choice is noted
            // in last-game.txt and start-chosen, the program starts again (as QUIT does to show
            // the list), and that start plays the game without showing the list.
            const std::string chosenMarker = dir + "/start-chosen";
            struct stat ms;
            const bool alreadyChosen = stat(chosenMarker.c_str(), &ms) == 0;
            if (alreadyChosen) remove(chosenMarker.c_str());
            else
            {
                // Each game's picture, if the player has one (Launcher::FindArt).
                for (Launcher::Game& g : games) Launcher::FindArt(g, dir);
                // A game with no picture of its own takes one another copy of it has (the icon of
                // its one-game program, say).
#ifdef SSR_PC_ART
                // Else the box art built into the program (before another copy's picture, which is
                // its one-game program's icon: that can be a placeholder).
                for (Launcher::Game& g : games)
                {
                    size_t size = 0;
                    if (g.art.empty())
                        if (const unsigned char* data = PcLauncher::BuiltInArt(g.code.c_str(), size)) Launcher::LoadArt(data, size, g.art);
                }
#endif
                for (Launcher::Game& g : games)
                    for (const Launcher::Game& other : games)
                        if (g.art.empty() && !other.art.empty() && other.code == g.code) g.art = other.art;
                chosen = PickGame(games, chosen, SettingsPath(dir));
                if (chosen < 0) return false;
                if (FILE* f = fopen(lastPath.c_str(), "w"))
                {
                    fprintf(f, "%s\n", games[chosen].rom.c_str());
                    fclose(f);
                }
                if (!g_self.empty() && envHasNextLoad())
                {
                    if (FILE* f = fopen(chosenMarker.c_str(), "wb")) fclose(f);
                    const std::string cmd = g_self.find(' ') == std::string::npos ? g_self : "\"" + g_self + "\"";
                    const Result rc = envSetNextLoad(g_self.c_str(), cmd.c_str());
                    fprintf(stderr, "[switch] %s chosen: the program starts again to play it (0x%x)\n",
                        games[chosen].rom.c_str(), (unsigned)rc);
                    if (R_SUCCEEDED(rc)) return false;
                    remove(chosenMarker.c_str());
                }
            }
            g_canChange = true;
        }
    }
#endif
#ifdef SSR_GBA
    // A GBA game (chosen in the list, which started the program again for it, or from its HOME
    // Menu icon): played here, Pokemon GBA Widescreen built in; then the program starts again for
    // the list, as QUIT does for a DS game.
    if (games[chosen].platform == Launcher::GBA)
    {
        const Launcher::Game& game = games[chosen];
        GbaWide::Options o;
        o.roms = game.rom.substr(0, game.rom.find_last_of('/'));
        o.saves = game.folder;
        o.data = dir;
        o.fullscreen = true;
        fprintf(stderr, "[switch] GBA: %s, saves in %s\n", game.rom.c_str(), game.folder.c_str());
        GbaWide::Play(game.rom, o);
        if (!g_self.empty() && envHasNextLoad())
        {
            const std::string cmd = g_self.find(' ') == std::string::npos ? g_self : "\"" + g_self + "\"";
            envSetNextLoad(g_self.c_str(), cmd.c_str());
        }
        return false;
    }
#endif
    const Launcher::Game& game = games[chosen];
    const std::string rom = game.rom, gameDir = game.folder;
    char code[5] = {};
    memcpy(code, game.code.data(), std::min<size_t>(4, game.code.size()));
    // SoulSilver and HeartGold (US): the same program, layouts from its own tables.
    const bool heartSilver = !strcmp(code, "IPGE") || !strcmp(code, "IPKE") || !strcmp(code, "IPGF") || !strcmp(code, "IPKF");
    // The save is named after the game (SoulSilver's name is the one earlier builds used), in
    // the game's own folder.
    const std::string saveName = game.saveName;
    fprintf(stderr, "[switch] %s (game code %s), save %s/%s%s\n", rom.c_str(), code[0] ? code : "?", gameDir.c_str(),
        saveName.c_str(), ownFolder ? "" : " (from another folder)");
    // The settings: settings.cfg beside the game (ssrecomp.cfg before; an old one is renamed).
    const std::string cfgPath = SettingsPath(dir);
    g_args = { argc > 0 && argv[0] ? argv[0] : "pokemon-ds-wide", rom, "--save", gameDir + "/" + saveName, "--fullscreen",
               "--config", cfgPath };
    // A game that is not recompiled (Black, White, Black 2, White 2) comes with its function table, made on the
    // PC by tools/bw2/funcmap.py or tools/bw1/funcmap.py; its layouts follow the functions that
    // run. SoulSilver and HeartGold need none (their layouts come from their own tables, built into the program)
    // and must not be given one: a function table switches to the Black games' layouts.
    // A funcs.txt beside the game is used first (made from the player's ROM by
    // tools/switch/build_game.sh); otherwise the shared build's own, one per version in its
    // romfs (tools/switch/build_game.sh bw), whose overlay bytes are read from the ROM.
    struct stat st;
    std::string funcsPath;
    if (!heartSilver)
    {
        const std::string own = gameDir + "/funcs.txt", builtIn = std::string("romfs:/funcs/") + code + ".txt";
        if (stat(own.c_str(), &st) == 0) funcsPath = own;
        else if (RomfsReady() && stat("romfs:/funcs", &st) == 0)
        {
            if (stat(builtIn.c_str(), &st) == 0) funcsPath = builtIn;
            else
            {
                ShowMessage(std::string("  ") + rom.substr(rom.find_last_of('/') + 1) + " is a game this program does not know\n"
                            "  (game code " + (code[0] ? code : "?") + ").\n\n"
                            "  It plays the English versions of:\n"
                            "    Pokemon Black 2 (IREO), Pokemon White 2 (IRDO), Pokemon Black (IRBO)\n"
                            "    and Pokemon White (IRAO), US and Europe;\n"
                            "    Pokemon SoulSilver (IPGE) and Pokemon HeartGold (IPKE), US, with the\n"
                            "    SoulSilver and HeartGold download;\n"
                            "    Pokemon Platinum (CPUE), with the Platinum download.\n"
                            "  Other languages put the game's code in other places.");
                return false;
            }
        }
    }
    const bool funcs = !funcsPath.empty();
    if (funcs)
    {
        fprintf(stderr, "[switch] function table %s\n", funcsPath.c_str());
        g_args.push_back("--funcs");
        g_args.push_back(funcsPath);
    }
    // Black and Black 2 run on melonDS's JIT or its interpreter; so do SoulSilver and HeartGold in a build
    // with the JIT (tools/switch/build.sh with SSR_JIT=ON: about 1.7 times as fast as its
    // recompiled code; a build without the JIT runs the recompiled code, given neither option).
#ifdef JIT_ENABLED
    if (funcs || heartSilver)
    {
        // melonDS's JIT (code translated for the Switch's processor as it runs), unless the
        // L is held while the game starts, or the last start with the JIT never got going (a
        // file made before the start and removed a few seconds into the game says so).
        // Fast memory (the DS's memory mapped where the JIT's code reads it directly, see
        // melonDS/src/ARMJIT_Memory.cpp) likewise: if a start with it did not get going, the
        // next start goes without it, and the one after tries it again.
        bool jit = true, fastMem = true;
        if (FILE* f = fopen(cfgPath.c_str(), "r"))
        {
            char line[128];
            while (fgets(line, sizeof(line), f))
            {
                // (FAST JIT always: "jit=" and "fastmem=" from older versions' EMULATION
                // menu are not read any more, so no one is left on the slower choices.)
                // The widescreen drawing threads on core 0 too (melonDS/src/SsrThreads.h).
                if (!strncmp(line, "wide_core0=", 11) && line[11] == '0') setenv("SSR_WIDE_CORE0", "0", 1);
            }
            fclose(f);
        }
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        padUpdate(&pad);
        if (jit && (padGetButtons(&pad) & HidNpadButton_L))
        {
            jit = false;
            fprintf(stderr, "[switch] L held: the interpreter this time\n");
        }
        const std::string marker = dir + "/jit-starting", fastMemMarker = dir + "/fastmem-starting";
        if (stat(fastMemMarker.c_str(), &st) == 0)
        {
            // (Fast memory is the likelier cause, so the JIT stays on this time.)
            remove(fastMemMarker.c_str());
            remove(marker.c_str());
            if (jit && fastMem)
            {
                fastMem = false;
                fprintf(stderr, "[switch] the last start with fast memory did not get going: "
                    "the JIT without it this time\n");
            }
        }
        if (jit && stat(marker.c_str(), &st) == 0)
        {
            jit = false;
            remove(marker.c_str());
            fprintf(stderr, "[switch] the last start with the JIT did not get going: the interpreter this time\n");
        }
        if (jit)
        {
            if (FILE* f = fopen(marker.c_str(), "wb")) fclose(f);
            g_jitMarker = marker;
            if (fastMem)
            {
                if (FILE* f = fopen(fastMemMarker.c_str(), "wb")) fclose(f);
                g_fastMemMarker = fastMemMarker;
            }
        }
        // (Read by the core, melonDS/src/ARMJIT_Memory.cpp, and by MakeConsole.)
        if (!fastMem) setenv("SSR_JIT_NOFASTMEM", "1", 1);
        g_args.push_back(jit ? "--jit" : "--interp");
    }
#else
    if (funcs) g_args.push_back("--interp");
#endif
    if (testRun)
    {
        g_args.insert(g_args.end(), testArgs.begin(), testArgs.end());
        // Each frame is shown alongside the next one's emulation, as in play (see
        // "pipelined" in main.cpp), pictures or not.
        setenv("SSR_PIPELINE", "1", 1);
    }
    for (auto& a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
    argc = (int)g_args.size();
    argv = g_argv.data();
    return true;
}

bool CanChangeGame() { return g_canChange && envHasNextLoad(); }
void ChangeGame() { g_changeGame = true; }

bool LocalTime(int& year, int& month, int& day, int& hour, int& minute, int& second)
{
    // newlib's localtime() knows no time zone here and gives UTC; the game lights the world
    // by the hour, so the console's own zone is asked for.
    u64 now = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &now))) return false;
    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo info;
    if (R_FAILED(timeToCalendarTimeWithMyRule(now, &cal, &info))) return false;
    year = cal.year;
    month = cal.month;
    day = cal.day;
    hour = cal.hour;
    minute = cal.minute;
    second = cal.second;
    return true;
}

static Thread g_emuThread;

bool StartThread(void (*entry)(void*), void* arg)
{
    if (R_FAILED(threadCreate(&g_emuThread, entry, arg, nullptr, 0x400000, 0x2C, 0))) return false;
    if (R_FAILED(threadStart(&g_emuThread)))
    {
        threadClose(&g_emuThread);
        return false;
    }
    return true;
}

void JoinThread()
{
    threadWaitForExit(&g_emuThread);
    threadClose(&g_emuThread);
}

void MoveMainThread()
{
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1 << 2);
    fprintf(stderr, "[switch] main thread to core 2: 0x%x\n", (unsigned)rc);
}

void JitStarted()
{
    if (!g_fastMemMarker.empty())
    {
        remove(g_fastMemMarker.c_str());
        g_fastMemMarker.clear();
    }
    if (g_jitMarker.empty()) return;
    remove(g_jitMarker.c_str());
    g_jitMarker.clear();
    fprintf(stderr, "[switch] the JIT is running\n");
}

std::string Status()
{
    // Test runs only, one session at a time, each closed straight after. The clocks come from
    // the system's pcv service and the temperatures from ptm, and pcv stops the whole console
    // when it has no room for another session (fatal 2011-0102, "out of session memory"; sys-clk
    // opens its own several times a second). That happened twice, both times a few seconds into
    // a game, at this line's first reading, when the three clock sessions were opened and kept
    // for the whole game. A player's log does without these numbers.
    if (!g_test.active) return {};
    char buf[128];
    int n = 0;
    buf[0] = 0;
    u32 mhz[3] = {};
    bool clocks = hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize());
    if (clocks)
    {
        const PcvModuleId modules[3] = { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC };
        for (int i = 0; i < 3 && clocks; i++)
        {
            ClkrstSession s;
            clocks = R_SUCCEEDED(clkrstOpenSession(&s, modules[i], 3));
            if (!clocks) break;
            clocks = R_SUCCEEDED(clkrstGetClockRate(&s, &mhz[i]));
            clkrstCloseSession(&s);
        }
        clkrstExit();
    }
    if (clocks)
        n += snprintf(buf + n, sizeof(buf) - n, "cpu %u gpu %u mem %u MHz", mhz[0] / 1000000, mhz[1] / 1000000, mhz[2] / 1000000);
    if (hosversionAtLeast(10, 0, 0) && R_SUCCEEDED(tsInitialize()))
    {
        const TsDeviceCode places[2] = { TsDeviceCode_LocationExternal, TsDeviceCode_LocationInternal };
        float temp[2] = {};
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++)
        {
            TsSession s;
            ok = R_SUCCEEDED(tsOpenSession(&s, places[i]));
            if (!ok) break;
            ok = R_SUCCEEDED(tsSessionGetTemperature(&s, &temp[i]));
            tsSessionClose(&s);
        }
        if (ok) n += snprintf(buf + n, sizeof(buf) - n, "%schip %.0fC board %.0fC", n ? ", " : "", temp[0], temp[1]);
        tsExit();
    }
    return buf;
}

void ForceExit()
{
    fflush(stderr);
    svcExitProcess();
    for (;;) {}
}

void Finish()
{
    JitStarted(); // (an ordinary exit: the JIT did start)
    // QUIT in the pause menu, for a game chosen from the game list: the Homebrew Menu's loader
    // starts the program again, which shows the list.
    if (g_changeGame && !g_self.empty())
    {
        const std::string cmd = g_self.find(' ') == std::string::npos ? g_self : "\"" + g_self + "\"";
        const Result rc = envSetNextLoad(g_self.c_str(), cmd.c_str());
        fprintf(stderr, "[switch] back to the game list: 0x%x\n", (unsigned)rc);
    }
    fflush(stderr);
    if (TestRun::Finish(g_test))
    {
        // The next run: the Homebrew Menu's loader starts the .nro again once this one ends
        // (if it cannot, starting it again by hand does the same).
        if (!g_self.empty() && envHasNextLoad())
        {
            const std::string cmd = g_self.find(' ') == std::string::npos ? g_self : "\"" + g_self + "\"";
            const Result rc = envSetNextLoad(g_self.c_str(), cmd.c_str());
            fprintf(stderr, "[test] starting again for the next run: 0x%x\n", (unsigned)rc);
        }
        else fprintf(stderr, "[test] start the game again for the next run\n");
        fflush(stderr);
    }
}
}
#endif
