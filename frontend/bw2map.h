// Pokemon Black 2 and White 2 (US), Black and White (US/Europe): more of the map loaded beside the
// player, for the wide view.
//
// The game keeps a small window of its map's 32x32-tile chunks loaded around the player (most
// outdoor maps 2x2 chunks, chosen by which half of a chunk the player stands in) and draws only
// those. On the DS's own screen that is always enough; a wide view can see past it, where the land
// simply stops (beside the cliff at Aspertia's lookout). The window's size comes from a table in
// the field overlay, MAP_CONFIGS (pokebw2: src/ov036/fieldmap.c), read when a map is set up
// (SetupLoadZoneMapTypeData), and the overworld's memory is sized from it before that, when the
// overworld starts (GetFieldmapZoneHeapSize). Just before either, the table is changed in memory: a window two
// chunks wide becomes three, centred on the player's chunk, so there is a chunk on each side, and
// the map's memory (the field heap, made from the same table) grows by what the extra chunks use.
//
// Development aid: SSR_B2_MAP=0 leaves the table alone; SSR_B2_MAP_LOG=1 prints it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "NDS.h"
#include "funcwatch.h"

class Bw2Map
{
public:
    bool on = false;

    // Whether this game is one of the versions below (whether WIDE MAP LOADING applies).
    bool Supported() const { return V != nullptr; }

    // Whether the console should have 8 MB (the ROM is Black 2 or White 2, US, and this is not
    // turned off).
    static bool WantsLargeRAM(const std::vector<uint8_t>& rom)
    {
        const char* env = getenv("SSR_B2_MAP");
        if (rom.size() <= 0x10 || (env && !strcmp(env, "0"))) return false;
        char code[5] = {};
        memcpy(code, &rom[0x0C], 4);
        return Find(code) != nullptr;
    }

    // `code`: the ROM's game code. Only the versions below: the addresses are theirs.
    void Init(const char* code, FuncWatch& watch, std::vector<std::string>& names, melonDS::NDS& nds, bool enabled)
    {
        const char* env = getenv("SSR_B2_MAP");
        V = Find(code);
        if (!V || !enabled || (env && !strcmp(env, "0"))) return;
        Nds = &nds;
        // (Both read the table: the overworld's memory is sized from it first, when the overworld
        // starts, then the map is set up from it.)
        const int heap = watch.Find("GetFieldmapZoneHeapSize"), setup = watch.Find("SetupLoadZoneMapTypeData");
        if (heap < 0 || setup < 0) return;
        names.push_back("GetFieldmapZoneHeapSize");
        names.push_back("SetupLoadZoneMapTypeData");
        const int app = watch.Find("GFLAppInit");
        if (app >= 0)
        {
            names.push_back("GFLAppInit");
            watch.AddOnEnter(app, &Bw2Map::AppInit, this);
        }
        watch.AddOnEnter(heap, &Bw2Map::Entered, this);
        watch.AddOnEnter(setup, &Bw2Map::Entered, this);
        on = true;
        fprintf(stderr, "[bw2map] wider map windows on\n");
    }

private:
    // Where each version has MAP_CONFIGS (in the field overlay) and HEAP_SETUP_NTR (in the main
    // binary), from the decompilation's symbols for that version (pokebw2: config/b2_us, w2_us).
    // White 2's code is Black 2's, a little further along in memory.
    // Black and White have the same table (21 entries) in their field overlay (21), found by its
    // contents, and their heap setup in the main binary with a smaller user heap (0x199500).
    struct Version { const char* code; uint32_t table, heapSetup, heapUser, count; };
    static const Version* Find(const char* code)
    {
        static const Version versions[] = {
            { "IREO", 0x021ca01c, 0x0208ee68, 0x1a3e00, 29 }, // Black 2
            { "IRDO", 0x021ca050, 0x0208ee94, 0x1a3e00, 29 }, // White 2
            { "IREF", 0x021ca058, 0x0208ee78, 0x1a3e00, 29 }, // Black 2 FR
            { "IRDF", 0x021ca06c, 0x0208eea4, 0x1a3e00, 29 }, // White 2 FR
            { "IRBO", 0x021d3700, 0x0209d8b0, 0x199500, 21 }, // Black
            { "IRAO", 0x021d3718, 0x0209d8c8, 0x199500, 21 }, // White
            { "IRBF", 0x021d3678, 0x0209d830, 0x199500, 21 }, // Black FR
            { "IRAF", 0x021d3690, 0x0209d848, 0x199500, 21 }, // White FR
        };
        for (const Version& v : versions)
            if (!strcmp(code, v.code)) return &v;
        return nullptr;
    }

    // MAP_CONFIGS: Version::count entries of 72 bytes (FieldG3DMapperConfig, 60 bytes, then the control
    // table, whether the map has a matrix, and the heap's size).
    static constexpr uint32_t Size = 72;
    static constexpr uint32_t OffDiameterX = 8, OffDiameterZ = 10, OffBuffer = 52, OffHeap = 68;
    // Besides its buffer each chunk takes its own state and lists from the heap (a few KB;
    // generously).
    static constexpr uint32_t ChunkOverhead = 0x3000;

    // OS_ARENA_MAIN's low and high ends (NitroSDK's arena information, HW_ARENA_INFO_BUF), and
    // HEAP_SETUP_NTR's heap 1 (pokebw2: src/system/gfl_use.c): Version::heapUser bytes (Black 2:
    // 0x1a3e00, 0x48ae00 on a DSi; Black: 0x199500, 0x486000 on a DSi).
    static constexpr uint32_t ArenaLo = 0x027FFDA0, ArenaHi = 0x027FFDC4;
    static constexpr uint32_t ArenaStart = 0x02400000, ArenaEnd = 0x027E0000;
    static constexpr uint32_t HeapUserExtra = 0x180000;

    const Version* V = nullptr;
    melonDS::NDS* Nds = nullptr;
    bool logged = false;

    uint8_t* Ram(uint32_t addr) { return Nds->MainRAM + (addr & Nds->MainRAMMask); }
    uint16_t Read16(uint32_t a) { uint16_t v; memcpy(&v, Ram(a), 2); return v; }
    uint32_t Read32(uint32_t a) { uint32_t v; memcpy(&v, Ram(a), 4); return v; }
    void Write16(uint32_t a, uint16_t v) { memcpy(Ram(a), &v, 2); }
    void Write32(uint32_t a, uint32_t v) { memcpy(Ram(a), &v, 4); }

    static void Entered(void* user) { static_cast<Bw2Map*>(user)->Patch(); }
    static void AppInit(void* user) { static_cast<Bw2Map*>(user)->Arena(); }

    // With 8 MB, as the game starts (GFLAppInit, before it makes its heaps from the main
    // arena): the arena moved (below 0x027E0000, where the ARM7 has its own part of main RAM) to the upper 4 MB, and the game's own heap (heap 1, HEAPID_USER, which
    // the overworld's memory comes from) made bigger, as the game makes it on a DSi. The lower
    // arena is left unused.
    void Arena()
    {
        if (Nds->MainRAMMask != 0x7FFFFF || Read32(V->heapSetup + 8) != V->heapUser) return;
        // The game's protection region for main RAM (region 1, set up by the start-up code for 4 MB:
        // 0x0200002b) made 8 MB, or the first use of the upper half faults.
        if (Nds->ARM9.PU_Region[1] != 0x0200002b) return;
        Nds->ARM9.CP15Write(0x610, 0x0200002d);
        Write32(ArenaLo, ArenaStart);
        Write32(ArenaHi, ArenaEnd);
        Write32(V->heapSetup + 8, V->heapUser + HeapUserExtra);
        fprintf(stderr, "[bw2map] 8 MB: the game's heap 0x%x bytes at 0x%x\n", V->heapUser + HeapUserExtra, ArenaStart);
    }

    void Patch()
    {
        // (The table is in the field overlay: its first entry, as the game has it, or already
        // changed, says it is there.)
        if (Read32(V->table) != 0x200000 || Read32(V->table + 4) != 0x400000) return;
        // (Only with the bigger heap: there is no room in the DS's own 4 MB, and a save state made
        // without it keeps the game's own windows.)
        if (Read32(V->heapSetup + 8) != V->heapUser + HeapUserExtra) return;
        static const bool log = getenv("SSR_B2_MAP_LOG") != nullptr;
        if (log) fprintf(stderr, "[bw2map] table read for zone %u\n", Nds->ARM9.R[0] & 0xFFFF);
        for (uint32_t i = 0; i < V->count; i++)
        {
            const uint32_t e = V->table + i * Size;
            const uint16_t dx = Read16(e + OffDiameterX), dz = Read16(e + OffDiameterZ);
            const uint32_t buffer = Read32(e + OffBuffer), heap = Read32(e + OffHeap);
            if (log && !logged)
                fprintf(stderr, "[bw2map] %2u: %ux%u chunks of 0x%x, heap 0x%x\n", i, dx, dz, buffer, heap);
            if (dx != 2 || dz > 3 || buffer > 0x20000 || heap > 0x200000) continue;
            Write16(e + OffDiameterX, 3);
            Write32(e + OffHeap, heap + dz * (buffer + ChunkOverhead));
        }
        logged = true;
    }
};
