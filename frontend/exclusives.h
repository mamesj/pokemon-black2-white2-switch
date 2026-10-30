#pragma once
// No version exclusives (OTHER VERSION POKEMON in the pause menu): the wild Pokemon of both
// versions of a game, in the one being played.
//
// SoulSilver and HeartGold: each game's ROM holds the wild Pokemon tables of both versions
// (pokeheartgold: files/fielddata/encountdata, HeartGold's in a/0/3/7 and SoulSilver's in a/1/3/6)
// and reads its own. The game reads the table of the map it enters into memory
// (WildEncounters_ReadFromNarc, pokeheartgold src/map_events.c: the same address in both games,
// 0x0203B88C); a hook there notes where. Once it is read, the table is found in the version's own
// archive, which gives the other version's table for the same map.
//
// Black, White, Black 2 and White 2: each ROM holds only its own version's tables, so the other
// version's Pokemon come from the list of the exclusives below, each with its counterpart: where
// the versions' tables differ by an exclusive, it is the other version's in the same slot (Gothita
// in Black where White has Solosis, at the same rate and levels). The game reads a zone's table for the season with EncData_Load (Black 2 and
// White 2; FIELD_ENCOUNT_ST_LoadEncountData in Black and White), hooked through the function
// table; the other version's is then the same table with each exclusive turned into its
// counterpart.
//
// Then, in all of them, every frame each slot where the versions differ gets one or the other
// version's Pokemon: each wild encounter is as likely to be one as the other, at that slot's own
// rate and with its own levels. Legendary Pokemon are not in these tables (they are met in the
// games' scripts) and stay with their own version. Before each write the table in memory is
// compared with what it should be (the map's table with one or the other in each such slot), so a
// table the game has let go of (a battle, another map) is never written to. Nothing of the games is
// in the program: the tables are the player's own ROM's.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ARM.h"
#include "NDS.h"
#include "funcwatch.h"
#include "recomp_api.h"

class VersionExclusives
{
public:
    bool on = false; // the setting

    // Whether this game has the setting.
    bool Supported() const { return Kind != None; }

    // SoulSilver and HeartGold, as the game starts, with its ROM file and the hook API ready (rc
    // tables loaded).
    void Init(const std::string& romPath, const std::string& code, melonDS::NDS& nds)
    {
        if (code != "IPGE" && code != "IPKE") return;
        Nds = &nds;
        std::vector<std::vector<uint8_t>> hg, ss;
        if (!ReadNarc(romPath, "a/0/3/7", hg) || !ReadNarc(romPath, "a/1/3/6", ss) || hg.size() != ss.size())
        {
            fprintf(stderr, "[exclusives] the wild Pokemon tables could not be read\n");
            return;
        }
        if (code == "IPKE") { Own = std::move(hg); Other = std::move(ss); }
        else { Own = std::move(ss); Other = std::move(hg); }
        if (rc_add_hook("main", 0x0203B88C, &VersionExclusives::Read, this) < 0)
        {
            Own.clear();
            Other.clear();
            return;
        }
        Kind = Gen4;
        Size = 0xC4; // EncounterData (pokeheartgold include/wild_encounter.h)
        fprintf(stderr, "[exclusives] %zu wild Pokemon tables of each version\n", Own.size());
    }

    // Black, White, Black 2 and White 2, with the function table (before it is attached: the
    // function is added to those watched).
    void Init(const std::string& code, FuncWatch& watch, std::vector<std::string>& names, melonDS::NDS& nds)
    {
        const bool b2w2 = code == "IREO" || code == "IRDO", bw = code == "IRBO" || code == "IRAO";
        if (!b2w2 && !bw) return;
        const char* fn = b2w2 ? "EncData_Load" : "FIELD_ENCOUNT_ST_LoadEncountData";
        const int f = watch.Find(fn);
        if (f < 0) return;
        Nds = &nds;
        names.push_back(fn);
        watch.AddOnEnter(f, &VersionExclusives::ReadGen5, this);
        Kind = Gen5;
        Size = 0xE8; // EncData (pokebw2 include/field/encounter.h)
        // Each version's exclusives with their counterparts (species numbers; the forms are 0):
        // the first of each pair is Black's or Black 2's. Checked against both versions' tables:
        // every slot where an exclusive differs from the other version holds its counterpart
        // there, and none of these is in a slot where the versions agree. (Pokemon that both
        // versions have in different places, Sawk and Throh, Karrablast and Shelmet, Basculin's
        // two forms, stay as they are.)
        static const uint16_t bw1[][2] = {
            { 14, 11 }, { 15, 12 }, { 198, 200 }, { 367, 368 }, { 430, 429 }, { 546, 548 }, { 547, 549 },
            { 574, 577 }, { 575, 578 }, { 576, 579 }, { 629, 627 }, { 630, 628 },
        };
        static const uint16_t bw2[][2] = {
            { 214, 127 }, { 240, 239 }, { 325, 322 }, { 326, 323 }, { 367, 368 }, { 427, 300 }, { 428, 301 },
            { 546, 548 }, { 547, 549 }, { 574, 577 }, { 575, 578 }, { 576, 579 }, { 629, 627 },
        };
        const bool white = code == "IRAO" || code == "IRDO";
        Pairs.clear();
        if (b2w2)
            for (const auto& p : bw2) Pairs.push_back({ p[white ? 1 : 0], p[white ? 0 : 1] });
        else
            for (const auto& p : bw1) Pairs.push_back({ p[white ? 1 : 0], p[white ? 0 : 1] });
        fprintf(stderr, "[exclusives] %zu exclusives paired with the other version's\n", Pairs.size());
    }

    // After each emulated frame, on the emulation's thread.
    void DoFrame()
    {
        if (Kind == None || !Table) return;
        uint8_t now[MaxSize];
        memcpy(now, Ram(Table), Size);
        Waited++;
        if (Pending)
        {
            if (Waited > 120)
            {
                if (Log) fprintf(stderr, "[exclusives] the table at %08X was not recognised\n", Table);
                Table = 0;
                return;
            }
            if (Kind == Gen4)
            {
                // The table read for the map just entered: found in the version's own archive.
                int found = -1;
                for (size_t i = 0; i < Own.size(); i++)
                    if (Own[i].size() >= Size && memcmp(Own[i].data(), now, Size) == 0)
                    {
                        // (Maps with the same table in one version and different ones in the
                        // other cannot be told apart: left as they are.)
                        if (found >= 0 && memcmp(Other[i].data(), Other[found].data(), Size) != 0) { Table = 0; return; }
                        if (found < 0) found = (int)i;
                    }
                if (found < 0) return; // (not read yet)
                memcpy(Mine, Own[found].data(), Size);
                memcpy(Theirs, Other[found].data(), Size);
            }
            else
            {
                // The table as read, and the same with each exclusive its counterpart. (Should it
                // still be being read, it is taken again below.)
                memcpy(Mine, now, Size);
                memcpy(Theirs, now, Size);
                for (uint32_t at = 8; at + 4 <= Size; at += 4)
                {
                    uint16_t sp;
                    memcpy(&sp, &Theirs[at], 2);
                    for (const Pair& p : Pairs)
                        if (sp == p.mine) { memcpy(&Theirs[at], &p.theirs, 2); break; }
                }
            }
            Pending = false;
            Units.clear();
            for (const Unit& u : AllUnits())
                if (memcmp(Mine + u.at, Theirs + u.at, u.size) != 0) Units.push_back(u);
            if (Log)
            {
                fprintf(stderr, "[exclusives] table at %08X: %zu slots differ:", Table, Units.size());
                for (const Unit& u : Units)
                {
                    const uint32_t sp = Kind == Gen4 ? u.at + u.size - 2 : u.at; // (the species)
                    fprintf(stderr, " %02X:%u/%u", u.at, Mine[sp] | Mine[sp + 1] << 8, Theirs[sp] | Theirs[sp + 1] << 8);
                }
                fprintf(stderr, "\n");
            }
            if (Units.empty() && Kind == Gen4) { Table = 0; return; }
        }
        // Still the map's table? Every byte as in it, or in the other version's where they differ.
        bool same = true;
        uint8_t differs[MaxSize] = {};
        for (const Unit& u : Units)
        {
            memset(&differs[u.at], 1, u.size);
            if (memcmp(now + u.at, Mine + u.at, u.size) != 0 && memcmp(now + u.at, Theirs + u.at, u.size) != 0) same = false;
        }
        for (uint32_t i = 0; i < Size && same; i++)
            if (!differs[i] && now[i] != Mine[i]) same = false;
        if (!same)
        {
            // (Black and White's table taken while it was still being read: taken again, for a
            // couple of seconds. Otherwise the game has let it go.)
            if (Kind == Gen5 && Waited <= 120) Pending = true;
            else Table = 0;
            return;
        }
        for (const Unit& u : Units)
        {
            if (!on && memcmp(now + u.at, Mine + u.at, u.size) == 0) continue;
            const bool theirs = on && (Rng() & 1);
            memcpy(Ram(Table + u.at), (theirs ? Theirs : Mine) + u.at, u.size);
        }
    }

private:
    enum { None, Gen4, Gen5 } Kind = None;
    static constexpr uint32_t MaxSize = 0xE8;
    uint32_t Size = 0;
    struct Unit { uint32_t at, size; };
    struct Pair { uint16_t mine, theirs; };
    melonDS::NDS* Nds = nullptr;
    std::vector<std::vector<uint8_t>> Own, Other;
    std::vector<Pair> Pairs;
    uint32_t Table = 0;
    bool Pending = false;
    int Waited = 0;
    uint8_t Mine[MaxSize] = {}, Theirs[MaxSize] = {};
    std::vector<Unit> Units;
    // (Seeded from the clock: std::random_device is not on every system this runs on.)
    std::minstd_rand Rng { (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() };
    const bool Log = getenv("SSR_EXCLUSIVES_LOG") != nullptr;

    // The Pokemon in a table. SoulSilver and HeartGold: the land slots' species (morning, day and
    // night: their levels are shared), the radio's Hoenn and Sinnoh sounds, then the surfing, Rock
    // Smash and fishing slots (levels and species together), and the swarms' and night fishing's
    // species. Black and White: after the rates, 56 slots of species and form, then levels.
    const std::vector<Unit>& AllUnits() const
    {
        static std::vector<Unit> gen4, gen5;
        if (gen4.empty())
        {
            for (uint32_t i = 0; i < 36; i++) gen4.push_back({ 0x14 + i * 2, 2 });
            for (uint32_t i = 0; i < 4; i++) gen4.push_back({ 0x5C + i * 2, 2 });
            for (uint32_t i = 0; i < 5 + 2 + 5 * 3; i++) gen4.push_back({ 0x64 + i * 4, 4 });
            for (uint32_t i = 0; i < 4; i++) gen4.push_back({ 0xBC + i * 2, 2 });
            for (uint32_t i = 0; i < 56; i++) gen5.push_back({ 8 + i * 4, 2 });
        }
        return Kind == Gen4 ? gen4 : gen5;
    }

    uint8_t* Ram(uint32_t addr) { return Nds->MainRAM + (addr & Nds->MainRAMMask); }

    void Noted(uint32_t ptr)
    {
        if ((ptr >> 24) != 0x02) return;
        Table = ptr;
        Pending = true;
        Waited = 0;
    }

    static void Read(void* user, melonDS::ARMv5* cpu)
    {
        auto* self = static_cast<VersionExclusives*>(user);
        if (self->Log) fprintf(stderr, "[exclusives] map %u's table read to %08X\n", cpu->R[1], cpu->R[0]);
        self->Noted(cpu->R[0]);
    }

    static void ReadGen5(void* user)
    {
        auto* self = static_cast<VersionExclusives*>(user);
        const uint32_t ptr = self->Nds->ARM9.R[0];
        if (self->Log) fprintf(stderr, "[exclusives] zone %u's table read to %08X\n", self->Nds->ARM9.R[2] & 0xFFFF, ptr);
        self->Noted(ptr);
    }

    // A file of the ROM's file system, by its path, and the members of that NARC archive.
    static bool ReadNarc(const std::string& path, const std::string& name, std::vector<std::vector<uint8_t>>& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        auto at = [&](uint32_t off, void* dst, size_t n) { return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n; };
        bool ok = false;
        uint32_t h[4];
        std::vector<uint8_t> fnt;
        if (at(0x40, h, 16) && h[1] < 0x200000)
        {
            fnt.resize(h[1]);
            const uint32_t fatOff = h[2];
            if (at(h[0], fnt.data(), fnt.size()))
            {
                const int id = FindFile(fnt, 0xF000, name);
                uint32_t range[2];
                if (id >= 0 && at(fatOff + (uint32_t)id * 8, range, 8) && range[1] > range[0] && range[1] - range[0] < 0x400000)
                {
                    std::vector<uint8_t> file(range[1] - range[0]);
                    ok = at(range[0], file.data(), file.size()) && SplitNarc(file, out);
                }
            }
        }
        fclose(f);
        return ok;
    }

    // The file id of `path` ("a/0/3/7") in the file name table, from directory `dir`.
    static int FindFile(const std::vector<uint8_t>& fnt, uint32_t dir, const std::string& path)
    {
        const size_t slash = path.find('/');
        const std::string part = path.substr(0, slash);
        const uint32_t e = (dir & 0xFFF) * 8;
        if (e + 8 > fnt.size()) return -1;
        uint32_t off;
        uint16_t first;
        memcpy(&off, &fnt[e], 4);
        memcpy(&first, &fnt[e + 4], 2);
        int id = first;
        for (size_t p = off; p < fnt.size();)
        {
            const uint8_t t = fnt[p++];
            if (!t) break;
            const size_t n = t & 0x7F;
            if (p + n > fnt.size()) break;
            const std::string nm((const char*)&fnt[p], n);
            p += n;
            if (t & 0x80)
            {
                if (p + 2 > fnt.size()) break;
                uint16_t sub;
                memcpy(&sub, &fnt[p], 2);
                p += 2;
                if (slash != std::string::npos && nm == part) return FindFile(fnt, sub, path.substr(slash + 1));
            }
            else
            {
                if (slash == std::string::npos && nm == part) return id;
                id++;
            }
        }
        return -1;
    }

    // NARC: a header, then BTAF (each member's start and end in GMIF), BTNF and GMIF.
    static bool SplitNarc(const std::vector<uint8_t>& f, std::vector<std::vector<uint8_t>>& out)
    {
        if (f.size() < 0x1C || memcmp(f.data(), "NARC", 4) != 0) return false;
        uint16_t headerSize;
        memcpy(&headerSize, &f[0x0C], 2);
        size_t p = headerSize;
        uint32_t fatSize, count;
        if (p + 12 > f.size() || memcmp(&f[p], "BTAF", 4) != 0) return false;
        memcpy(&fatSize, &f[p + 4], 4);
        memcpy(&count, &f[p + 8], 4);
        const size_t fat = p + 12;
        p += fatSize;
        if (p + 8 > f.size() || memcmp(&f[p], "BTNF", 4) != 0) return false;
        uint32_t fntSize;
        memcpy(&fntSize, &f[p + 4], 4);
        p += fntSize;
        if (p + 8 > f.size() || memcmp(&f[p], "GMIF", 4) != 0) return false;
        const size_t img = p + 8;
        if (fat + count * 8 > f.size()) return false;
        out.clear();
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t s, e;
            memcpy(&s, &f[fat + i * 8], 4);
            memcpy(&e, &f[fat + i * 8 + 4], 4);
            if (e < s || img + e > f.size()) return false;
            out.emplace_back(f.begin() + img + s, f.begin() + img + e);
        }
        return true;
    }
};
