// What the frontend may ask the recompiler runtime, kept apart from recomp_rt.h so that
// changing it does not rebuild the generated game code.
#pragma once

// A game application (bag, party menu, battle, ...) and the function it runs every frame.
struct RcApp
{
    const char* name;
    const char* section;
    unsigned execAddr;
    // The functions that start and end the application, run every frame while it starts
    // and ends (0 when it has none of its own).
    unsigned initAddr;
    const char* initSection;
    unsigned exitAddr;
    const char* exitSection;
};
extern const RcApp rc_apps[];
extern const unsigned rc_num_apps;

// True if recompiled code at this address, in this section, ran within the last `frames` frames.
bool rc_ran_recently(const char* section, unsigned addr, unsigned frames);

// Functions named in tools/focus_symbols.txt (same layout as RcApp: name, section, address).
extern const RcApp rc_focus_syms[];
extern const unsigned rc_num_focus_syms;

// Probes record the last frame in which a function was entered by recompiled code.
// The address must be the function's first instruction.
int rc_add_probe(const char* section, unsigned addr);
unsigned rc_probe_last_frame(int probe);
// Current value of the runtime's frame counter, for comparing with rc_probe_last_frame.
unsigned rc_current_frame();

// A hook: a probe that calls `hook` every time the function is entered, before its first
// instruction runs, with the ARM9 (its arguments in R[0] to R[3], which the hook may change).
// On the recompiled code, only where the function is entered through the dispatcher (a call
// from another chunk), as probes.
namespace melonDS { class ARMv5; }
typedef void (*RcHook)(void* user, melonDS::ARMv5* cpu);
int rc_add_hook(const char* section, unsigned addr, RcHook hook, void* user);

// Without the recompiled code (melonDS's interpreter or JIT: --interp, --jit), the same probes
// and the overlays' activity (rc_section_last_frame, rc_ran_recently) come from the core's
// watch on chosen addresses (melonDS/src/SsrWatch.h) instead of from the dispatch into
// recompiled chunks: every chunk start of the overlays, and every probe. The tables in gen/
// (chunks, and each section's code image to tell overlays at the same address apart) are all
// it needs. Returns false if the watch is taken already (a game with a function table).
namespace melonDS { class NDS; }
bool rc_watch_attach(melonDS::NDS& nds);

// Tables without the game's code (tools/soulsilver/tables_only.py): they hold addresses only,
// and the overlays' code, which the watch compares to tell overlays at the same address apart,
// is read from the player's own ROM when the program starts. The same tables serve games
// whose code is SoulSilver's with some functions moved (HeartGold): per such game, the
// stretches of SoulSilver addresses that are shifted in it.
struct RcPortableShift
{
    const char* section;
    unsigned from, to; // SoulSilver addresses, `to` not included
    int shift;
};
struct RcPortableGame
{
    const char* code;  // the ROM header's game code, e.g. "IPGE"
    const char* name;
    const RcPortableShift* shifts;
    unsigned numShifts;
};
struct RcPortable
{
    const int* overlayOf; // per section: its overlay's number in the ROM, -1 for none (main, ITCM, BIOS)
    const RcPortableGame* games;
    unsigned numGames;
};
// Called by the tables as the program starts (nothing else needs to know about them).
bool rc_register_portable(const RcPortable* tables);
// Whether the tables are waiting for the code from the ROM (call rc_portable_load first).
bool rc_portable_needed();
// Reads the overlays' code through `overlay` (its bytes and size, or null when the ROM has no
// such overlay) for a ROM with game code `code`. Returns the game's name, or null if the
// tables do not know that game (then nothing is watched). Before rc_watch_attach and probes.
const char* rc_portable_load(const char code[4], const unsigned char* (*overlay)(void* user, int id, unsigned* size), void* user);
