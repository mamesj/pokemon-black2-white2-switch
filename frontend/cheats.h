#pragma once
// Cheats (Action Replay codes), run by melonDS's own engine (melonDS/src/AREngine.cpp) every
// time the DS's second processor answers the screen's vertical blank, as a real Action Replay
// does. They come from the cheats folder (beside the saves folder, or in the save's folder):
//
//   <game code>.txt   codes of your own: a line with the cheat's name, then its code lines
//                     ("92247612 00002801"); a line starting with # is a comment
//   <game code>.mch   a melonDS cheat file (as melonDS saves them)
//   usrcheat.dat      the R4 / DeSmuME cheat database: this game's codes are read from it
//
// Which are on is kept in <game code>-on.txt there (one name a line), so the game list's
// other games and a new database keep theirs. CHEATS in the pause menu turns them on and off.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "ARCodeFile.h"
#include "ARDatabaseDAT.h"
#include "AREngine.h"
#include "CRC32.h"

namespace Cheats
{

struct Cheat
{
    std::string name;  // its own full name, in the menus' font
    std::string key;   // as kept in <code>-on.txt: where it came from and its full name
    std::vector<melonDS::u32> code;
    bool on = false;
    int group = -1;    // a folder of the database in which only one may be on (-1: none)
    int folder = -1;   // a collapsible folder in the menu (-1: at the root)
    std::string note;  // what the database says about it ("hold L+R", ...), in the menus' font
};

struct Folder
{
    std::string name, note;
    bool expanded = false;
};

struct Row
{
    int index;
    bool folder;
};

// A name in the menus' font: capitals, digits and : - + . / < > ! ( ), others as spaces, cut to
// `max` characters.
inline std::string Shown(const std::string& s, size_t max)
{
    std::string o;
    bool space = false;
    for (unsigned char c : s)
    {
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(":-+./<>!()", c);
        if (!ok || c == ' ')
        {
            if (!o.empty() && !space) o += ' ';
            space = true;
            continue;
        }
        o += (char)c;
        space = false;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.size() > max) o = o.substr(0, max - 1) + ".";
    return o.empty() ? "?" : o;
}

struct List
{
    std::vector<Cheat> items;
    std::vector<Folder> folders;
    std::string folder, code;
    std::vector<std::string> sources; // what was read, for game.log

    static bool Exists(const std::string& p)
    {
        struct stat st;
        return stat(p.c_str(), &st) == 0;
    }

    // Two words of hex ("92247612 00002801", also run together) from a line; false for anything else.
    static bool CodeLine(const std::string& l, std::vector<melonDS::u32>& out)
    {
        std::string hex;
        for (char c : l)
        {
            if (isxdigit((unsigned char)c)) hex += c;
            else if (c != ' ' && c != '\t' && c != '\r') return false;
        }
        if (hex.empty() || hex.size() % 16) return false;
        for (size_t i = 0; i < hex.size(); i += 8) out.push_back((melonDS::u32)strtoul(hex.substr(i, 8).c_str(), nullptr, 16));
        return true;
    }

    void Add(const std::string& name, const std::string& key, const std::vector<melonDS::u32>& code, int group = -1,
             const std::string& note = "", int parent = -1)
    {
        if (code.empty()) return;
        Cheat c;
        // Keep the name intact: the menu fits it to the current window and text size.
        c.name = Shown(name, name.size() + 1);
        c.key = key;
        c.code = code;
        c.group = group;
        c.folder = parent;
        c.note = c.name;
        if (!note.empty() && Shown(note, note.size() + 1) != "?") c.note += ": " + Shown(note, note.size() + 1);
        items.push_back(c);
    }

    void AddCategory(const melonDS::ARCodeCat& cat, bool database)
    {
        const int parent = (int)folders.size();
        const std::string name = Shown(cat.Name, cat.Name.size() + 1);
        std::string note = name;
        if (!cat.Description.empty()) note += ": " + Shown(cat.Description, cat.Description.size() + 1);
        if (cat.OnlyOneCodeEnabled) note += ". ONE CHEAT AT A TIME";
        folders.push_back({ name, note });
        const size_t first = items.size();
        for (const auto& sub : cat.Children)
            if (std::holds_alternative<melonDS::ARCode>(sub))
            {
                const auto& c = std::get<melonDS::ARCode>(sub);
                // Keep the existing saved keys, including the database folder's original name.
                Add(c.Name, database ? "dat:" + cat.Name + "/" + c.Name : "mch:" + c.Name,
                    c.Code, cat.OnlyOneCodeEnabled ? parent : -1,
                    c.Description.empty() ? cat.Description : c.Description, parent);
            }
        if (items.size() == first) folders.pop_back();
    }

    std::vector<Row> Rows(bool includeClosed = false) const
    {
        std::vector<Row> rows;
        for (size_t i = 0; i < items.size(); i++)
        {
            const int parent = items[i].folder;
            if (parent >= 0 && (i == 0 || items[i - 1].folder != parent)) rows.push_back({ parent, true });
            if (parent < 0 || includeClosed || folders[parent].expanded) rows.push_back({ (int)i, false });
        }
        return rows;
    }

    std::string Label(const Row& row) const
    {
        if (row.folder)
            return std::string(folders[row.index].expanded ? "- " : "+ ") + folders[row.index].name;
        const auto& c = items[row.index];
        // Start children beyond both the folder marker and its name's left edge.
        return std::string(c.folder < 0 ? "" : "    ") + c.name;
    }

    void Load(const std::string& cheatFolder, const std::string& gameCode, const unsigned char* romHeader)
    {
        items.clear();
        folders.clear();
        sources.clear();
        folder = cheatFolder;
        code = gameCode;
        // Codes of one's own: a name line, then code lines.
        const std::string txt = folder + "/" + code + ".txt";
        if (FILE* f = fopen(txt.c_str(), "r"))
        {
            char buf[512];
            std::string name;
            std::vector<melonDS::u32> lines;
            int unnamed = 0;
            auto flush = [&]() {
                if (!lines.empty()) Add(name.empty() ? "CHEAT " + std::to_string(++unnamed) : name, "txt:" + name, lines);
                lines.clear();
            };
            while (fgets(buf, sizeof buf, f))
            {
                std::string l = buf;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
                size_t a = l.find_first_not_of(" \t");
                if (a == std::string::npos || l[a] == '#' || l[a] == ';') continue;
                l = l.substr(a);
                if (CodeLine(l, lines)) continue;
                flush();
                name = l;
                if (name.size() > 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
            }
            flush();
            fclose(f);
            sources.push_back(txt);
        }
        // melonDS's own cheat files.
        const std::string mch = folder + "/" + code + ".mch";
        if (Exists(mch))
        {
            melonDS::ARCodeFile file(mch);
            if (!file.Error)
            {
                for (const auto& item : file.RootCat.Children)
                    if (std::holds_alternative<melonDS::ARCodeCat>(item)) AddCategory(std::get<melonDS::ARCodeCat>(item), false);
                    else
                    {
                        const auto& c = std::get<melonDS::ARCode>(item);
                        Add(c.Name, "mch:" + c.Name, c.Code, -1, c.Description);
                    }
                sources.push_back(mch);
            }
        }
        // The R4 / DeSmuME database: this game's entry (the one made for this very ROM if there
        // is one, by the checksum of its header, as melonDS picks it).
        const std::string dat = folder + "/usrcheat.dat";
        if (Exists(dat) && romHeader)
        {
            melonDS::ARDatabaseDAT db(dat);
            const melonDS::u32 gc = (melonDS::u32)romHeader[12] | romHeader[13] << 8 | romHeader[14] << 16 | (melonDS::u32)romHeader[15] << 24;
            if (!db.Error && db.FindGameCode(gc))
            {
                melonDS::ARDatabaseEntryList entries = db.GetEntriesByGameCode(gc);
                const melonDS::u32 sum = ~melonDS::CRC32(romHeader, 0x200, 0);
                const melonDS::ARDatabaseEntry* pick = entries.empty() ? nullptr : &entries[0];
                for (const auto& e : entries)
                    if (e.Checksum == sum) pick = &e;
                if (pick)
                {
                    for (const auto& item : pick->RootCat.Children)
                    {
                        if (std::holds_alternative<melonDS::ARCode>(item))
                        {
                            const auto& c = std::get<melonDS::ARCode>(item);
                            Add(c.Name, "dat:" + c.Name, c.Code, -1, c.Description);
                        }
                        else AddCategory(std::get<melonDS::ARCodeCat>(item), true);
                    }
                    sources.push_back(dat + " (" + pick->Name + ")");
                }
            }
        }
        // Which are on.
        if (FILE* f = fopen(OnFile().c_str(), "r"))
        {
            char buf[512];
            while (fgets(buf, sizeof buf, f))
            {
                std::string l = buf;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                for (Cheat& c : items)
                    if (c.key == l) c.on = true;
            }
            fclose(f);
        }
    }

    std::string OnFile() const { return folder + "/" + code + "-on.txt"; }

    // Turns cheat i on or off (another of its folder off, if only one may be on), and keeps it.
    void Toggle(size_t i)
    {
        if (i >= items.size()) return;
        items[i].on = !items[i].on;
        if (items[i].on && items[i].group >= 0)
            for (size_t j = 0; j < items.size(); j++)
                if (j != i && items[j].group == items[i].group) items[j].on = false;
        // (The folder made if need be: one level, beside the saves.)
#ifdef _WIN32
        _mkdir(folder.c_str());
#else
        mkdir(folder.c_str(), 0755);
#endif
        if (FILE* f = fopen(OnFile().c_str(), "w"))
        {
            for (const Cheat& c : items)
                if (c.on) fprintf(f, "%s\n", c.key.c_str());
            fclose(f);
        }
    }

    int On(int parent = -1) const
    {
        int n = 0;
        for (const Cheat& c : items) n += c.on && (parent < 0 || c.folder == parent);
        return n;
    }

    int Count(int parent) const
    {
        return (int)std::count_if(items.begin(), items.end(), [&](const Cheat& c) { return c.folder == parent; });
    }

    std::string FolderValue(int parent) const
    {
        const int n = On(parent);
        return n ? std::to_string(n) : "";
    }

    // The ones that are on, to melonDS's engine.
    void Apply(melonDS::AREngine& engine) const
    {
        engine.Cheats.clear();
        for (const Cheat& c : items)
        {
            if (!c.on) continue;
            melonDS::ARCode a;
            a.Parent = nullptr;
            a.Name = c.key;
            a.Enabled = true;
            a.Code = c.code;
            engine.Cheats.push_back(a);
        }
    }
};

// The cheats folder for a save: beside a folder called saves, otherwise in the save's own folder.
inline std::string FolderFor(const std::string& save)
{
    const size_t slash = save.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : save.substr(0, slash);
    const size_t s2 = dir.find_last_of("/\\");
    const std::string last = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
    if (last == "saves") return (s2 == std::string::npos ? std::string(".") : dir.substr(0, s2)) + "/cheats";
    return dir + "/cheats";
}

} // namespace Cheats
