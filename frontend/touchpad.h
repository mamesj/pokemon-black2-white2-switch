// On-screen controls for touch screens without a controller (Android): a D-pad, the four
// face buttons, L and R, Start, Select, a menu button and fast forward, drawn over the picture. Each
// finger is followed on its own, so a direction can be held while a button is pressed.
// Fingers that start anywhere else touch the game's touch screen (see main.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <SDL.h>
#include "ui.h"

struct TouchPad
{
    // What a finger is on. The values of the buttons are the PAD_* controller buttons
    // they act as (by label: the button marked A is A).
    enum { NONE = -1, DPAD = -2, MENU = -3, SCREEN = -4, FAST = -5 };

    struct Button
    {
        int pad;           // PAD_* index, MENU or FAST
        float x, y, r;     // centre and radius in output pixels (pills use w/h)
        float w, h;        // non-zero: a rounded rectangle instead of a circle
        const char* label;
    };

    bool visible = true;
    std::vector<Button> buttons;
    float dpadX = 0, dpadY = 0, dpadR = 0;
    int w = 0, h = 0;

    std::map<SDL_FingerID, int> fingerOn;          // what each finger went down on
    std::map<SDL_FingerID, SDL_FPoint> fingerAt;   // where each finger is, in output pixels
    std::map<SDL_FingerID, int> lifted;            // up, frames left to count as held (see Up)

    SDL_Texture* labels = nullptr;
    std::map<std::string, SDL_Rect> labelRect;

    void Layout(int outW, int outH)
    {
        if (outW == w && outH == h) return;
        w = outW;
        h = outH;
        const float u = outH / 10.0f;
        dpadR = 1.55f * u;
        dpadX = 0.35f * u + dpadR;
        dpadY = outH - 0.45f * u - dpadR;
        const float fx = outW - 0.35f * u - 1.75f * u, fy = dpadY, d = 1.05f * u, br = 0.62f * u;
        buttons = {
            { PAD_A, fx + d, fy, br, 0, 0, "A" },
            { PAD_B, fx, fy + d, br, 0, 0, "B" },
            { PAD_X, fx, fy - d, br, 0, 0, "X" },
            { PAD_Y, fx - d, fy, br, 0, 0, "Y" },
            { PAD_LB, 0.35f * u + 1.3f * u, 0.3f * u + 0.45f * u, 0, 2.6f * u, 0.9f * u, "L" },
            { PAD_RB, outW - 0.35f * u - 1.3f * u, 0.3f * u + 0.45f * u, 0, 2.6f * u, 0.9f * u, "R" },
            { PAD_BACK, outW / 2.0f - 1.25f * u, outH - 0.55f * u, 0, 2.2f * u, 0.7f * u, "SELECT" },
            { PAD_START, outW / 2.0f + 1.25f * u, outH - 0.55f * u, 0, 2.2f * u, 0.7f * u, "START" },
            { MENU, outW / 2.0f, 0.55f * u, 0, 1.6f * u, 0.7f * u, "MENU" },
            // Fast forward while held, beside R: the DS's R stays the game's own button, as on
            // a controller, where fast forward is the right trigger.
            { FAST, outW - 0.35f * u - 2.6f * u - 0.3f * u - 0.8f * u, 0.3f * u + 0.45f * u, 0, 1.6f * u, 0.9f * u, ">>" },
        };
    }

    int HitButton(float x, float y) const
    {
        for (const Button& b : buttons)
        {
            if (b.w > 0)
            {
                // A little slack around the small pills.
                if (fabsf(x - b.x) <= b.w / 2 + h * 0.02f && fabsf(y - b.y) <= b.h / 2 + h * 0.02f) return b.pad;
            }
            else if ((x - b.x) * (x - b.x) + (y - b.y) * (y - b.y) <= b.r * b.r * 1.3f) return b.pad;
        }
        float dx = x - dpadX, dy = y - dpadY;
        if (dx * dx + dy * dy <= dpadR * dpadR * 1.2f) return DPAD;
        return NONE;
    }

    // A finger went down; returns what it landed on (SCREEN when it is not a control).
    int Down(SDL_FingerID id, float x, float y)
    {
        int on = visible ? HitButton(x, y) : NONE;
        if (on == NONE) on = SCREEN;
        // Android reuses finger ids: a new touch is not the release of the last one.
        lifted.erase(id);
        fingerOn[id] = on;
        fingerAt[id] = { x, y };
        return on;
    }
    void Move(SDL_FingerID id, float x, float y)
    {
        if (fingerOn.count(id)) fingerAt[id] = { x, y };
    }
    // A lifted finger still counts for a few frames, so that a quick tap still presses its
    // button: some screens of the game read the buttons only every other frame.
    void Up(SDL_FingerID id)
    {
        if (fingerOn.count(id) && !lifted.count(id)) lifted[id] = 3;
    }
    void NextFrame()
    {
        for (auto it = lifted.begin(); it != lifted.end();)
        {
            if (--it->second > 0)
            {
                ++it;
                continue;
            }
            fingerOn.erase(it->first);
            fingerAt.erase(it->first);
            it = lifted.erase(it);
        }
    }

    // The finger on the game's touch screen, if any (the first one that went down there).
    bool ScreenFinger(float& x, float& y) const
    {
        for (auto& f : fingerOn)
            if (f.second == SCREEN)
            {
                const SDL_FPoint& p = fingerAt.at(f.first);
                x = p.x;
                y = p.y;
                return true;
            }
        return false;
    }

    // The buttons held, written into a controller state (button names by label).
    void Apply(bool btn[], bool& menu) const
    {
        menu = false;
        for (auto& f : fingerOn)
        {
            const int on = f.second;
            const SDL_FPoint& p = fingerAt.at(f.first);
            if (on == DPAD)
            {
                // Eight directions; the centre is a dead zone.
                float dx = p.x - dpadX, dy = p.y - dpadY;
                if (dx * dx + dy * dy < dpadR * dpadR * 0.06f) continue;
                float a = atan2f(dy, dx);
                const float pi = 3.14159265f;
                int sector = (int)lroundf(a / (pi / 4)) & 7;   // 0 = right, 2 = down, 4 = left, 6 = up
                if (sector == 7 || sector == 0 || sector == 1) btn[PAD_RIGHT] = true;
                if (sector >= 1 && sector <= 3) btn[PAD_DOWN] = true;
                if (sector >= 3 && sector <= 5) btn[PAD_LEFT] = true;
                if (sector >= 5 && sector <= 7) btn[PAD_UP] = true;
            }
            else if (on == MENU) menu = true;
            else if (on >= 0) btn[on] = true;
        }
    }

    // A finger on fast forward.
    bool FastHeld() const
    {
        for (auto& f : fingerOn)
            if (f.second == FAST) return true;
        return false;
    }

    void MakeLabels(SDL_Renderer* r)
    {
        if (labels) return;
        const char* names[] = { "A", "B", "X", "Y", "L", "R", "SELECT", "START", "MENU", ">>" };
        const int scale = 3;
        Canvas c;
        int x = 0;
        for (const char* n : names)
        {
            int tw = Canvas::TextWidth(n, scale);
            labelRect[n] = { x, 0, tw, 7 * scale };
            x += tw + 2;
        }
        c.Resize(x, 7 * scale);
        x = 0;
        for (const char* n : names)
        {
            c.Text(labelRect[n].x, 0, n, scale, 0xFFFFFFFF);
        }
        labels = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, c.w, c.h);
        SDL_UpdateTexture(labels, nullptr, c.px.data(), c.w * 4);
        SDL_SetTextureBlendMode(labels, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(labels, SDL_ScaleModeLinear);
    }

    static void Disc(std::vector<SDL_Vertex>& v, float cx, float cy, float rx, float ry, SDL_Color col)
    {
        const int n = 28;
        for (int i = 0; i < n; i++)
        {
            float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
            v.push_back({ { cx, cy }, col, { 0, 0 } });
            v.push_back({ { cx + rx * cosf(a0), cy + ry * sinf(a0) }, col, { 0, 0 } });
            v.push_back({ { cx + rx * cosf(a1), cy + ry * sinf(a1) }, col, { 0, 0 } });
        }
    }
    static void Quad(std::vector<SDL_Vertex>& v, float x0, float y0, float x1, float y1, SDL_Color col)
    {
        v.push_back({ { x0, y0 }, col, { 0, 0 } });
        v.push_back({ { x1, y0 }, col, { 0, 0 } });
        v.push_back({ { x1, y1 }, col, { 0, 0 } });
        v.push_back({ { x0, y0 }, col, { 0, 0 } });
        v.push_back({ { x1, y1 }, col, { 0, 0 } });
        v.push_back({ { x0, y1 }, col, { 0, 0 } });
    }
    static void Pill(std::vector<SDL_Vertex>& v, float cx, float cy, float pw, float ph, SDL_Color col)
    {
        float r = ph / 2;
        Quad(v, cx - pw / 2 + r, cy - r, cx + pw / 2 - r, cy + r, col);
        // Half discs at the ends (whole discs: the overlap is hidden by the alpha being even).
        const int n = 14;
        for (int side = 0; side < 2; side++)
        {
            float ex = side ? cx + pw / 2 - r : cx - pw / 2 + r;
            for (int i = 0; i < n; i++)
            {
                float a0 = 3.14159265f * i / n + (side ? -1.5707963f : 1.5707963f);
                float a1 = 3.14159265f * (i + 1) / n + (side ? -1.5707963f : 1.5707963f);
                v.push_back({ { ex, cy }, col, { 0, 0 } });
                v.push_back({ { ex + r * cosf(a0), cy + r * sinf(a0) }, col, { 0, 0 } });
                v.push_back({ { ex + r * cosf(a1), cy + r * sinf(a1) }, col, { 0, 0 } });
            }
        }
    }

    void Draw(SDL_Renderer* r, const bool held[], bool menuHeld)
    {
        if (!visible) return;
        MakeLabels(r);
        std::vector<SDL_Vertex> v;
        const SDL_Color idle = { 255, 255, 255, 46 }, down = { 255, 255, 255, 110 }, dark = { 0, 0, 0, 70 };
        // D-pad: a cross on a faint disc.
        Disc(v, dpadX, dpadY, dpadR, dpadR, { 0, 0, 0, 40 });
        const float arm = dpadR * 0.36f;
        // (A dark edge around the cross, as around the buttons, so that it shows on light
        // scenery too: towns and snow.)
        const float edge = dpadR * 0.05f;
        Quad(v, dpadX - dpadR * 0.92f - edge, dpadY - arm - edge, dpadX + dpadR * 0.92f + edge, dpadY + arm + edge, dark);
        Quad(v, dpadX - arm - edge, dpadY - dpadR * 0.92f - edge, dpadX + arm + edge, dpadY - arm - edge, dark);
        Quad(v, dpadX - arm - edge, dpadY + arm + edge, dpadX + arm + edge, dpadY + dpadR * 0.92f + edge, dark);
        Quad(v, dpadX - dpadR * 0.92f, dpadY - arm, dpadX + dpadR * 0.92f, dpadY + arm, idle);
        Quad(v, dpadX - arm, dpadY - dpadR * 0.92f, dpadX + arm, dpadY - arm, idle);
        Quad(v, dpadX - arm, dpadY + arm, dpadX + arm, dpadY + dpadR * 0.92f, idle);
        auto armLit = [&](float x0, float y0, float x1, float y1) { Quad(v, x0, y0, x1, y1, down); };
        if (held[PAD_LEFT]) armLit(dpadX - dpadR * 0.92f, dpadY - arm, dpadX - arm, dpadY + arm);
        if (held[PAD_RIGHT]) armLit(dpadX + arm, dpadY - arm, dpadX + dpadR * 0.92f, dpadY + arm);
        if (held[PAD_UP]) armLit(dpadX - arm, dpadY - dpadR * 0.92f, dpadX + arm, dpadY - arm);
        if (held[PAD_DOWN]) armLit(dpadX - arm, dpadY + arm, dpadX + arm, dpadY + dpadR * 0.92f);
        for (const Button& b : buttons)
        {
            bool on = b.pad == MENU ? menuHeld : b.pad == FAST ? FastHeld() : held[b.pad];
            if (b.w > 0)
            {
                Pill(v, b.x, b.y, b.w, b.h, dark);
                Pill(v, b.x, b.y, b.w * 0.94f, b.h * 0.84f, on ? down : idle);
            }
            else
            {
                Disc(v, b.x, b.y, b.r, b.r, dark);
                Disc(v, b.x, b.y, b.r * 0.9f, b.r * 0.9f, on ? down : idle);
            }
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
        // Labels.
        for (const Button& b : buttons)
        {
            const SDL_Rect& src = labelRect[b.label];
            float s = (b.w > 0 ? b.h * 0.42f : b.r * 0.7f) / src.h;
            SDL_FRect dst = { b.x - src.w * s / 2, b.y - src.h * s / 2, src.w * s, src.h * s };
            SDL_SetTextureAlphaMod(labels, 170);
            SDL_RenderCopyF(r, labels, &src, &dst);
        }
    }
};
