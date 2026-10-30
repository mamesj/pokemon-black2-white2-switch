#pragma once
// The GBA games in the all-games list (launcher.h), from Pokemon GBA Widescreen built in
// (embed.h, SSR_GBA): each as a Launcher::Game of the GBA section, with its box art.
#ifdef SSR_GBA
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "embed.h"
#include "launcher.h"

namespace GbaGames
{

// The game code the list goes by (Launcher::Order, a desktop shortcut's --game): the cartridge's
// own, except Ultra Violet's (a FireRed hack: BPRE too).
inline std::string CodeOf(const std::string& key)
{
    if (key == "emerald") return "BPEE";
    if (key == "ultraviolet") return "UV22";
    if (key == "firered") return "BPRE";
    if (key == "leafgreen") return "BPGE";
    return "GBA-" + key;
}

// Adds the GBA games in roms (saves in saves) to games; their pictures read once (artCache, by
// the ROM's path).
inline void Add(const std::string& roms, const std::string& saves, std::vector<Launcher::Game>& games,
                std::map<std::string, std::vector<uint32_t>>& artCache)
{
    for (const GbaWide::ListGame& l : GbaWide::Find(roms, saves))
    {
        Launcher::Game g;
        g.platform = Launcher::GBA;
        g.rom = l.file;
        g.folder = saves;
        g.code = CodeOf(l.key);
        g.name = l.name;
        g.saveName = l.key + ".sav";
        g.known = true; // (any GBA game plays: with widescreen when it is one of these)
        g.hasSave = l.hasSave;
        auto it = artCache.find(g.rom);
        if (it == artCache.end())
        {
            const unsigned char* jpeg = nullptr;
            size_t size = 0;
            if (!l.art.empty() && GbaWide::Art(l.art, jpeg, size)) Launcher::LoadArt(jpeg, size, g.art);
            it = artCache.emplace(g.rom, g.art).first;
        }
        g.art = it->second;
        games.push_back(std::move(g));
    }
}

// The GBA games in a roms folder (saves in saves), and in a GBA folder inside it (any case), as
// Pokemon GBA Widescreen keeps its own on a phone: those with their saves in its saves folder, so
// that they carry on. One that is in both is marked with where it is.
inline void AddWithGbaFolder(const std::string& roms, const std::string& saves, std::vector<Launcher::Game>& games,
                             std::map<std::string, std::vector<uint32_t>>& artCache)
{
    const size_t before = games.size();
    Add(roms, saves, games, artCache);
    const size_t mine = games.size();
    std::vector<std::string> subs;
    if (DIR* d = opendir(roms.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            for (char& ch : n) ch = (char)tolower((unsigned char)ch);
            struct stat st;
            if (n == "gba" && stat((roms + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                subs.push_back(roms + "/" + e->d_name);
        }
        closedir(d);
    }
    for (const std::string& sub : subs)
    {
        const size_t from = games.size();
        Add(sub, sub + "/saves", games, artCache);
        for (size_t i = from; i < games.size(); i++)
            for (size_t j = before; j < mine; j++)
                if (games[j].code == games[i].code) games[i].where = "IN GBA FOLDER";
    }
}

}
#endif
