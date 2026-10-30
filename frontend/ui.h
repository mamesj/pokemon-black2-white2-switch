// Software-drawn overlay: pause menu, touch cursor and status messages.
// Drawn into an ARGB buffer the size of the output, so the window and the
// headless screenshots show the same thing.
#pragma once
#include <algorithm>
#include <cmath>
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
        };
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') return font[c - 'A'];
        if (c >= '0' && c <= '9') return font[26 + c - '0'];
        static const char punct[] = ":-+./<>!()\x01\x02\x03\x04";
        const char* p = c ? strchr(punct, c) : nullptr;
        return p ? font[36 + (p - punct)] : nullptr;
    }

    static int TextWidth(const std::string& s, int scale) { return (int)s.size() * 6 * scale; }
    static int TextWidth(const std::string& s, double scale) { return (int)std::ceil(s.size() * 6 * scale); }

    static std::string FitText(const std::string& s, int width, double scale)
    {
        const size_t chars = (size_t)std::max(0, (int)std::floor(width / (6 * scale)));
        if (s.size() <= chars) return s;
        return chars ? s.substr(0, chars - 1) + "." : "";
    }

    // Fractional sizing uses the same glyphs, with each edge rounded to an output pixel.
    // This keeps the existing font while allowing the cheat menu's 0.1X steps.
    void Text(int x, int y, const std::string& s, double scale, uint32_t argb)
    {
        for (size_t i = 0; i < s.size(); i++)
            if (const uint8_t* g = Glyph(s[i]))
                for (int row = 0; row < 7; row++)
                    for (int col = 0; col < 5; col++)
                        if (g[row] & (0x10 >> col))
                        {
                            const int x0 = (int)std::round((i * 6 + col) * scale);
                            const int y0 = (int)std::round(row * scale);
                            Fill(x + x0, y + y0, (int)std::round((i * 6 + col + 1) * scale) - x0,
                                 (int)std::round((row + 1) * scale) - y0, argb);
                        }
    }

    void Text(int x, int y, const std::string& s, int scale, uint32_t argb)
    {
        for (char c : s)
        {
            if (const uint8_t* g = Glyph(c))
                for (int row = 0; row < 7; row++)
                    for (int col = 0; col < 5; col++)
                        if (g[row] & (0x10 >> col))
                            Fill(x + col * scale, y + row * scale, scale, scale, argb);
            x += 6 * scale;
        }
    }
};

// Button presses the menu understands, edge-triggered.
struct MenuInput
{
    bool up = false, down = false, left = false, right = false, accept = false, back = false;
};

// One press straight away, then a repeat after a quarter of a second and every 60 ms.
struct MenuRepeat
{
    int held = 0;
    uint32_t next = 0;

    bool Update(int direction, uint32_t now)
    {
        if (!direction) { held = 0; return false; }
        if (direction != held) { held = direction; next = now + 250; return true; }
        if ((int32_t)(now - next) < 0) return false;
        next = now + 60;
        return true;
    }
};

struct MenuStyle
{
    int textTenths = 10;
    bool fitWidth = false;
    bool margins = false;
};

struct MenuItem
{
    std::string label;
    std::string value; // shown right-aligned; empty hides the value
    bool directional = false; // folders can open/close with left/right even without a count
};

// The complete contents used to size a page, including rows in closed folders. Values use
// their widest form so that toggles, counts and navigation do not move the window.
struct MenuLayout
{
    std::vector<MenuItem> items;
    std::string hint;
};

struct Menu
{
    bool open = false;
    int selected = 0;
    std::vector<MenuItem> items;

    static std::string TextSizeName(int tenths)
    {
        return std::to_string(tenths / 10) + (tenths % 10 ? "." + std::to_string(tenths % 10) : "") + "X";
    }

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
        if (in.left && (!items[selected].value.empty() || items[selected].directional))
        {
            dir = -1;
            return selected;
        }
        if (in.right && (!items[selected].value.empty() || items[selected].directional))
        {
            dir = 1;
            return selected;
        }
        return -1;
    }

    // Where the box and its rows were last drawn (Draw), in the canvas's pixels, and the first
    // row shown (a list longer than the screen scrolls with the chosen row: CHEATS).
    mutable int drawnX = 0, drawnY = 0, drawnW = 0, drawnH = 0, drawnLineH = 0, drawnFirstY = 0;
    mutable int scroll = 0, drawnShown = 0;

    // The row at (x, y) of the canvas the menu was drawn on (touch screens): its index, -1 for
    // a point in the box but on no row, -2 for a point outside the box.
    int ItemAt(int x, int y) const
    {
        if (drawnW <= 0 || x < drawnX || x >= drawnX + drawnW || y < drawnY || y >= drawnY + drawnH) return -2;
        if (drawnLineH <= 0 || y < drawnFirstY) return -1;
        const int row = (y - drawnFirstY) / drawnLineH;
        return row < drawnShown ? scroll + row : -1;
    }

    // note: a few lines of text in a strip under the box (CHEATS: what the database says about
    // the chosen cheat), wrapped to the box's width. A complete layout reserves the same
    // rows and four lines of notes for every selection, including when its note is empty.
    void Draw(Canvas& c, const std::string& title, const std::string& hint, const std::string& note = "",
              const MenuStyle& style = {}, const MenuLayout* layout = nullptr) const
    {
        const auto& sizingItems = layout ? layout->items : items;
        const auto& sizingHint = layout ? layout->hint : hint;
        const int baseScale = std::max(1, c.h / 240);
        const double textScale = baseScale * std::clamp(style.textTenths, 1, 50) / 10.0;
        // Keep the title, instructions and notes usable even with very large list text.
        const double detailScale = std::min((double)baseScale, textScale);
        const int scale = std::max(1, (int)std::ceil(detailScale));
        int lineH = std::max(1, (int)std::round(11 * textScale));
        const int detailLineH = std::max(1, (int)std::round(11 * detailScale));
        const bool hasTitle = !title.empty();
        const int marginX = style.margins ? std::max(8, c.w / 20) : 4;
        const int marginY = style.margins ? std::max(4, c.h / 20) : 2;
        int boxW = 190 * baseScale;
        if (style.fitWidth)
        {
            boxW = std::max(Canvas::TextWidth(title, detailScale), Canvas::TextWidth(sizingHint, detailScale)) + 12 * scale;
            for (const auto& item : sizingItems)
            {
                const int valueW = item.value.empty() ? 0 : Canvas::TextWidth("< " + item.value + " >", textScale) + 8 * scale;
                boxW = std::max(boxW, Canvas::TextWidth(item.label, textScale) + valueW + 12 * scale);
            }
        }
        boxW = std::max(1, std::min(boxW, c.w - 2 * marginX - 2 * scale));
        // Wrap the full selected name and note within the space reserved for this page.
        std::vector<std::string> noteLines;
        const int noteChars = std::max(1, (int)((boxW - 12 * scale) / (6 * detailScale)));
        if (!note.empty())
        {
            size_t pos = 0;
            while (pos < note.size() && noteLines.size() < 4)
            {
                size_t end = std::min(note.size(), pos + noteChars);
                if (end < note.size())
                {
                    const size_t space = note.rfind(' ', end);
                    if (space != std::string::npos && space > pos) end = space;
                }
                noteLines.push_back(note.substr(pos, end - pos));
                pos = note.find_first_not_of(' ', end);
            }
            if (pos < note.size()) noteLines.back() = noteLines.back().substr(0, std::max(0, noteChars - 3)) + "...";
        }
        const int noteLineH = std::max(1, (int)std::round(9 * detailScale));
        const int fixedH = (hasTitle ? detailLineH : 0) + 2 * detailLineH + 6 * scale;
        const int available = c.h - 2 * marginY - 2 * scale;
        int noteRows = layout ? 4 : (int)noteLines.size();
        while (noteRows && fixedH + lineH + noteRows * noteLineH + 9 * scale > available)
            noteRows--;
        if ((int)noteLines.size() > noteRows) noteLines.resize(noteRows);
        const int noteH = noteRows ? noteRows * noteLineH + 6 * scale : 0;
        const int maxBoxH = available - noteH - (noteH ? 3 * scale : 0);
        const int minLineH = std::max(1, (int)std::round(9 * textScale));
        const int fit = std::max(1, (maxBoxH - fixedH) / minLineH);
        const int slots = std::min((int)sizingItems.size(), fit);
        const int shown = std::min((int)items.size(), slots);
        if (slots) lineH = std::max(minLineH, std::min(lineH, (maxBoxH - fixedH) / slots));
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + shown) scroll = selected - shown + 1;
        scroll = std::max(0, std::min(scroll, (int)items.size() - shown));
        const int boxH = slots * lineH + fixedH;
        int bx = (c.w - boxW) / 2, by = std::max(marginY + scale, (c.h - boxH - noteH - (noteH ? 3 * scale : 0)) / 2);
        // (Kept for ItemAt: where the rows were drawn.)
        drawnX = bx;
        drawnY = by;
        drawnW = boxW;
        drawnH = boxH;
        drawnLineH = lineH;
        drawnShown = shown;
        drawnFirstY = by + (hasTitle ? detailLineH : 0) + 3 * scale;

        c.Fill(0, 0, c.w, c.h, 0xA0000000);
        c.Fill(bx - scale, by - scale, boxW + 2 * scale, boxH + 2 * scale, 0xFFE8E8E8);
        c.Fill(bx, by, boxW, boxH, 0xFF1C2A44);
        const std::string heading = Canvas::FitText(title, boxW - 12 * scale, detailScale);
        if (hasTitle) c.Text(bx + (boxW - Canvas::TextWidth(heading, detailScale)) / 2, by + 3 * scale, heading, detailScale, 0xFFFFD75E);

        int y = drawnFirstY;
        // (Marks at the box's right edge where rows are scrolled out of view, above or below.)
        if (scroll > 0) c.Text(bx + boxW - 8 * scale, y - lineH + 2 * scale, "+", scale, 0xFF8FA0C0);
        if (scroll + shown < (int)items.size()) c.Text(bx + boxW - 8 * scale, y + shown * lineH - 2 * scale, "+", scale, 0xFF8FA0C0);
        for (int i = scroll; i < scroll + shown; i++)
        {
            bool sel = i == selected;
            if (sel) c.Fill(bx + 2 * scale, y, boxW - 4 * scale, lineH, 0xFF3D6FD1);
            uint32_t col = sel ? 0xFFFFFFFF : 0xFFC8D0E0;
            const std::string v = items[i].value.empty() ? "" : "< " + items[i].value + " >";
            const int valueW = Canvas::TextWidth(v, textScale);
            const int labelW = boxW - 12 * scale - (v.empty() ? 0 : valueW + 8 * scale);
            const int textY = y + std::max(0, (lineH - (int)std::ceil(7 * textScale)) / 2);
            c.Text(bx + 6 * scale, textY, Canvas::FitText(items[i].label, labelW, textScale), textScale, col);
            if (!items[i].value.empty())
            {
                const std::string value = Canvas::FitText(v, boxW - 12 * scale, textScale);
                c.Text(bx + boxW - 6 * scale - Canvas::TextWidth(value, textScale), textY, value, textScale, col);
            }
            y += lineH;
        }
        const std::string footer = Canvas::FitText(hint, boxW - 12 * scale, detailScale);
        const int footerY = drawnFirstY + slots * lineH + 4 * scale;
        c.Text(bx + (boxW - Canvas::TextWidth(footer, detailScale)) / 2, footerY, footer, detailScale, 0xFF8FA0C0);
        if (noteH)
        {
            const int ny = by + boxH + 3 * scale;
            c.Fill(bx - scale, ny - scale, boxW + 2 * scale, noteH + 2 * scale, 0xFFE8E8E8);
            c.Fill(bx, ny, boxW, noteH, 0xFF142034);
            for (size_t i = 0; i < noteLines.size(); i++)
                c.Text(bx + 6 * scale, ny + 4 * scale + (int)i * noteLineH,
                       Canvas::FitText(noteLines[i], boxW - 12 * scale, detailScale), detailScale, 0xFFC8D0E0);
        }
    }
};
