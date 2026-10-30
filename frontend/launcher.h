#pragma once
// The game picker of the all-games download (one program for every game): finds the players'
// ROMs, reads each one's name and icon from the ROM itself (every DS game carries a 32x32 icon
// and its title in its banner), and draws the list. Kept free of any platform so that it can be
// drawn into a picture on a PC as well; the Switch shows it with switch_platform.cpp before the
// game's window opens.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <cmath>
#include <vector>
#include <dirent.h>
#include "ssr_os.h"
#include <sys/stat.h>

#include "ui.h"
// (JPEG and PNG pictures for the list: the program that draws it defines STB_IMAGE_IMPLEMENTATION
// with STBI_ONLY_JPEG and STBI_ONLY_PNG before including this, in one file.)
#include "thirdparty/stb_image.h"

namespace Launcher
{

struct Game
{
    std::string rom;      // the ROM's full path
    std::string folder;   // where its save (and a funcs.txt of the player's own) live
    std::string code;     // the game code, e.g. IREO
    std::string name;     // as shown, e.g. POKEMON BLACK 2
    std::string saveName; // e.g. black2.sav
    bool known = false;   // one of the games this program plays
    bool hasSave = false;
    bool hasIcon = false;
    uint32_t icon[32 * 32] = {}; // ARGB, transparent where the icon is
    // A larger picture for the game, if the player has one (FindArt): ARGB, Art x Art pixels.
    std::vector<uint32_t> art;
    // Set when the same game is also in another folder: this one's folder, shown under it.
    std::string where;
};

// The size the larger pictures are kept at (the tiles of the list at 720 lines).
constexpr int Art = 224;

// The games this program plays, in the order the list shows them.
struct KnownGame
{
    const char* code;
    const char* name;
    const char* save;
};
inline const KnownGame* Known(const std::string& code)
{
    static const KnownGame games[] = {
        { "IRBO", "Pokemon Black", "black.sav" },
        { "IRBF", "Pokemon Black (FR)", "black-fr.sav" },
        { "IRAO", "Pokemon White", "white.sav" },
        { "IRAF", "Pokemon White (FR)", "white-fr.sav" },
        { "IREO", "Pokemon Black 2", "black2.sav" },
        { "IREF", "Pokemon Black 2 (FR)", "black2-fr.sav" },
        { "IRDO", "Pokemon White 2", "white2.sav" },
        { "IRDF", "Pokemon White 2 (FR)", "white2-fr.sav" },
        { "IPKE", "Pokemon HeartGold", "heartgold.sav" },
        { "IPKF", "Pokemon HeartGold (FR)", "heartgold-fr.sav" },
        { "IPGE", "Pokemon SoulSilver", "soulsilver.sav" },
        { "IPGF", "Pokemon SoulSilver (FR)", "soulsilver-fr.sav" },
        { "CPUE", "Pokemon Platinum", "platinum.sav" },
        { "CPUF", "Pokemon Platinum (FR)", "platinum-fr.sav" },
    };
    for (const KnownGame& g : games)
        if (code == g.code) return &g;
    return nullptr;
}
inline int Order(const std::string& code)
{
    static const char* order[] = { "IRBO", "IRAO", "IREO", "IRDO", "IPKE", "IPGE", "CPUE" };
    for (int i = 0; i < 7; i++)
        if (code == order[i] || (code.size() == 4 && code[3] == 'F' && code.compare(0, 3, order[i], 3) == 0)) return i;
    return 7;
}

inline bool EndsWithNds(const std::string& name)
{
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".nds";
}

// Reads a ROM's game code, title and icon. False if it is not a DS ROM.
inline bool ReadGame(const std::string& rom, const std::string& folder, Game& g)
{
    FILE* f = fopen(rom.c_str(), "rb");
    if (!f) return false;
    uint8_t header[0x70];
    bool ok = fread(header, 1, sizeof header, f) == sizeof header;
    g = Game();
    g.rom = rom;
    g.folder = folder;
    if (ok)
    {
        g.code.assign((const char*)header + 0x0C, 4);
        for (char c : g.code)
            if (c < 0x20 || c > 0x7E) ok = false;
    }
    // The banner: version, CRCs, then the icon's 4-bit pixels (4x4 tiles of 8x8), its 16
    // colours (BGR555, the first transparent) and the title in six languages (UTF-16).
    const uint32_t banner = ok ? header[0x68] | header[0x69] << 8 | header[0x6A] << 16 | (uint32_t)header[0x6B] << 24 : 0;
    uint8_t ban[0x440];
    if (ok && banner && fseek(f, (long)banner, SEEK_SET) == 0 && fread(ban, 1, sizeof ban, f) == sizeof ban)
    {
        uint32_t pal[16];
        for (int i = 0; i < 16; i++)
        {
            const uint16_t c = (uint16_t)(ban[0x220 + 2 * i] | ban[0x221 + 2 * i] << 8);
            const uint32_t r = (c & 31) * 255 / 31, gr = (c >> 5 & 31) * 255 / 31, b = (c >> 10 & 31) * 255 / 31;
            pal[i] = (i ? 0xFF000000u : 0u) | r << 16 | gr << 8 | b;
        }
        for (int ty = 0; ty < 4; ty++)
            for (int tx = 0; tx < 4; tx++)
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                    {
                        const uint8_t byte = ban[0x20 + (ty * 4 + tx) * 32 + y * 4 + x / 2];
                        g.icon[(ty * 8 + y) * 32 + tx * 8 + x] = pal[x & 1 ? byte >> 4 : byte & 15];
                    }
        g.hasIcon = true;
        // The English title's first line, for a game this program does not know.
        for (int i = 0; i < 64; i++)
        {
            const uint16_t ch = (uint16_t)(ban[0x340 + 2 * i] | ban[0x341 + 2 * i] << 8);
            if (ch == 0 || ch == '\n') break;
            g.name += ch == 0xE9 ? 'E' : ch < 0x80 ? (char)ch : ' ';
        }
    }
    fclose(f);
    if (!ok) return false;
    if (const KnownGame* k = Known(g.code))
    {
        g.known = true;
        g.name = k->name;
        g.saveName = k->save;
    }
    else
    {
        if (g.name.empty()) g.name = rom.substr(rom.find_last_of('/') + 1);
        g.saveName = "game.sav";
    }
    for (char& c : g.name) c = (char)toupper((unsigned char)c);
    struct stat st;
    g.hasSave = stat((folder + "/" + g.saveName).c_str(), &st) == 0;
    return true;
}

// A JPEG or PNG picture, scaled to Art x Art (each pixel the average of the ones it covers).
inline bool LoadArt(const unsigned char* data, size_t size, std::vector<uint32_t>& out)
{
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!px) return false;
    out.assign((size_t)Art * Art, 0);
    for (int y = 0; y < Art; y++)
        for (int x = 0; x < Art; x++)
        {
            const int x0 = x * w / Art, x1 = std::max(x0 + 1, (x + 1) * w / Art);
            const int y0 = y * h / Art, y1 = std::max(y0 + 1, (y + 1) * h / Art);
            uint32_t sum[4] = {}, count = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                {
                    const unsigned char* p = px + ((size_t)yy * w + xx) * 4;
                    for (int c = 0; c < 4; c++) sum[c] += p[c];
                    count++;
                }
            out[(size_t)y * Art + x] = (sum[3] / count) << 24 | (sum[0] / count) << 16 | (sum[1] / count) << 8 | sum[2] / count;
        }
    stbi_image_free(px);
    return true;
}

inline bool ReadFile(const std::string& path, std::vector<unsigned char>& data, long offset = 0, long size = -1)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (size < 0)
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f) - offset;
    }
    bool ok = size > 0 && size < (16 << 20) && fseek(f, offset, SEEK_SET) == 0;
    if (ok)
    {
        data.resize((size_t)size);
        ok = fread(data.data(), 1, data.size(), f) == data.size();
    }
    fclose(f);
    return ok;
}

// The icon a Switch homebrew program carries (its Homebrew Menu icon, a JPEG), from a .nro file.
inline bool NroIcon(const std::string& nro, std::vector<unsigned char>& jpeg)
{
    std::vector<unsigned char> head;
    if (!ReadFile(nro, head, 0, 0x20) || memcmp(head.data() + 0x10, "NRO0", 4) != 0) return false;
    const long size = head[0x18] | head[0x19] << 8 | head[0x1A] << 16 | (long)head[0x1B] << 24;
    std::vector<unsigned char> aset;
    if (!ReadFile(nro, aset, size, 0x38) || memcmp(aset.data(), "ASET", 4) != 0) return false;
    uint64_t off = 0, len = 0;
    for (int i = 7; i >= 0; i--)
    {
        off = off << 8 | aset[0x08 + i];
        len = len << 8 | aset[0x10 + i];
    }
    return len > 0 && ReadFile(nro, jpeg, size + (long)off, (long)len);
}

// The game's larger picture, from the first of: <list folder>/icons/<game code>.jpg or .png (e.g.
// icons/IREO.jpg), a picture with the ROM's name (Black 2.nds: Black 2.jpg or .png), icon.jpg in
// the game's folder, or the Homebrew Menu icon of a one-game program (.nro) in its folder (not in
// the list's own folder: that one is the all-games program, whose icon is no game's). The
// downloads hold none: these are the player's own.
// Square ARGB pixels (side n) at side m: smooth both ways (bilinear when larger, the average of
// the pixels covered when smaller), with the colours weighted by their opacity.
inline std::vector<uint32_t> ResizeArt(const std::vector<uint32_t>& src, int n, int m)
{
    std::vector<uint32_t> out((size_t)m * m);
    auto at = [&](int x, int y) { return src[(size_t)std::min(n - 1, std::max(0, y)) * n + std::min(n - 1, std::max(0, x))]; };
    for (int y = 0; y < m; y++)
        for (int x = 0; x < m; x++)
        {
            double acc[4] = {}, wsum = 0;
            auto add = [&](uint32_t p, double w) {
                const double al = (p >> 24) / 255.0;
                acc[0] += w * al * ((p >> 16) & 255);
                acc[1] += w * al * ((p >> 8) & 255);
                acc[2] += w * al * (p & 255);
                acc[3] += w * (p >> 24);
                wsum += w;
            };
            if (m >= n)
            {
                const double fx = (x + 0.5) * n / m - 0.5, fy = (y + 0.5) * n / m - 0.5;
                const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
                const double tx = fx - x0, ty = fy - y0;
                add(at(x0, y0), (1 - tx) * (1 - ty));
                add(at(x0 + 1, y0), tx * (1 - ty));
                add(at(x0, y0 + 1), (1 - tx) * ty);
                add(at(x0 + 1, y0 + 1), tx * ty);
            }
            else
            {
                const int sx0 = x * n / m, sx1 = std::max(sx0 + 1, (x + 1) * n / m);
                const int sy0 = y * n / m, sy1 = std::max(sy0 + 1, (y + 1) * n / m);
                for (int sy = sy0; sy < sy1; sy++)
                    for (int sx = sx0; sx < sx1; sx++) add(at(sx, sy), 1);
            }
            const double alpha = acc[3] / wsum;
            const double k = alpha > 0 ? 255.0 / (alpha * wsum) : 0;
            auto c = [&](double v) { return (uint32_t)std::min(255.0, std::max(0.0, v * k + 0.5)); };
            out[(size_t)y * m + x] = ((uint32_t)std::min(255.0, alpha + 0.5) << 24) | (c(acc[0]) << 16) | (c(acc[1]) << 8) | c(acc[2]);
        }
    return out;
}

// The game's picture at side m, for an icon outside the list (desktop shortcuts, HOME Menu
// entries): its box art or picture, else its DS icon (sharp, as a pixel picture). Empty when it
// has neither.
inline std::vector<uint32_t> GamePicture(const Game& g, int m)
{
    if (!g.art.empty()) return ResizeArt(g.art, Art, m);
    if (!g.hasIcon) return {};
    std::vector<uint32_t> out((size_t)m * m);
    for (int y = 0; y < m; y++)
        for (int x = 0; x < m; x++) out[(size_t)y * m + x] = g.icon[(y * 32 / m) * 32 + x * 32 / m];
    return out;
}

inline void FindArt(Game& g, const std::string& listFolder)
{
    std::vector<unsigned char> data;
    const std::string base = g.rom.substr(0, g.rom.size() - 4);
    for (const std::string& path : { listFolder + "/icons/" + g.code + ".jpg", listFolder + "/icons/" + g.code + ".png",
                                     base + ".jpg", base + ".png", g.folder + "/icon.jpg" })
        if (ReadFile(path, data) && LoadArt(data.data(), data.size(), g.art)) return;
    if (g.folder == listFolder) return;
    if (DIR* d = opendir(g.folder.c_str()))
    {
        std::vector<std::string> nros;
        while (dirent* e = readdir(d))
        {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".nro") == 0) nros.push_back(g.folder + "/" + n);
        }
        closedir(d);
        std::sort(nros.begin(), nros.end());
        for (const std::string& nro : nros)
            if (NroIcon(nro, data) && LoadArt(data.data(), data.size(), g.art)) return;
    }
}

// The ROMs in a folder (and in its roms/ folder, with subfolder); their saves go in the folder
// itself.
inline void ScanFolder(const std::string& folder, std::vector<Game>& out, bool subfolder = true)
{
    for (const std::string& dir : { folder, subfolder ? folder + "/roms" : std::string() })
    {
        if (dir.empty()) continue;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (EndsWithNds(e->d_name)) names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names)
        {
            Game g;
            if (ReadGame(dir + "/" + n, folder, g)) out.push_back(g);
        }
    }
}

// Known games first, in the list's order, then the rest by name.
inline void Sort(std::vector<Game>& games)
{
    std::stable_sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        const int oa = Order(a.code), ob = Order(b.code);
        if (oa != ob) return oa < ob;
        return a.name < b.name;
    });
}

// The list's colours (ARGB): near-black with white text and red details, on the Switch and on a PC.
struct Theme
{
    uint32_t bg = 0xFF0B0B0C, band = 0xFF141416, bandLine = 0xFFE0242C, title = 0xFFFFFFFF;
    uint32_t tile = 0xFF1A1A1D, frame = 0xFFE0242C;
    uint32_t name = 0xFFB8B8BC, nameSel = 0xFFFFFFFF, sub = 0xFF707076, subSel = 0xFFE0242C, bad = 0xFFE0242C;
    uint32_t hint = 0xFF8A8A90;
    int bandLineH = 1;       // the band's line, in font pixels
    int frameW = 2;          // the chosen game's frame, in font pixels
    uint32_t selBar = 0;     // a bar under the chosen game's name (0: none)
};

// Where the list's tiles go on a canvas (Draw, and TileAt for a mouse): three across, or four
// when there are more than six games (all seven on one screen).
struct Grid
{
    int s, gap, labelH, top, bottom, tile, cols, rows, gx;
};
inline Grid Layout(const Canvas& c, int count)
{
    Grid g;
    g.s = std::max(1, c.h / 240); // the font's scale: 3 at 720 lines
    // Tiles three across and two rows down (six games on one screen), at most Art x Art at 720
    // lines.
    g.gap = 8 * g.s;
    g.labelH = 20 * g.s;
    g.top = 28 * g.s;
    g.bottom = c.h - 16 * g.s;
    g.tile = std::max(32, std::min(Art * c.h / 720, (g.bottom - g.top) / 2 - g.labelH - g.gap));
    g.cols = std::max(1, std::min(count > 6 ? 4 : 3, (c.w - g.gap) / (g.tile + g.gap * 3)));
    g.rows = std::max(1, (g.bottom - g.top) / (g.tile + g.labelH + g.gap));
    const int gridW = g.cols * g.tile + (g.cols - 1) * g.gap * 3;
    g.gx = (c.w - gridW) / 2;
    return g;
}

// The game whose tile (with its name below it) is at x, y on the canvas, or -1.
inline int TileAt(const Canvas& c, int count, int first, int x, int y)
{
    const Grid g = Layout(c, count);
    for (int i = first * g.cols; i < count && i < (first + g.rows) * g.cols; i++)
    {
        const int tx = g.gx + (i % g.cols) * (g.tile + g.gap * 3), ty = g.top + (i / g.cols - first) * (g.tile + g.labelH + g.gap);
        if (x >= tx && x < tx + g.tile && y >= ty && y < ty + g.tile + g.labelH) return i;
    }
    return -1;
}

// The list, drawn over the whole canvas (1280x720 on the Switch): a grid of tiles, each the
// game's picture (or its DS icon, enlarged) with its name and whether it has a save. first is the
// first row shown. hint: the line along the bottom (the Switch's buttons when empty). Returns the
// number of columns, for moving up and down.
inline int Draw(Canvas& c, const std::vector<Game>& games, int selected, int& first, const std::string& note,
                const std::string& hintText = std::string(), const Theme& th = Theme())
{
    const Grid grid = Layout(c, (int)games.size());
    const int s = grid.s;
    c.Fill(0, 0, c.w, c.h, th.bg);
    // A band along the top with the title.
    c.Fill(0, 0, c.w, 22 * s, th.band);
    c.Fill(0, 22 * s, c.w, th.bandLineH * s, th.bandLine);
    const std::string title = "POKEMON DS WIDESCREEN";
    c.Text((c.w - Canvas::TextWidth(title, s + 1)) / 2, 6 * s, title, s + 1, th.title);

    const int gap = grid.gap, labelH = grid.labelH, top = grid.top, bottom = grid.bottom;
    const int tile = grid.tile, cols = grid.cols, rows = grid.rows, gx = grid.gx;
    const int selRow = selected / cols;
    if (selRow < first) first = selRow;
    if (selRow >= first + rows) first = selRow - rows + 1;
    for (int i = first * cols; i < (int)games.size() && i < (first + rows) * cols; i++)
    {
        const Game& g = games[i];
        const bool sel = i == selected;
        const int x = gx + (i % cols) * (tile + gap * 3), y = top + (i / cols - first) * (tile + labelH + gap);
        // A frame round the chosen one.
        if (sel) c.Fill(x - th.frameW * s, y - th.frameW * s, tile + 2 * th.frameW * s, tile + 2 * th.frameW * s, th.frame);
        c.Fill(x, y, tile, tile, th.tile);
        if (!g.art.empty())
        {
            for (int py = 0; py < tile; py++)
            {
                const uint32_t* row = &g.art[(size_t)(py * Art / tile) * Art];
                for (int px = 0; px < tile; px++)
                {
                    const uint32_t p = row[px * Art / tile];
                    if (p >> 24 >= 0x80) c.px[(size_t)(y + py) * c.w + x + px] = p | 0xFF000000u;
                }
            }
            c.Fill(x, y, 1, 1, th.tile); // (so the canvas knows the tile was drawn)
        }
        else if (g.hasIcon)
        {
            // The DS icon, enlarged by whole pixels.
            const int is = std::max(1, tile / 40), o = (tile - 32 * is) / 2;
            for (int py = 0; py < 32; py++)
                for (int px = 0; px < 32; px++)
                    if (g.icon[py * 32 + px] >> 24) c.Fill(x + o + px * is, y + o + py * is, is, is, g.icon[py * 32 + px]);
        }
        // The name (shortened: POKEMON is the same for all) and the save.
        std::string name = g.name;
        if (name.rfind("POKEMON ", 0) == 0) name = name.substr(8);
        const int ns = Canvas::TextWidth(name, s) <= tile ? s : std::max(1, s - 1);
        c.Text(x + (tile - Canvas::TextWidth(name, ns)) / 2, y + tile + 3 * s, name, ns, sel ? th.nameSel : th.name);
        if (sel && th.selBar) c.Fill(x + tile / 4, y + tile + 19 * s, tile / 2, s, th.selBar);
        std::string sub = !g.known ? "NOT PLAYABLE" : g.hasSave ? "SAVE FOUND" : "NEW GAME";
        if (!g.where.empty() && g.known) sub = (g.hasSave ? "SAVE " : "NEW ") + g.where; // e.g. SAVE IN BLACK2
        const int ss = std::max(1, s * 2 / 3);
        c.Text(x + (tile - Canvas::TextWidth(sub, ss)) / 2, y + tile + 12 * s, sub, ss, !g.known ? th.bad : sel ? th.subSel : th.sub);
    }
    // More games above or below the ones shown.
    if (first > 0) c.Fill(c.w / 2 - 8 * s, top - 4 * s, 16 * s, s, th.hint);
    if ((first + rows) * cols < (int)games.size()) c.Fill(c.w / 2 - 8 * s, bottom - 2 * s, 16 * s, s, th.hint);
    const std::string hint = !note.empty() ? note : !hintText.empty() ? hintText : "A: PLAY    Y: SETTINGS    +: QUIT";
    c.Text((c.w - Canvas::TextWidth(hint, s)) / 2, c.h - 12 * s, hint, s, th.hint);
    return cols;
}

// The settings page (Y on the Switch's list, H or a controller's Y on a PC's): the pause menu's
// options with what each does, set here before playing. They are written to the same settings
// file the games read (main.cpp, Settings: keys, values and defaults must stay in step with it),
// a line at a time, leaving the others as they are. Entries without a key are what can only be
// done in the pause menu. The list's font has capitals, digits and : - + . / < > ! ( ) only.
struct Setting
{
    const char* key;  // nullptr: told about, not set here; "!...": something to do, not stored
    const char* name;
    std::vector<std::pair<std::string, std::string>> values; // stored, shown
    std::string def;  // the stored value when the file has none ("": the game decides, AUTO)
    const char* text;
};
inline std::vector<Setting> SettingsList()
{
    const std::vector<std::pair<std::string, std::string>> onOff = { { "1", "ON" }, { "0", "OFF" } };
    std::vector<Setting> l;
#ifdef __SWITCH__
    // (The first start's 60 FPS and WORLD DETAIL depend on the game: SoulSilver and HeartGold
    // start lighter. Until either is set, AUTO.)
    l.push_back({ "fps60", "60 FPS", onOff, "", "ON: THE 3D WORLD GETS IN-BETWEEN FRAMES AND MOVES AT 60 FRAMES A SECOND (THE GAMES DRAW IT AT 30). OFF: 30 AS ON A DS." });
#else
    // (A PC, and the Android app: no BORDERLESS FULLSCREEN, WORLD ON GPU or STICK TOUCH CURSOR
    // there, which is always full screen, draws the world with the processor and has a touch
    // screen.)
    l.push_back({ "view", "SCREEN LAYOUT", { { "merged", "MERGED" }, { "hybrid", "HYBRID" }, { "side", "SIDE" }, { "vertical", "VERTICAL" }, { "top", "TOP" }, { "bottom", "BOTTOM" } }, "merged",
                  "MERGED: THE ONE WIDESCREEN PICTURE. THE OTHERS SHOW THE TWO DS SCREENS LAID OUT AS ON OTHER EMULATORS." });
    l.push_back({ "large_screen", "LARGE SCREEN", { { "auto", "AUTO" }, { "top", "TOP" }, { "bottom", "BOTTOM" } }, "auto",
                  "FOR THE LAYOUTS WITH A LARGE AND A SMALL SCREEN: WHICH ONE IS LARGE. AUTO FOLLOWS WHAT THE GAME IS DOING." });
    l.push_back({ "fps60", "60 FPS", onOff, "1", "ON: THE 3D WORLD GETS IN-BETWEEN FRAMES AND MOVES AT 60 FRAMES A SECOND (THE GAMES DRAW IT AT 30). OFF: 30 AS ON A DS." });
#ifndef SSR_PHONE
    l.push_back({ "fullscreen", "BORDERLESS FULLSCREEN", onOff, "1", "ON: FILLS YOUR SCREEN AS A BORDERLESS WINDOW AT ITS OWN RESOLUTION. SWITCHING TO OTHER WINDOWS IS INSTANT. F11 DOES THE SAME." });
#endif
#endif
    l.push_back({ "smooth", "SMOOTH SCALING", onOff, "0", "ON: THE PICTURE IS SOFTENED AS IT IS SCALED UP TO YOUR SCREEN. OFF: SHARP PIXELS." });
    l.push_back({ "wide", "WIDESCREEN WORLD", onOff, "1", "ON: THE 3D WORLD IS DRAWN OUT TO THE SIDES OF THE 16:9 SCREEN. OFF: ONLY AS WIDE AS A DS SCREEN." });
    l.push_back({ "wide_map", "WIDE MAP LOADING", onOff, "1", "BLACK / WHITE / BLACK 2 / WHITE 2: LOADS MORE OF THE MAP AROUND YOU SO THE WIDE VIEW HAS LAND AT ITS EDGES." });
    l.push_back({ "battle_buttons", "BATTLE BUTTONS", { { "corner", "CORNER" }, { "panel", "PANEL" }, { "strip", "STRIP" } }, "strip",
                  "WHERE THE BATTLE MENU GOES. CORNER: SMALL BUTTONS IN A CORNER. PANEL: THE FIGHT / BAG / POKEMON / RUN PANEL FROM THE GAME BESIDE THE BATTLE. "
                  "STRIP: LARGE BUTTONS IN THE MESSAGE BOX IN BLACK AND WHITE (CORNER IN THE OTHERS). SOULSILVER / HEARTGOLD / PLATINUM HAVE THEIR OWN." });
#ifdef __SWITCH__
    l.push_back({ "wide_lines", "WORLD DETAIL", { { "192", "192" }, { "288", "288" }, { "384", "384" }, { "540", "540" }, { "720", "720" } }, "",
                  "HOW MANY LINES THE WIDE 3D WORLD IS DRAWN WITH. 720 IS FULL SHARPNESS ON THE SWITCH. LOWER IS LIGHTER ON THE CONSOLE." });
    l.push_back({ "busy_detail", "LOWER DETAIL WHEN BUSY", onOff, "0", "ON: DROPS THE WORLD DETAIL FOR A MOMENT WHEN DRAWING CANNOT KEEP UP (A MOVE FULL OF EFFECTS) INSTEAD OF SLOWING THE GAME DOWN." });
#else
#ifdef SSR_PHONE
    l.push_back({ "wide_lines", "3D RESOLUTION", { { "192", "192P (DS)" }, { "288", "288P" }, { "384", "384P" }, { "540", "540P" }, { "720", "720P" }, { "1080", "1080P" } }, "720",
                  "HOW SHARP THE 3D WORLD IS. 720P SUITS MOST PHONES. LOWER IF YOURS CANNOT KEEP UP OR TO SAVE BATTERY. SPRITES AND TEXT KEEP THEIR DS PIXELS." });
    l.push_back({ "busy_detail", "LOWER RES WHEN BUSY", onOff, "0", "ON: DROPS THE 3D RESOLUTION FOR A MOMENT WHEN THE PHONE CANNOT KEEP UP (A MOVE FULL OF EFFECTS) INSTEAD OF SLOWING THE GAME DOWN." });
#else
    l.push_back({ "wide_lines", "3D RESOLUTION", { { "192", "192P (DS)" }, { "288", "288P" }, { "384", "384P" }, { "540", "540P" }, { "720", "720P" }, { "1080", "1080P" }, { "1440", "1440P (2K)" }, { "2160", "2160P (4K)" } },
#ifdef __APPLE__
                  "720", // (drawn by the processor on a Mac)
#else
                  "1080",
#endif

                  "HOW SHARP THE 3D WORLD IS. PICK YOUR SCREEN RESOLUTION OR LOWER IF YOUR PC CANNOT KEEP UP. SPRITES AND TEXT KEEP THEIR DS PIXELS." });
    l.push_back({ "busy_detail", "LOWER RES WHEN BUSY", onOff, "0", "ON: DROPS THE 3D RESOLUTION FOR A MOMENT WHEN YOUR PC CANNOT KEEP UP (A MOVE FULL OF EFFECTS) INSTEAD OF SLOWING THE GAME DOWN." });
    l.push_back({ "ff_low_res", "FAST FORWARD LOW RES", onOff, "0", "ON: THE 3D WORLD DROPS TO 360P WHILE YOU FAST FORWARD FOR MORE FPS (A FASTER FAST FORWARD IF YOUR PC CANNOT KEEP UP). OFF: YOUR 3D RESOLUTION STAYS." });
#ifndef __APPLE__ // (a Mac's OpenGL stops at 4.1: the processor draws the world)
    l.push_back({ "wide_gpu", "WORLD ON GPU", onOff, "1", "ON: THE WIDE 3D WORLD IS DRAWN BY YOUR GRAPHICS CARD (OPENGL 4.3). TURN IT OFF IF THE WORLD LOOKS WRONG OR THE GAME WILL NOT START: THE PROCESSOR THEN DRAWS IT (THAT WANTS A FAST PC)." });
#endif
#endif
#endif
    l.push_back({ "show_fps", "FPS COUNTER", onOff, "0", "SHOWS THE FRAMES A SECOND IN A CORNER." });
#ifdef __SWITCH__
    l.push_back({ "start_button", "START BUTTON", { { "menu", "PAUSE MENU" }, { "ds", "DS START" } }, "menu",
                  "PAUSE MENU: + OPENS THE PAUSE MENU WHILE PLAYING. DS START: + IS THE DS START BUTTON (THE GAMES HARDLY USE IT) AND + AND - TOGETHER OPEN THE MENU." });
#elif defined(SSR_PHONE)
    l.push_back({ "start_button", "START BUTTON", { { "menu", "PAUSE MENU" }, { "ds", "DS START" } }, "menu",
                  "PAUSE MENU: START ON A CONTROLLER OPENS THE PAUSE MENU WHILE PLAYING. DS START: IT IS THE DS START BUTTON (THE GAMES HARDLY USE IT). THE START ON THE SCREEN IS ALWAYS THE DS START." });
#else
    l.push_back({ "start_button", "START BUTTON", { { "menu", "PAUSE MENU" }, { "ds", "DS START" } }, "menu",
                  "PAUSE MENU: START ON A CONTROLLER (MENU ON XBOX / OPTIONS ON PS) OPENS THE PAUSE MENU WHILE PLAYING. DS START: IT IS THE DS START BUTTON (THE GAMES HARDLY USE IT). ENTER IS ALWAYS THE DS START." });
#endif
#ifndef __SWITCH__
    l.push_back({ "buttons", "BUTTONS", { { "labels", "BY LABEL" }, { "positions", "BY POSITION" } }, "labels",
                  "BY LABEL: THE BUTTON MARKED A IS THE DS A ON ANY CONTROLLER. BY POSITION: THE BUTTONS WHERE A DS HAS THEM (XBOX A IS DS B)." });
#ifndef SSR_PHONE
    l.push_back({ "pad_touch", "STICK TOUCH CURSOR", onOff, "0", "ON: THE RIGHT STICK MOVES A CURSOR ON THE TOUCH SCREEN AND THE LEFT TRIGGER PRESSES IT." });
    // (frontend/shortcuts.h, made by the game list: pc_launcher.cpp. Not on a Mac: its Dock and
    // Launchpad start the app itself.)
#if defined(_WIN32)
    l.push_back({ "!shortcuts", "DESKTOP SHORTCUTS", { { "", "CREATE" } }, "",
                  "PUTS A SHORTCUT FOR EACH GAME IN THIS LIST ON YOUR DESKTOP WITH ITS PICTURE. IT STARTS THAT GAME STRAIGHT AWAY (QUIT IN THE PAUSE MENU STILL COMES BACK TO THIS LIST). CREATE AGAIN AFTER ADDING GAMES OR MOVING THIS FOLDER." });
#elif !defined(__APPLE__)
    l.push_back({ "!shortcuts", "DESKTOP SHORTCUTS", { { "", "CREATE" } }, "",
                  "PUTS A SHORTCUT FOR EACH GAME IN THIS LIST ON YOUR DESKTOP AND IN YOUR APPS MENU WITH ITS PICTURE. IT STARTS THAT GAME STRAIGHT AWAY. ON THE STEAM DECK THEY ALSO SHOW UP IN ADD A NON-STEAM GAME. CREATE AGAIN AFTER ADDING GAMES OR MOVING THIS FOLDER." });
#endif
#endif
#endif
#if defined(__SWITCH__) && defined(SSR_ALL_GAMES)
    // (frontend/switch_forwarder.cpp, made by the game list: switch_platform.cpp.)
    l.push_back({ "!forwarders", "HOME MENU ICONS", { { "", "INSTALL" } }, "",
                  "PUTS AN ICON FOR EACH GAME IN THIS LIST ON THE HOME MENU WITH ITS PICTURE. IT STARTS THAT GAME STRAIGHT AWAY. MADE ON THIS CONSOLE: NO PC OR KEYS FILE NEEDED. LIKE ANY FORWARDER THE ICONS NEED SIGPATCHES TO START. INSTALL AGAIN AFTER ADDING GAMES. REMOVE ONE IN THE HOME MENU (OPTIONS / DELETE SOFTWARE)." });
#endif
    // Only in the pause menu.
#ifdef __SWITCH__
    l.push_back({ nullptr, "THE PAUSE MENU", {}, "", "PRESS + WHILE PLAYING (+ AND - TOGETHER WITH START BUTTON ON DS START). EVERYTHING ABOVE IS THERE TOO AND CAN BE CHANGED AS YOU PLAY." });
    l.push_back({ nullptr, "FAST FORWARD", {}, "", "HOLD ZR (UP TO 4X). FAST FORWARD IN THE PAUSE MENU KEEPS IT ON WITHOUT HOLDING." });
#elif defined(SSR_PHONE)
    l.push_back({ nullptr, "THE PAUSE MENU", {}, "", "THE MENU BUTTON AT THE TOP OF THE SCREEN WHILE PLAYING OR START OR GUIDE ON A CONTROLLER. EVERYTHING ABOVE IS THERE TOO." });
    l.push_back({ nullptr, "FAST FORWARD", {}, "", "HOLD THE RIGHT TRIGGER OF A CONTROLLER (UP TO 4X). FAST FORWARD IN THE PAUSE MENU KEEPS IT ON WITHOUT HOLDING." });
#else
    l.push_back({ nullptr, "THE PAUSE MENU", {}, "", "ESC ON THE KEYBOARD. ON A CONTROLLER: START (MENU ON XBOX / OPTIONS ON PS) OR ITS GUIDE BUTTON. EVERYTHING ABOVE IS THERE TOO." });
    l.push_back({ nullptr, "FAST FORWARD", {}, "", "HOLD THE RIGHT TRIGGER OR LEFT SHIFT (UP TO 4X). FAST FORWARD IN THE PAUSE MENU KEEPS IT ON WITHOUT HOLDING." });
#endif
#ifdef __SWITCH__
    l.push_back({ "!keys", "KEYS AND BUTTONS", { { "", "CHANGE" } }, "",
                  "GIVE ANY DS BUTTON / THE PAUSE MENU / FAST FORWARD / SWAP SCREENS ANOTHER BUTTON (ANY CONTROLLER: PRO / JOY-CON OR ONE ADDED WITH SYS-CON OR MISSIONCONTROL). GIVING ONE A BUTTON ANOTHER ONE HAD SWAPS THE TWO. ALSO IN THE PAUSE MENU (CONTROLS). THIS LIST FOLLOWS THE ONES YOU CHANGE." });
#else
    l.push_back({ "!keys", "KEYS AND BUTTONS", { { "", "CHANGE" } }, "",
                  "GIVE ANY DS BUTTON / THE PAUSE MENU / FAST FORWARD / SWAP SCREENS ANOTHER KEY OR CONTROLLER BUTTON. GIVING ONE A KEY OR BUTTON ANOTHER ONE HAD SWAPS THE TWO. ALSO IN THE PAUSE MENU (CONTROLS). THIS LIST FOLLOWS THE ONES YOU CHANGE (A PLAYS / B GOES BACK / Y OPENS THESE SETTINGS)." });
#endif
    l.push_back({ nullptr, "SAVE STATE / LOAD STATE", {}, "", "IN THE PAUSE MENU: ONE QUICK SLOT PER GAME. YOUR SAVE IN THE GAME IS SEPARATE AND STILL WORKS AS NORMAL." });
    l.push_back({ nullptr, "RESET GAME / QUIT", {}, "", "IN THE PAUSE MENU: START THE GAME AGAIN FROM ITS TITLE SCREEN / BACK TO THIS GAME LIST." });
    return l;
}

// Words to lines of at most `chars` characters.
inline std::vector<std::string> Wrap(const std::string& text, int chars)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (!line.empty() && (int)(line.size() + 1 + word.size()) > chars)
        {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char ch : text)
        if (ch == ' ') flush();
        else word += ch;
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// The settings page's groups: first the list of them, then one group's options (Enter, Back).
struct SettingsGroup
{
    const char* key;  // its row in the list of groups (a "!" key: chosen, not stored)
    const char* name;
    const char* text;
};
inline const std::vector<SettingsGroup>& SettingsGroups()
{
    static const std::vector<SettingsGroup> g = {
        // (What each holds is listed under it: its options' names, ShowGroups.)
        { "!group0", "DISPLAY", "" },
        { "!group1", "GRAPHICS", "" },
        { "!group2", "CONTROLS", "" },
        { "!group3", "SHORTCUTS", "" },
        { "!group4", "HELP", "" },
    };
    return g;
}
// Which group an option is in (SettingsGroups).
inline int GroupOf(const Setting& s)
{
    if (!s.key) return !strcmp(s.name, "KEYS AND BUTTONS") ? 2 : 4;
    static const char* display[] = { "view", "large_screen", "fullscreen", "smooth", "battle_buttons", "show_fps" };
    static const char* controls[] = { "start_button", "buttons", "pad_touch" };
    for (const char* k : display)
        if (!strcmp(s.key, k)) return 0;
    for (const char* k : controls)
        if (!strcmp(s.key, k)) return 2;
    if (!strcmp(s.key, "!keys")) return 2;
    if (s.key[0] == '!') return 3; // (DESKTOP SHORTCUTS, HOME MENU ICONS)
    return 1;
}

struct SettingsPage
{
    std::string path;
    std::vector<Setting> all = SettingsList();
    std::vector<Setting> items; // what is shown: the groups, or one group's options
    int group = -1;             // the group shown, -1: the list of groups
    std::string groupText[8];   // (the groups' lines: their options' names)
    std::vector<std::string> lines; // the file, as it is
    int selected = 0, scroll = 0;
    // Draw keeps the chosen option in view while follow is set (the keys and the D-pad); a
    // finger scrolling the page clears it. Draw also notes, for taps (ItemAt), where each
    // option's name line is and how tall a line is.
    bool follow = true;
    int lineH = 20;
    std::vector<std::pair<int, int>> rows; // (option, the top of its name line)

    // The option whose part of the page (its name and the lines below it) is at y, or -1.
    int ItemAt(int y) const
    {
        int found = -1;
        for (const auto& r : rows)
            if (r.second - lineH / 4 <= y) found = r.first;
        if (found >= 0 && !rows.empty() && y > rows.back().second + 6 * lineH) return -1;
        return found;
    }

    void Open(const std::string& cfgPath, bool keepChoice = false)
    {
        path = cfgPath;
        lines.clear();
        if (FILE* f = fopen(path.c_str(), "r"))
        {
            char buf[1024];
            while (fgets(buf, sizeof buf, f))
            {
                std::string l = buf;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                lines.push_back(l);
            }
            fclose(f);
        }
        if (keepChoice) return;
        ShowGroups(0);
    }
    // The list of groups, with group `sel` chosen.
    void ShowGroups(int sel)
    {
        group = -1;
        items.clear();
        int row = 0;
        for (int g = 0; g < (int)SettingsGroups().size(); g++)
        {
            bool any = false;
            for (const Setting& s : all) any |= GroupOf(s) == g;
            if (!any) continue; // (a platform without any of it: no SHORTCUTS on a Mac)
            if (g == sel) row = (int)items.size();
            const SettingsGroup& sg = SettingsGroups()[g];
            std::string names;
            for (const Setting& s : all)
                if (GroupOf(s) == g) names += (names.empty() ? "" : " / ") + std::string(s.name);
            groupText[g] = names + ".";
            items.push_back({ sg.key, sg.name, { { "", ">" } }, "", groupText[g].c_str() });
        }
        selected = row;
        scroll = 0;
        follow = true;
    }
    // Group g's options.
    void Enter(int g)
    {
        group = g;
        items.clear();
        for (const Setting& s : all)
            if (GroupOf(s) == g) items.push_back(s);
        selected = 0;
        scroll = 0;
        follow = true;
    }
    // B: from a group back to the list of groups; false when already there (the page closes).
    bool Back()
    {
        if (group < 0) return false;
        ShowGroups(group);
        return true;
    }
    std::string Stored(const std::string& key) const
    {
        for (const std::string& l : lines)
            if (l.size() > key.size() && !l.compare(0, key.size(), key) && l[key.size()] == '=') return l.substr(key.size() + 1);
        return "";
    }
    // The index of the shown value (-1: AUTO).
    int ValueIndex(const Setting& s) const
    {
        if (s.key[0] == '!') return 0; // (something to do: its one value is what it says)
        std::string v = Stored(s.key);
        if (v.empty()) v = s.def;
        if (v.empty()) return -1;
        for (size_t i = 0; i < s.values.size(); i++)
            if (s.values[i].first == v) return (int)i;
        // Values the file may hold in other spellings: numbers between the choices (the
        // nearest below), ON/OFF as anything but "0" or "1".
        if (!strcmp(s.key, "wide_lines"))
        {
            int best = 0;
            for (size_t i = 0; i < s.values.size(); i++)
                if (atoi(s.values[i].first.c_str()) <= atoi(v.c_str())) best = (int)i;
            return best;
        }
        for (size_t i = 0; i < s.values.size(); i++)
            if (s.values[i].first == s.def) return (int)i;
        return 0;
    }
    std::string Shown(const Setting& s) const
    {
        const int i = ValueIndex(s);
        return i < 0 ? "AUTO" : s.values[i].second;
    }
    void Move(int d)
    {
        follow = true;
        if (selected + d >= 0 && selected + d < (int)items.size()) selected += d;
    }
    // The chosen option's next (d 1) or previous (d -1) value, written to the file. Returns its
    // key, or nullptr.
    const char* Change(int d)
    {
        if (selected < 0 || selected >= (int)items.size() || !items[selected].key) return nullptr;
        const Setting& s = items[selected];
        if (!strncmp(s.key, "!group", 6))
        {
            if (d > 0) Enter(atoi(s.key + 6));
            return nullptr;
        }
        if (s.key[0] == '!') return s.key; // (done by the caller)
        const int n = (int)s.values.size();
        const int i = ValueIndex(s);
        const int next = i < 0 ? (d > 0 ? 0 : n - 1) : (i + d + n) % n;
        const std::string line = std::string(s.key) + "=" + s.values[next].first;
        bool found = false;
        for (std::string& l : lines)
            if (l.size() > strlen(s.key) && !l.compare(0, strlen(s.key), s.key) && l[strlen(s.key)] == '=')
            {
                l = line;
                found = true;
            }
        if (!found) lines.push_back(line);
        if (FILE* f = fopen(path.c_str(), "w"))
        {
            for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
        return s.key;
    }

    // Over the whole canvas: the options with their values (the chosen one marked), each with
    // what it does below it, kept in view as the choice moves.
    void Draw(Canvas& c, const std::string& hint, const Theme& th = Theme())
    {
        const int s = std::max(1, c.h / 240);
        const int ts = std::max(1, (s * 2 + 1) / 3); // the text's scale: 2 at 720 lines, 3 at 1080
        c.Fill(0, 0, c.w, c.h, th.bg);
        c.Fill(0, 0, c.w, 22 * s, th.band);
        c.Fill(0, 22 * s, c.w, th.bandLineH * s, th.bandLine);
        const std::string title = group < 0 ? "SETTINGS" : std::string("SETTINGS: ") + SettingsGroups()[group].name;
        c.Text((c.w - Canvas::TextWidth(title, s + 1)) / 2, 6 * s, title, s + 1, th.title);
        const int width = std::min(c.w - 24 * s, 100 * 6 * ts);
        const int x = (c.w - width) / 2;
        struct Line { std::string text, value; int item; bool name; };
        std::vector<Line> out;
        int selFirst = 0, selLast = 0;
        bool infoHeading = false;
        for (int i = 0; i < (int)items.size(); i++)
        {
            const Setting& e = items[i];
            if (!e.key && !infoHeading)
            {
                out.push_back({ "", "", -1, false });
                infoHeading = true;
            }
            if (i == selected) selFirst = (int)out.size();
            out.push_back({ e.name, e.key ? Shown(e) : "", i, true });
            for (const std::string& l : Wrap(e.text, width / (6 * ts) - 2)) out.push_back({ l, "", i, false });
            if (i == selected) selLast = (int)out.size() - 1;
            out.push_back({ "", "", -1, false });
        }
        const int top = 30 * s, bottom = c.h - 18 * s;
        lineH = 10 * ts;
        const int fit = std::max(1, (bottom - top) / lineH);
        if (follow)
        {
            if (selFirst < scroll) scroll = selFirst;
            if (selLast >= scroll + fit) scroll = selLast - fit + 1;
        }
        scroll = std::max(0, std::min(scroll, (int)out.size() - fit));
        rows.clear();
        int y = top;
        for (int i = scroll; i < (int)out.size() && y + lineH <= bottom; i++, y += lineH)
        {
            const Line& l = out[i];
            if (l.name) rows.push_back({ l.item, y });
            const bool sel = l.item == selected && l.item >= 0;
            if (l.name)
            {
                if (sel) c.Fill(x - 4 * ts, y - 2 * ts, width + 8 * ts, lineH, th.tile);
                const bool info = !items[l.item].key;
                c.Text(x, y, l.text, ts, info ? th.hint : sel ? th.nameSel : th.subSel);
                if (!l.value.empty())
                {
                    const bool group = !strncmp(items[l.item].key, "!group", 6);
                const std::string v = sel && !group ? "< " + l.value + " >" : l.value;
                    c.Text(x + width - Canvas::TextWidth(v, ts), y, v, ts, sel ? th.nameSel : th.name);
                }
            }
            else if (!l.text.empty())
                c.Text(x + 2 * 6 * ts, y, l.text, ts, sel ? th.name : th.sub);
        }
        if (scroll > 0) c.Fill(c.w / 2 - 8 * s, top - 4 * s, 16 * s, s, th.hint);
        if (scroll + fit < (int)out.size()) c.Fill(c.w / 2 - 8 * s, bottom + s, 16 * s, s, th.hint);
        c.Text((c.w - Canvas::TextWidth(hint, s)) / 2, c.h - 12 * s, hint, s, th.hint);
    }
};

}
