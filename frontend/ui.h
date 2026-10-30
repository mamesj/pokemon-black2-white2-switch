// Software-drawn overlay: pause menu, touch cursor and status messages.
// Drawn into an ARGB buffer the size of the output, so the window and the
// headless screenshots show the same thing.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct Canvas
{
    int w = 0, h = 0;
    std::vector<uint32_t> px; // ARGB; alpha 0 means "show the game underneath"

    void Resize(int nw, int nh)
    {
        if (nw != w || nh != h)
        {
            w = nw;
            h = nh;
            px.assign((size_t)w * h, 0);
            x0d = y0d = 0;
            x1d = w;
            y1d = h;
        }
    }
    // The rectangle that has been drawn into since the last Clear. Only that part is
    // cleared and sent to the screen: the canvas is as large as the window.
    int x0d = 0, y0d = 0, x1d = 0, y1d = 0;
    bool Dirty() const { return x1d > x0d && y1d > y0d; }
    void Clear()
    {
        for (int yy = y0d; yy < y1d; yy++)
            std::fill(px.begin() + (size_t)yy * w + x0d, px.begin() + (size_t)yy * w + x1d, 0);
        x0d = w;
        y0d = h;
        x1d = y1d = 0;
    }

    void Fill(int x, int y, int rw, int rh, uint32_t argb)
    {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + rw > w ? w : x + rw, y1 = y + rh > h ? h : y + rh;
        if (x1 <= x0 || y1 <= y0) return;
        if (x0 < x0d) x0d = x0;
        if (y0 < y0d) y0d = y0;
        if (x1 > x1d) x1d = x1;
        if (y1 > y1d) y1d = y1;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++)
                px[(size_t)yy * w + xx] = argb;
    }

    // 5x7 glyphs, one row per byte, bit 4 is the leftmost pixel.
    static const uint8_t* Glyph(char c)
    {
        static const uint8_t font[][7] = {
            { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // A
            { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, // B
            { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, // C
            { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, // D
            { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, // E
            { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, // F
            { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, // G
            { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // H
            { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, // I
            { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, // J
            { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, // K
            { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, // L
            { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, // M
            { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, // N
            { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // O
            { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, // P
            { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, // Q
            { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, // R
            { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, // S
            { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, // T
            { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // U
            { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, // V
            { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 }, // W
            { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, // X
            { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, // Y
            { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, // Z
            { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, // 0
            { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, // 1
            { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, // 2
            { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E }, // 3
            { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, // 4
            { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, // 5
            { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, // 6
            { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, // 7
            { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, // 8
            { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, // 9
            { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 }, // :
            { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, // -
            { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 }, // +
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06 }, // .
            { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 }, // /
            { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 }, // <
            { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, // >
            { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 }, // !
            { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 }, // (
            { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 }, // )
            // PlayStation's button symbols (controls.h, PS_*): cross, circle, square, triangle. Smaller
            // than the letters, so the cross and the circle are not taken for an X and an O.
            { 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 },
            { 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00 },
            { 0x00, 0x1F, 0x11, 0x11, 0x11, 0x1F, 0x00 },
            { 0x00, 0x04, 0x0A, 0x0A, 0x11, 0x1F, 0x00 },
            { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 }, // % (VOLUME)
        };
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') return font[c - 'A'];
        if (c >= '0' && c <= '9') return font[26 + c - '0'];
        static const char punct[] = ":-+./<>!()\x01\x02\x03\x04%";
        const char* p = c ? strchr(punct, c) : nullptr;
        return p ? font[36 + (p - punct)] : nullptr;
    }

    static int TextWidth(const std::string& s, int scale) { return (int)s.size() * 6 * scale; }

    void Text(int x, int y, const std::string& s, int scale, uint32_t argb)
    {
        for (char c : s)
        {
            // \x05: E with an acute accent (the É of POKÉMON): a whole E, the accent above the
            // line, two pixels slanting up to the right with a row's gap.
            if (c == '\x05')
            {
                Fill(x + 3 * scale, y - 3 * scale, scale, scale, argb);
                Fill(x + 2 * scale, y - 2 * scale, scale, scale, argb);
                c = 'E';
            }
            if (const uint8_t* g = Glyph(c))
                for (int row = 0; row < 7; row++)
                    for (int col = 0; col < 5; col++)
                        if (g[row] & (0x10 >> col))
                            Fill(x + col * scale, y + row * scale, scale, scale, argb);
            x += 6 * scale;
        }
    }
};

// The accent colours of COLOUR (the game list's settings, GAME LIST): the game list's lines and
// frame (launcher.h, ThemeNamed) and the pause menu's chosen row, its box and text shaded from it.
inline uint32_t AccentNamed(const std::string& name)
{
    static const struct { const char* name; uint32_t argb; } colours[] = {
        { "red", 0xFFE0242C }, { "blue", 0xFF2A7BE0 }, { "green", 0xFF2EB85C }, { "purple", 0xFF9B4DE0 },
        { "gold", 0xFFE0A82C }, { "pink", 0xFFE0479A }, { "teal", 0xFF1FB5A8 },
    };
    for (const auto& c : colours)
        if (name == c.name) return c.argb;
    return colours[0].argb;
}

// Button presses the menu understands, edge-triggered.
struct MenuInput
{
    bool up = false, down = false, left = false, right = false, accept = false, back = false;
};

struct MenuItem
{
    std::string label;
    std::string value; // shown right-aligned; empty for plain actions
    bool info = false; // the value is only shown (no < > around it): nothing to change here
    int slider = -1;   // 0 to 10: also drawn as a bar of ten blocks, that many lit (VOLUME)
};

struct Menu
{
    bool open = false;
    int selected = 0;
    std::vector<MenuItem> items;

    // Returns the index of an item that was activated this frame, or -1.
    // dir is -1/+1 when left/right was used on it, 0 for accept.
    int Update(const MenuInput& in, int& dir)
    {
        dir = 0;
        if (!open || items.empty()) return -1;
        int n = (int)items.size();
        if (in.up) selected = (selected + n - 1) % n;
        if (in.down) selected = (selected + 1) % n;
        if (in.accept) return selected;
        if (in.left && !items[selected].value.empty())
        {
            dir = -1;
            return selected;
        }
        if (in.right && !items[selected].value.empty())
        {
            dir = 1;
            return selected;
        }
        return -1;
    }

    // Where the box and its rows were last drawn (Draw), in the canvas's pixels, and the first
    // row shown (a list longer than the screen scrolls with the chosen row: CHEATS).
    mutable int drawnX = 0, drawnY = 0, drawnW = 0, drawnH = 0, drawnLineH = 0, drawnFirstY = 0;
    mutable int scroll = 0;
    // Where a slider's bar was drawn (the same on every row): its left edge and one block's step.
    mutable int barX = 0, barStep = 0;

    // The value a tap at x on a slider's row stands for (0 to 10: the block tapped and those
    // before it; left of the bar, 0), or -1 for a tap beside the bar.
    int SliderAt(int x) const
    {
        if (barStep <= 0 || x >= barX + 10 * barStep + barStep / 2) return -1;
        if (x < barX) return x >= barX - 2 * barStep ? 0 : -1;
        return std::min(10, (x - barX) / barStep + 1);
    }

    // The row at (x, y) of the canvas the menu was drawn on (touch screens): its index, -1 for
    // a point in the box but on no row, -2 for a point outside the box.
    int ItemAt(int x, int y) const
    {
        if (drawnW <= 0 || x < drawnX || x >= drawnX + drawnW || y < drawnY || y >= drawnY + drawnH) return -2;
        if (drawnLineH <= 0 || y < drawnFirstY) return -1;
        const int i = scroll + (y - drawnFirstY) / drawnLineH;
        return i < (int)items.size() ? i : -1;
    }

    // note: a few lines of text in a strip under the box (CHEATS: what the database says about
    // the chosen cheat), wrapped to the box's width.
    // The chosen row's colour (COLOUR, AccentNamed); the box, text and bars are shaded from it.
    static inline uint32_t Accent = 0xFFE0242C;

    void Draw(Canvas& c, const std::string& title, const std::string& hint, const std::string& note = "") const
    {
        // (Each channel of the accent c: f(c) per part. With blue, 0xFF3D6FD1, it is the menu as
        // it was: the box 0x1C2A44, the text 0xC8D0E0, the hints 0x8FA0C0.)
        auto shade = [](uint32_t a, auto f) {
            const int r = f((int)(a >> 16 & 0xFF)), g = f((int)(a >> 8 & 0xFF)), b = f((int)(a & 0xFF));
            return 0xFF000000u | (uint32_t)std::clamp(r, 0, 255) << 16 | (uint32_t)std::clamp(g, 0, 255) << 8 | (uint32_t)std::clamp(b, 0, 255);
        };
        const uint32_t boxCol = shade(Accent, [](int v) { return v * 30 / 100; });
        const uint32_t noteBox = shade(Accent, [](int v) { return v * 22 / 100; });
        const uint32_t textCol = shade(Accent, [](int v) { return 180 + v * 17 / 100; });
        const uint32_t hintCol = shade(Accent, [](int v) { return 110 + v * 35 / 100; });
        const uint32_t barOffSel = shade(Accent, [](int v) { return v + (255 - v) * 3 / 10; });
        const uint32_t barOff = shade(Accent, [](int v) { return v / 2; });
        int scale = c.h / 240;
        if (scale < 1) scale = 1;
        int lineH = 11 * scale;
        // (Rows closer together if they would not all fit on the screen, down to 9 * scale.)
        // With no title the box starts with the first item.
        const bool hasTitle = !title.empty();
        // (As many rows as fit at the smallest spacing; the rest scroll into view.)
        const int fit = std::max(1, (c.h - 4 - 6 * scale) / (9 * scale) - (hasTitle ? 3 : 2));
        const int shown = std::min((int)items.size(), fit);
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + shown) scroll = selected - shown + 1;
        scroll = std::max(0, std::min(scroll, (int)items.size() - shown));
        const int rows = shown + (hasTitle ? 3 : 2);
        if (lineH * rows + 6 * scale > c.h - 4) lineH = std::max(9 * scale, (c.h - 4 - 6 * scale) / rows);
        int boxW = 190 * scale;
        if (boxW > c.w - 8) boxW = c.w - 8;
        int boxH = lineH * rows + 6 * scale;
        // The note's lines (up to 4, the last cut short), and the box moved up to make room.
        std::vector<std::string> noteLines;
        if (!note.empty())
        {
            std::string line, word;
            const int maxW = boxW - 12 * scale;
            auto push = [&](const std::string& w) {
                const std::string t = line.empty() ? w : line + " " + w;
                if (!line.empty() && Canvas::TextWidth(t, scale) > maxW) { noteLines.push_back(line); line = w; }
                else line = t;
            };
            for (char ch : note + " ")
            {
                if (ch == ' ') { if (!word.empty()) push(word); word.clear(); }
                else word += ch;
            }
            if (!line.empty()) noteLines.push_back(line);
            if (noteLines.size() > 4) { noteLines.resize(4); noteLines[3] += "..."; }
        }
        const int noteH = noteLines.empty() ? 0 : (int)noteLines.size() * 9 * scale + 6 * scale;
        int bx = (c.w - boxW) / 2, by = std::max(2, (c.h - boxH - noteH) / 2);
        // (Kept for ItemAt: where the rows were drawn.)
        drawnX = bx;
        drawnY = by;
        drawnW = boxW;
        drawnH = boxH;
        drawnLineH = lineH;
        drawnFirstY = by + (hasTitle ? lineH : 0) + 5 * scale - 2 * scale;

        c.Fill(0, 0, c.w, c.h, 0xA0000000);
        c.Fill(bx - scale, by - scale, boxW + 2 * scale, boxH + 2 * scale, 0xFFE8E8E8);
        c.Fill(bx, by, boxW, boxH, boxCol);
        if (hasTitle) c.Text(bx + (boxW - Canvas::TextWidth(title, scale)) / 2, by + 3 * scale, title, scale, 0xFFFFD75E);

        int y = by + (hasTitle ? lineH : 0) + 5 * scale;
        // (Marks at the box's right edge where rows are scrolled out of view, above or below.)
        if (scroll > 0) c.Text(bx + boxW - 8 * scale, y - lineH + 2 * scale, "+", scale, hintCol);
        if (scroll + shown < (int)items.size()) c.Text(bx + boxW - 8 * scale, y + shown * lineH - 2 * scale, "+", scale, hintCol);
        for (int i = scroll; i < scroll + shown; i++)
        {
            bool sel = i == selected;
            if (sel) c.Fill(bx + 2 * scale, y - 2 * scale, boxW - 4 * scale, lineH, Accent);
            uint32_t col = sel ? 0xFFFFFFFF : textCol;
            c.Text(bx + 6 * scale, y, items[i].label, scale, col);
            if (!items[i].value.empty())
            {
                std::string v = items[i].info ? items[i].value : "< " + items[i].value + " >";
                // (A slider's value right-aligned in room for its widest, "< 100% >", the bar left
                // of it, so the bar stays put as the number changes.)
                const int vx = bx + boxW - 6 * scale - Canvas::TextWidth(items[i].slider >= 0 ? "< 100% >" : v, scale);
                c.Text(items[i].slider >= 0 ? bx + boxW - 6 * scale - Canvas::TextWidth(v, scale) : vx, y, v, scale, col);
                if (items[i].slider >= 0)
                {
                    barStep = 5 * scale;
                    barX = vx - 4 * scale - 10 * barStep;
                    for (int k = 0; k < 10; k++)
                        c.Fill(barX + k * barStep, y, 4 * scale, 7 * scale,
                               k < items[i].slider ? (sel ? 0xFFFFFFFF : textCol) : (sel ? barOffSel : barOff));
                }
            }
            y += lineH;
        }
        c.Text(bx + (boxW - Canvas::TextWidth(hint, scale)) / 2, y + 4 * scale, hint, scale, hintCol);
        if (!noteLines.empty())
        {
            const int ny = by + boxH + 3 * scale;
            c.Fill(bx - scale, ny - scale, boxW + 2 * scale, noteH + 2 * scale, 0xFFE8E8E8);
            c.Fill(bx, ny, boxW, noteH, noteBox);
            for (size_t i = 0; i < noteLines.size(); i++)
                c.Text(bx + 6 * scale, ny + 4 * scale + (int)i * 9 * scale, noteLines[i], scale, textCol);
        }
    }
};
