// What a source of a port's user may use to change what the game does: "hooks". Such a source is built into the port
// (tools/build.py --also <file.c>, as many as wanted, none knowing of another), and each names the function that puts its
// changes in:
//
//     #include "nx2_hook.h"
//     static void install(void) {
//         if (!nx2_hook_on("my-change")) return;                       // <port>/mods/Code/my-change: a mod's switch for it
//         if (!nx2_hook_swap("main", 0x1234560, 0x654320, 0, mine)) return;
//         ...
//     }
//     NX2_HOOKS(install);
//
// That function is called once, with the game's modules in memory and their addresses settled, before anything of the game
// has run. The rules, which the helpers keep: no instruction of the game is changed, only a place in its writable data that
// holds a function's address (a slot of a table of functions), and only when that place holds what the source expects of
// the game's build it was written for. A game's update moves everything: a source's numbers are one build's alone.
#pragma once
#include <stdint.h>

// What a hook file loaded from the card is handed (below): the same functions, as a table. A source need not know of it.
typedef struct NxhApi {
    uint32_t size, version;  // (of this table: a file made for a later one is refused)
    void (*log)(const char *fmt, ...);
    void *(*at)(const char *module, uint64_t offset);
    int (*swap)(const char *module, uint64_t slot, uint64_t expect, uint32_t first, void *mine);
    int (*on)(const char *name);
    uint64_t (*title)(void);
    const char *(*version_of_game)(void);
} NxhApi;
#define NXH_API_VERSION 1

#ifdef NXH
// The same source built as a file of its own (tools/hooks.py build: <name>.nxh), which a port loads from its folder's
// mods/Code/ as it starts, with nothing built again: the names below are the table's, and the C library isn't there
// (memcpy, memset, memcmp, strlen, strcmp and the functions of math.h are; nothing that opens a file or prints).
extern const NxhApi *nxh_api;
#define nx2_log nxh_api->log
#define nx2_hook_at nxh_api->at
#define nx2_hook_swap nxh_api->swap
#define nx2_hook_on nxh_api->on
#define nx2_hook_title nxh_api->title
#define nx2_hook_version nxh_api->version_of_game
#define NX2_HOOKS(fn)     static void (*nx2_hooks_of_##fn)(void) __attribute__((section("nx2_hooks"), used)) = fn
#else

// (how a source names its function; several sources, each with its own, are called in the order they were given to the build)
#define NX2_HOOKS(fn)     static void (*nx2_hooks_of_##fn)(void) __attribute__((section("nx2_hooks"), used)) = fn

// A line of the port's log (log.txt in its folder). %u %d %x %p %s, each argument a uint64_t or a pointer; no floating point.
void nx2_log(const char *fmt, ...);

// The address a place of one of the game's modules has in this program, by the module's name ("main", "sdk", "subsdk0", ...)
// and the place's offset from the module's start; null for a module the game hasn't, or an offset past its end.
void *nx2_hook_at(const char *module, uint64_t offset);

// A slot swapped: the eight bytes at `slot` of the module, if they are the address of the module's `expect` (the function the
// slot holds in the build the source was written for), are made `mine`'s. With `first` not 0, that function must also begin
// with that instruction. 1: done; 0: nothing touched, and the log says why. The function taken out is still
// nx2_hook_at(module, expect): `mine` calls it for what the game did there.
int nx2_hook_swap(const char *module, uint64_t slot, uint64_t expect, uint32_t first, void *mine);

// Whether <port>/mods/Code/<name> is there: the file a mod carries to turn a change on (the mods manager puts it there and
// takes it away with the mod).
int nx2_hook_on(const char *name);

// The game the port is of: its program's number, and its version as it shows it (0 and "" when the port wasn't told).
uint64_t nx2_hook_title(void);
const char *nx2_hook_version(void);  // (as the tool names it: "1.0.2", or "update 2" for a dump that has no more than that)
#endif
