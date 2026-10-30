// Pokemon Black and White (US/Europe, IRBO and IRAO): their own screen layouts, on top of the
// Black 2 ones.
//
// Black runs like Black 2 (melonDS's JIT, layouts that follow the game's own functions), with
// a function table made by tools/bw1/funcmap.py from the aletheia decompilation. The functions
// the Black 2 layouts watch carry their Black 2 names in that table (tools/bw1/aliases.txt), so
// pickSceneBw2 in main.cpp picks the scenes for Black too. Where Black's screens are drawn
// differently, Bw1Layouts::Adjust changes the scene it picked (by the scene's name), and a few
// screens Black 2 does not have get scenes of their own (by Black's function names). For any
// other game it does nothing. White is the same program as Black, its functions a few bytes
// apart (tools/bw1/funcmap.py --white): everything here is for both.
//
// Hooked into main.cpp in two places: Init before the function watch is attached (so Black's own
// functions are watched too) and Adjust on the scene pickSceneBw2 chose.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "funcwatch.h"
#include "merged.h"

class Bw1Layouts
{
public:
    bool on = false;

    // The battle's buttons on a wide view (BATTLE BUTTONS in the pause menu, when the shared code
    // has that row; ssrecomp.cfg key battle_buttons=strip|panel|corner):
    //   STRIP:  large buttons in the message box's band, like SoulSilver's (BattleStrip);
    //   PANEL:  the touch screen's own panel, as it looks, scaled to the message box's height
    //           at its right end;
    //   CORNER: that panel in the lower right corner, larger, standing above the box (Black 2's).
    enum { BUTTONS_CORNER, BUTTONS_STRIP, BUTTONS_PANEL };
    int battleButtons = BUTTONS_STRIP;

    // romPath: the game being started; names: the functions the layouts watch (Black's own
    // are added to it when the game is Black); configPath: ssrecomp.cfg, for battle_buttons
    // until the shared settings know it.
    void Init(const std::string& romPath, const FuncWatch& fw, std::vector<std::string>& names,
        const std::string& configPath = "")
    {
        if (FILE* f = configPath.empty() ? nullptr : fopen(configPath.c_str(), "r"))
        {
            char line[128];
            while (fgets(line, sizeof(line), f))
                if (!strncmp(line, "battle_buttons=", 15))
                    battleButtons = !strncmp(line + 15, "corner", 6) ? BUTTONS_CORNER
                        : !strncmp(line + 15, "panel", 5) ? BUTTONS_PANEL : BUTTONS_STRIP;
            fclose(f);
        }
        char code[5] = {};
        if (FILE* f = fopen(romPath.c_str(), "rb"))
        {
            if (fseek(f, 0x0C, SEEK_SET) != 0 || fread(code, 1, 4, f) != 4) code[0] = 0;
            fclose(f);
        }
        on = !strcmp(code, "IRBO") || !strcmp(code, "IRAO") || !strcmp(code, "IRBF") || !strcmp(code, "IRAF");
        if (!on) return;
        Fw = &fw;
        auto find = [&](const char* name) {
            names.push_back(name);
            return fw.Find(name);
        };
        // The trainer card's other side, the Badge Case (gym leaders' portraits and the badges,
        // all on the touch screen; the top screen only has the League's emblem), and editing the
        // card (the signature): TRCARD_SYS runs one of these every frame while its page is up.
        badges = find("TRCARD_SYS_StateWaitBadgeProc");
        cardEdit = find("TRCARD_SYS_StateWaitEditProc");
        // SAVE in the X menu: the question, the save's card and YES / NO are all on the touch
        // screen (Black 2 asks on the top screen).
        report = find("REPORT_EVENT_Main");
        // A Pokemon registered in the Pokedex (caught, hatched, received): its page on the top
        // screen; the other only holds a dimmed Poke Ball, until the battle's touch screen
        // asks YES / NO ("Give a nickname?"), which is polled by BTLV_INPUT_ProcessTouchInput.
        dexRegister = find("ZUKAN_TOROKU_GRAPHIC_Main");
        touchInput = find("BTLV_INPUT_ProcessTouchInput");
        // The overworld's frame and the battle's two touch-screen menus (not watched again here:
        // the shared table already does).
        field = fw.Find("FieldmapProc_Update");
        commandMenu = fw.Find("BtlvCore_WaitCommandSelect");
        moveMenu = fw.Find("BtlvCore_WaitMoveSelect");
        targetMenu = fw.Find("BtlvCore_WaitTargetSelect");
        fprintf(stderr, "[bw1] Pokemon Black: badges %d, card edit %d, save %d\n", badges, cardEdit, report);
    }

    void Adjust(Scene& sc, float viewWidth, const ScreenImage* screens)
    {
        if (!on) return;
        // (The player's choice of screen, and the C-Gear's question answered unseen, stay.)
        if (!strcmp(sc.name, "manual") || !strcmp(sc.name, "continue")) return;
        const bool wideView = viewWidth >= 300;
        const float viewLeft = 128 - viewWidth / 2, viewRight = 128 + viewWidth / 2;
        // (It skips a few frames as it opens; and while the save is written, for a second or
        // two, the game does nothing else, the overworld included: the save stays until the
        // overworld runs again.)
        if (Fw->Ran(report, 10) || (saving && !Fw->Ran(field, 1)))
        {
            // Laid out as Black 2's (merged.h, SaveScreenPieces): the overworld, without the
            // strip on its bottom that points at the other screen, the message box along the
            // bottom and the rest of the touch screen in the upper right corner.
            saving = true;
            const bool weather = sc.weatherObj;
            Only(sc, "save", 0);
            sc.wide = wideView;
            sc.weatherObj = weather;
            sc.items[0].drop = LAYER_BG1;
            SaveScreenPieces(sc, wideView, viewRight, screens);
            return;
        }
        saving = false;
        if (Fw->Ran(dexRegister, 2))
        {
            if (Fw->Ran(touchInput, 2))
                sc = Stack("dex-register-ask", { Crop(0, 0, 0, 256, 192), Crop(1, 0, 0, 256, 192) }, true, viewWidth);
            else
            {
                Only(sc, "dex-register", 0);
                sc.softBars = true;
            }
            return;
        }
        if (Fw->Ran(badges, 2) || Fw->Ran(cardEdit, 2))
        {
            Only(sc, "trainer-card", 1);
            return;
        }
        if (!strcmp(sc.name, "launch-menu"))
        {
            // Black's start menu (CONTINUE with the saved game's card, then NEW GAME, MYSTERY
            // GIFT, ... in a list) is all on the top screen; the touch screen is empty. The list
            // goes on below the screen (the DS shows its fourth entry, BATTLE COMPETITION, cut
            // off; there are eight in all). On a wide view the card (x 24-229, rows 15-127) and
            // the list (x 32-223, rows 128-296, drawn on below the screen's last line:
            // frontend/belowscreen.h) go side by side across the whole view, on the dark grey of
            // the menu's own background, without the Poke Ball and stripes behind them (BG3, also
            // left out of the buttons it shows through: Scene::redrawSkip). Otherwise the top
            // screen alone, with soft bars.
            // (With no saved game there is no card: the list alone, NEW GAME first, rows 15-110,
            // nothing on the screen below it but the background.)
            bool listBelow = false;
            if (screens && screens[0].flags)
                for (int y = 135; y < 186 && !listBelow; y += 2)
                    for (int x = 40; x < 216 && !listBelow; x += 4)
                    {
                        const uint8_t c = LayerClass(screens[0].flags[y * 256 + x]);
                        listBelow = c != LAYER_BACKDROP && c != LAYER_BG3;
                    }
            if (wideView && !listBelow)
            {
                Element list = Crop(0, 32, 14, 194, 98);
                list.drop = LAYER_BACKDROP;
                sc = CleanMenu("launch-menu", { list }, viewWidth, 0x1c1c1c);
                sc.redrawSkip[0] = 1 << 3;
                return;
            }
            if (wideView)
            {
                Element card = Crop(0, 24, 15, 206, 113), list = Crop(0, 32, 128, 192, 169);
                card.drop = list.drop = LAYER_BACKDROP;
                sc = CleanMenu("launch-menu", { card, list }, viewWidth, 0x1c1c1c);
                sc.extraRows[0] = 296 + 1 - 192;
                sc.redrawSkip[0] = 1 << 3;
                sc.redrawUnscroll[0] = 2; // (the list scrolls as the cursor goes down it)
                return;
            }
            Only(sc, "launch-menu", 0);
            sc.softBars = true;
            return;
        }
        if (!strcmp(sc.name, "battle"))
        {
            const bool many = BattleBoxes(sc, viewLeft, viewRight);
            // (A double, triple or rotation battle has the player's boxes down to the message
            // box, where the corner panel would stand: there the panel takes the box's band.)
            const int buttons = many && battleButtons == BUTTONS_CORNER ? BUTTONS_PANEL : battleButtons;
            if (buttons != BUTTONS_CORNER && wideView && screens)
            {
                const bool moves = Fw->Ran(moveMenu, 1), command = !moves && Fw->Ran(commandMenu, 1);
                // (Choosing a move's target, in a double, triple or rotation battle: the foes'
                // buttons above, the player's side below, touch screen rows 32-134.)
                const bool target = !moves && !command && Fw->Ran(targetMenu, 1);
                if (moves || command || target)
                {
                    // The panel pickSceneBw2 put in the corner (a bordered backing and a piece
                    // of the touch screen) goes; the strip or the smaller panel takes its place.
                    std::vector<Element> keep;
                    for (const Element& e : sc.items)
                        if (e.screen != 1 && !(e.screen == -1 && e.border)) keep.push_back(e);
                    sc.items = keep;
                    if (buttons == BUTTONS_STRIP && !target)
                        BattleStrip(sc, viewRight, screens[1].pixels, moves);
                    else
                    {
                        // The same pieces as the corner panel (pickSceneBw2's: the four moves,
                        // or FIGHT above BAG, RUN and POKEMON; the targets, also in STRIP), as
                        // tall as the message box (rows 147-189), against the view's right edge.
                        Element c = moves ? Crop(1, 0, 30, 256, 98) : target ? Crop(1, 0, 32, 256, 103)
                            : Crop(1, 0, 78, 256, 114);
                        c.scale = 43.0f / c.sh;
                        c.dx = viewRight - 2 - c.sw * c.scale;
                        c.dy = 147;
                        c.border = true;
                        sc.items.push_back(c);
                    }
                }
            }
            return;
        }
        if (!strcmp(sc.name, "pokedex-list"))
        {
            // Black's Pokedex list is on the touch screen; the top one only has the title and
            // the SEEN / OBTAINED counts (rows 0-50) above an empty grid (Black 2's list runs on
            // from one screen into the other, which pickSceneBw2 shows as one column). The
            // counts above the list, as large as fits the view's height.
            sc = Stack("pokedex-list", { Crop(0, 0, 0, 256, 51), Crop(1, 0, 0, 256, 192) }, false, viewWidth);
            for (Element& el : sc.items) el.crisp = true;
            return;
        }
        if (!strcmp(sc.name, "start-menu") && screens)
        {
            XMenu(sc, wideView, viewWidth, screens);
            return;
        }
    }

private:
    const FuncWatch* Fw = nullptr;
    int badges = -1, cardEdit = -1, report = -1, field = -1, dexRegister = -1, touchInput = -1;
    int commandMenu = -1, moveMenu = -1, targetMenu = -1;
    bool saving = false; // the save's screen was shown last

    static void Only(Scene& sc, const char* name, int screen)
    {
        Scene n;
        n.name = name;
        n.smartEdges = sc.smartEdges;
        n.items = { FullScreen(screen) };
        sc = n;
    }

    // A battle on a wide view (pickSceneBw2 has made holes for Black 2's HP boxes and put them
    // against the view's edges). Black's boxes are drawn against the screen's edges and stay
    // against those edges, now the view's. In a single battle the foe's runs along the left edge
    // (x 0-123, y 30-48), its bars running off it, and the player's along the right edge
    // (x 136-255, y 110-138): the foe's goes to the view's left, 4 pixels higher, the player's to
    // its right, 10 pixels higher (still in the lower half), so that it clears the buttons in the
    // lower right corner. In a double, triple or rotation battle (pickSceneBw2's foes' hole starts
    // at row 4) the foes' boxes are stacked down the left edge (a double's x 0-126, y 18-62; a
    // triple's centred on rows 19, 40 and 61) and the player's down the right edge (a double's
    // x 136-255, y 90-146, just above the message box): they keep their rows. Returns whether
    // the battle has more than one Pokemon a side.
    static bool BattleBoxes(Scene& sc, float viewLeft, float viewRight)
    {
        if (sc.items.empty() || sc.items[0].holeCount != 2) return false; // no boxes moved
        Element& top = sc.items[0];
        // pickSceneBw2's pieces: the foes' first, then the player's. Their sources tell the kind
        // of battle (the foes' piece starts at row 4 with more than one Pokemon a side) and
        // whether the level-up window is open (the player's starts at row 20); a side whose
        // boxes are put away (a Poke Ball being thrown) has no hole and a piece of no size, and
        // stays so.
        Element* piece[2] = {};
        for (Element& e : sc.items)
            if (e.wideOnly && !piece[1]) (piece[0] ? piece[1] : piece[0]) = &e;
        if (!piece[1]) return false;
        const bool many = piece[0]->sy == 4, levelUp = piece[1]->sy == 20;
        const bool shown[2] = { top.holes[0][2] > 0, top.holes[1][2] > 0 };
        static const int enemy1[4] = { 0, 26, 128, 54 }, mine1[4] = { 132, 106, 256, 142 };
        const int enemyN[4] = { 0, 8, 128, 72 }, mineN[4] = { 134, 82, 256, 147 };
        const int* enemy = many ? enemyN : enemy1;
        int mine[4];
        std::copy(many ? mineN : mine1, (many ? mineN : mine1) + 4, mine);
        // While the level-up window is open (over the player's box, inside its columns) it moves
        // with the box, from row 20: cut through at the box's top, its last rows went right with
        // the box and the rest stayed.
        if (levelUp) mine[1] = 20;
        std::copy(enemy, enemy + 4, top.holes[0]);
        std::copy(mine, mine + 4, top.holes[1]);
        for (int i = 0; i < 2; i++)
            if (!shown[i]) std::fill(top.holes[i], top.holes[i] + 4, 0);
        for (int n = 0; n < 2; n++)
        {
            Element& e = *piece[n];
            if (!shown[n]) continue;
            const int* r = n == 0 ? enemy : mine;
            e.sx = r[0];
            e.sy = r[1];
            e.sw = r[2] - r[0];
            e.sh = r[3] - r[1];
            if (n == 0)
            {
                e.dx = viewLeft;
                e.dy = enemy[1] - (many ? 0 : 4);
            }
            else
            {
                e.dx = viewRight - e.sw;
                e.dy = mine[1] - (many ? 0 : 10);
            }
        }
        return many;
    }

    // The battle's buttons as a strip in the lower right of a wide view, below the player's HP
    // box, in the message box's band right of its text ("What will" and a ten-letter name end
    // before x 150), like SoulSilver's: on
    // the command menu FIGHT across the top and BAG, RUN, POKEMON below it; on the move menu the
    // four moves two by two. Each is cut from the touch screen (Black's buttons are octagons,
    // so the cut takes their middle: the label, the move's type and PP) and framed; the one the
    // game's cursor (magenta brackets, 0xFB0092, at its corners) is on gets a magenta frame. The
    // D-pad moves the cursor as it looks: the cells are where the buttons are on the DS.
    static void BattleStrip(Scene& sc, float viewRight, const uint32_t* px, bool moves)
    {
        struct Cell { int sx, sy, sw, sh; int hit[4]; float dx, dy, scale; }; // hit: the button's area
        // Lined up with the message box (rows 147-189 of the top screen, a band across it):
        // the buttons fill its rows, 2 pixels in, out to the view's right edge, on the box itself.
        // One scale for all the pieces of a menu.
        const float boxTop = 147, boxBottom = 190, gap = 2, right = viewRight - 2;
        const float top = boxTop + 2, bottom = boxBottom - 1, height = bottom - top;
        std::vector<Cell> cells;
        float width;
        if (!moves)
        {
            // Touch-screen pieces (x, y, w, h) and the buttons' areas (x0, y0, x1, y1): FIGHT's
            // middle (its label) above BAG, RUN and POKEMON.
            const float sc1 = (height - gap) / (23 + 36), cw = 68 * sc1;
            width = 3 * cw + 2 * gap;
            const float left = right - width, rowB = bottom - 36 * sc1;
            cells.push_back({ 23, 77, 210, 23, { 16, 40, 240, 128 }, left, top, sc1 });                 // FIGHT
            cells.push_back({ 6, 146, 68, 36, { 0, 128, 84, 192 }, left, rowB, sc1 });                 // BAG
            cells.push_back({ 94, 152, 68, 36, { 84, 140, 172, 192 }, left + cw + gap, rowB, sc1 });   // RUN
            cells.push_back({ 182, 146, 68, 36, { 172, 128, 256, 192 }, left + 2 * (cw + gap), rowB, sc1 }); // POKEMON
        }
        else
        {
            // Each move's name, type and PP (the middle of its button), two by two.
            const float ms = (height - gap) / (2 * 32), cw = 128 * ms;
            width = 2 * cw + gap;
            const float left = right - width;
            for (int i = 0; i < 4; i++)
            {
                const int c = i & 1, r = i >> 1;
                cells.push_back({ c * 128, 40 + r * 48, 128, 32, { c * 128, 30 + r * 50, c * 128 + 128, 80 + r * 50 },
                    left + c * (cw + gap), r ? bottom - 32 * ms : top, ms });
            }
        }
        const float left = right - width;
        // Which cell the cursor is on: its brackets' outline (the box around all magenta
        // pixels). FIGHT is the whole upper part of the screen, so its brackets span most of
        // it; the others are the button nearest the brackets' middle.
        int x0 = 256, y0 = 192, x1 = -1, y1 = -1;
        for (int y = 16; y < 192; y++)
            for (int x = 0; x < 256; x++)
                if ((px[y * 256 + x] & 0xFFFFFF) == 0xFB0092)
                {
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        int best = -1;
        if (x1 >= 0)
        {
            const float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
            if (!moves && y1 - y0 >= 90) best = 0;
            else
            {
                float bestD = 1e9f;
                for (size_t i = moves ? 0 : 1; i < cells.size(); i++)
                {
                    const int* h = cells[i].hit;
                    const float dx = cx - (h[0] + h[2]) / 2.0f, dy = cy - (h[1] + h[3]) / 2.0f;
                    if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = (int)i; }
                }
            }
        }
        // (No backing of its own: the buttons sit on the message box, seamless with it.)
        for (size_t i = 0; i < cells.size(); i++)
        {
            const Cell& c = cells[i];
            Element e = Crop(1, c.sx, c.sy, c.sw, c.sh);
            e.dx = c.dx;
            e.dy = c.dy;
            e.scale = c.scale;
            e.border = true;
            e.borderRadius = 2;
            if ((int)i == best)
            {
                e.borderColor = 0xFB0092;
                e.borderWidth = 10;
            }
            sc.items.push_back(e);
        }
    }

    // The X menu. Black's buttons are 123 x 41 on a light grid, two to a row, rows 48 apart
    // from y 24 (x 3 and 131); a slot is used when most of its middle row is not the light
    // grid. They go in a panel at the right edge in the same grid as Black 2's (pickSceneBw2
    // measured Black 2's buttons, which are larger and elsewhere).
    static void XMenu(Scene& sc, bool wideView, float viewWidth, const ScreenImage* screens)
    {
        if (sc.items.empty()) return;
        sc.items.resize(1); // the overworld (and its settings) as pickSceneBw2 chose them
        struct Slot { Element crop; int row, col; };
        std::vector<Slot> buttons;
        int rows = 0;
        const uint32_t* px = screens[1].pixels;
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 2; col++)
            {
                const int bx = col ? 131 : 3, by = 24 + row * 48;
                int dark = 0;
                for (int x = bx; x < bx + 123; x++)
                {
                    const uint32_t c = px[(by + 20) * 256 + x];
                    const int lum = (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) / 3;
                    if (lum <= 150) dark++;
                }
                // (And something light on it, its icon or name: while the menu opens its buttons
                // are empty dark boxes for a few frames, which showed as a grid of black boxes.)
                int light = 0;
                for (int y = by + 8; y < by + 35; y += 2)
                    for (int x = bx + 8; x < bx + 115; x += 2)
                    {
                        const uint32_t c = px[y * 256 + x];
                        if ((((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) / 3 > 200) light++;
                    }
                if (dark * 10 >= 123 * 3 && light >= 8)
                {
                    buttons.push_back({ Crop(1, bx, by, 123, 41), row, col });
                    rows = row + 1;
                }
            }
        if (buttons.empty()) return; // opening or closing: the overworld alone
        const float scale = wideView ? 0.46f : 0.41f, gap = 2, pad = 3;
        const float bw = 123 * scale, bh = 41 * scale;
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
            // (The grid's white and its lines show above the right part of each button, beside
            // the tab at its top left: not drawn, within a few pixels of the piece's edge.)
            b.crop.keys[0] = 0xFBFBFB;
            b.crop.keys[1] = 0xD3D3D3;
            b.crop.keyRing = 7;
            sc.items.push_back(b.crop);
        }
    }
};
