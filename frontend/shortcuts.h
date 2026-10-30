// Desktop shortcuts for the PC all-games program (frontend/pc_launcher.cpp): one per game in the
// list, which starts that game straight away ("--game IREO"), with the game's picture as its icon.
// Windows: a .lnk on the desktop and an .ico beside the program (shortcut-icons/). Linux: a
// .desktop file in the applications menu (~/.local/share/applications) and on the desktop, with a
// .png icon.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <propidl.h>
#include <propsys.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

#include "launcher.h"

namespace Shortcuts
{
// ---------------------------------------------------------------------------
// Pictures: the game's picture at the icon sizes, and as PNG files (uncompressed: the few
// hundred kilobytes do not matter, and no library is needed).

inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

// ARGB pixels (w x h) as a PNG file.
inline std::vector<uint8_t> Png(const std::vector<uint32_t>& argb, int w, int h)
{
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 4 + 1));
    for (int y = 0; y < h; y++)
    {
        raw.push_back(0); // (no filter)
        for (int x = 0; x < w; x++)
        {
            const uint32_t p = argb[(size_t)y * w + x];
            raw.push_back((p >> 16) & 255);
            raw.push_back((p >> 8) & 255);
            raw.push_back(p & 255);
            raw.push_back(p >> 24);
        }
    }
    // zlib, with stored (uncompressed) deflate blocks.
    std::vector<uint8_t> z = { 0x78, 0x01 };
    for (size_t at = 0; at < raw.size() || at == 0;)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 255);
        z.push_back(n >> 8);
        z.push_back(~n & 255);
        z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 255);

    std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        const uint32_t n = (uint32_t)data.size();
        for (int s = 24; s >= 0; s -= 8) out.push_back((n >> s) & 255);
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        const uint32_t crc = Crc32(td.data(), td.size());
        for (int s = 24; s >= 0; s -= 8) out.push_back((crc >> s) & 255);
    };
    std::vector<uint8_t> ihdr;
    for (int v : { w, h })
        for (int s = 24; s >= 0; s -= 8) ihdr.push_back((v >> s) & 255);
    ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 }); // 8 bits, RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

// The game's picture at side m (Launcher::GamePicture).
inline std::vector<uint32_t> Picture(const Launcher::Game& g, int m) { return Launcher::GamePicture(g, m); }

inline bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

// The shortcut's name: the game's ("Pokemon Black 2"), as a file name.
inline std::string Title(const Launcher::Game& g)
{
    if (const Launcher::KnownGame* k = Launcher::Known(g.code)) return k->name;
    if (g.platform != Launcher::DS && !g.name.empty()) return Launcher::TitleCase(g.name); // (a GBA game)
    return "Pokemon DS " + g.code;
}

#ifdef _WIN32
inline std::wstring Wide(const std::string& s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.resize(n > 0 ? n - 1 : 0);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

// An .ico holding the picture as PNGs at 256, 48, 32 and 16 pixels.
inline std::vector<uint8_t> Ico(const Launcher::Game& g)
{
    const int sizes[] = { 256, 48, 32, 16 };
    std::vector<std::vector<uint8_t>> pngs;
    for (int s : sizes)
    {
        const std::vector<uint32_t> px = Picture(g, s);
        if (px.empty()) return {};
        pngs.push_back(Png(px, s, s));
    }
    std::vector<uint8_t> out = { 0, 0, 1, 0, (uint8_t)pngs.size(), 0 };
    uint32_t offset = 6 + 16 * (uint32_t)pngs.size();
    auto u16 = [&](uint32_t v) { out.push_back(v & 255); out.push_back((v >> 8) & 255); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    for (size_t i = 0; i < pngs.size(); i++)
    {
        out.push_back(sizes[i] >= 256 ? 0 : sizes[i]);
        out.push_back(sizes[i] >= 256 ? 0 : sizes[i]);
        out.push_back(0);
        out.push_back(0);
        u16(1);
        u16(32);
        u32((uint32_t)pngs[i].size());
        u32(offset);
        offset += (uint32_t)pngs[i].size();
    }
    for (auto& p : pngs) out.insert(out.end(), p.begin(), p.end());
    return out;
}
#endif

#ifdef _WIN32
// The taskbar's names for the program (the game list, and a game started from it) and for each
// game started from its desktop shortcut. Windows groups the windows on the taskbar, and shows
// their icon, by these: without them every window of the program counted as any shortcut to it
// that was pinned (Platinum's, say), and showed that game's picture.
inline std::wstring AppId(const std::string& code)
{
    return code.empty() ? L"mamesj.PokemonDSWide" : L"mamesj.PokemonDSWide." + std::wstring(code.begin(), code.end());
}

// This process's taskbar name: before any window is made.
inline void SetAppId(const std::string& code)
{
    typedef HRESULT(WINAPI * SetId)(PCWSTR);
    if (HMODULE shell = GetModuleHandleW(L"shell32.dll"))
        if (auto set = (SetId)(void*)GetProcAddress(shell, "SetCurrentProcessExplicitAppUserModelID"))
            set(AppId(code).c_str());
}
#endif

// Makes the shortcuts: one per game (once each, the first of any in several folders). dir is
// the program's folder (for the icons). Returns how many were made; where says where they went,
// or what failed.
inline int Make(const std::string& dir, const std::vector<Launcher::Game>& games, std::string& where)
{
    std::set<std::string> done;
    int made = 0;
    const std::string iconDir = dir + "/shortcut-icons";
#ifdef _WIN32
    _wmkdir(Wide(iconDir).c_str());
    wchar_t exe[32768];
    const DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
    if (n == 0 || n >= 32768)
    {
        where = "NO PROGRAM PATH";
        return 0;
    }
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    {
        where = "NO DESKTOP FOLDER";
        return 0;
    }
    const std::wstring desk = desktop;
    CoTaskMemFree(desktop);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (const Launcher::Game& g : games)
    {
        if (!g.known || !done.insert(g.code).second) continue;
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) break;
        link->SetPath(exe);
        link->SetArguments((L"--game " + Wide(g.code)).c_str());
        link->SetWorkingDirectory(Wide(dir).c_str());
        link->SetDescription(Wide(Title(g) + " (Pokémon Fulldex)").c_str());
        const std::vector<uint8_t> ico = Ico(g);
        const std::string icoPath = iconDir + "/" + g.code + ".ico";
        if (!ico.empty() && WriteFile(icoPath, ico)) link->SetIconLocation(Wide(icoPath).c_str(), 0);
        // (The game's own taskbar name, as the program takes when started with --game: SetAppId.)
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, (void**)&props)))
        {
            // PKEY_AppUserModel_ID
            static const PROPERTYKEY key = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
            const std::wstring id = AppId(g.code);
            PROPVARIANT v;
            PropVariantInit(&v);
            v.vt = VT_LPWSTR;
            v.pwszVal = (LPWSTR)CoTaskMemAlloc((id.size() + 1) * sizeof(wchar_t));
            if (v.pwszVal)
            {
                memcpy(v.pwszVal, id.c_str(), (id.size() + 1) * sizeof(wchar_t));
                if (SUCCEEDED(props->SetValue(key, v))) props->Commit();
            }
            PropVariantClear(&v);
            props->Release();
        }
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)))
        {
            if (SUCCEEDED(file->Save((desk + L"\\" + Wide(Title(g)) + L".lnk").c_str(), TRUE))) made++;
            file->Release();
        }
        link->Release();
        fprintf(stderr, "[launcher] desktop shortcut for %s\n", g.code.c_str());
    }
    if (SUCCEEDED(init)) CoUninitialize();
    // Explorer redraws the desktop with the new icons.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    where = "ON YOUR DESKTOP";
#else
    mkdir(iconDir.c_str(), 0755);
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
    {
        where = "NO PROGRAM PATH";
        return 0;
    }
    exe[n] = 0;
    // From an AppImage: the AppImage file itself (the program inside lives only while it runs).
    if (const char* appImage = getenv("APPIMAGE"); appImage && *appImage && strlen(appImage) < sizeof exe)
        strcpy(exe, appImage);
    const char* homeEnv = getenv("HOME");
    const std::string home = homeEnv ? homeEnv : "";
    // The applications menu (and Steam's "Add a Non-Steam Game" list on the Steam Deck).
    const std::string dataHome = getenv("XDG_DATA_HOME") && *getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME") : home + "/.local/share";
    const std::string apps = dataHome + "/applications";
    // (Each folder on the way, as mkdir -p: a new account may have no ~/.local yet.)
    for (size_t i = 1; i <= apps.size(); i++)
        if (i == apps.size() || apps[i] == '/') mkdir(apps.substr(0, i).c_str(), 0755);
    // The desktop folder: XDG_DESKTOP_DIR from ~/.config/user-dirs.dirs, else ~/Desktop.
    std::string desk = home + "/Desktop";
    if (FILE* f = fopen((home + "/.config/user-dirs.dirs").c_str(), "r"))
    {
        char line[1024];
        while (fgets(line, sizeof line, f))
        {
            std::string l = line;
            if (l.compare(0, 16, "XDG_DESKTOP_DIR=") != 0) continue;
            l = l.substr(16);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == '"')) l.pop_back();
            if (!l.empty() && l[0] == '"') l = l.substr(1);
            if (l.compare(0, 5, "$HOME") == 0) l = home + l.substr(5);
            desk = l;
        }
        fclose(f);
    }
    struct stat st;
    const bool hasDesk = stat(desk.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    // (A path in Exec is quoted, with its backslashes, quotes, backticks and dollars escaped.)
    std::string quoted = "\"";
    for (char c : std::string(exe))
    {
        // (The file's own escaping doubles the backslash the Exec rules want before these.)
        if (c == '\\') quoted += "\\\\\\\\";
        else
        {
            if (c == '"' || c == '`' || c == '$') quoted += "\\\\";
            quoted += c;
        }
    }
    quoted += "\"";
    for (const Launcher::Game& g : games)
    {
        if (!g.known || !done.insert(g.code).second) continue;
        const std::string png = iconDir + "/" + g.code + ".png";
        const std::vector<uint32_t> px = Picture(g, 256);
        const bool icon = !px.empty() && WriteFile(png, Png(px, 256, 256));
        std::string lower = g.code;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        const std::string entry = "[Desktop Entry]\nType=Application\nName=" + Title(g) +
            "\nComment=Pokémon Fulldex\nExec=" + quoted + " --game " + g.code +
            "\nPath=" + dir + "\n" + (icon ? "Icon=" + png + "\n" : "") + "Terminal=false\nCategories=Game;\n";
        const std::string file = "pokemon-ds-wide-" + lower + ".desktop";
        bool ok = WriteFile(apps + "/" + file, std::vector<uint8_t>(entry.begin(), entry.end()));
        if (hasDesk)
        {
            const std::string d = desk + "/" + file;
            ok = WriteFile(d, std::vector<uint8_t>(entry.begin(), entry.end())) && ok;
            chmod(d.c_str(), 0755); // (desktops start only launchers marked as programs)
        }
        if (ok) made++;
        fprintf(stderr, "[launcher] shortcut for %s\n", g.code.c_str());
    }
    where = hasDesk ? "ON YOUR DESKTOP AND IN YOUR APPS" : "IN YOUR APPS MENU";
#endif
    return made;
}
}
