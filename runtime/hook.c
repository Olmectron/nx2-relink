// A port's hooks: sources of its user's built into it (tools/build.py --also), each with a function that changes slots of the
// game's tables of functions before the game starts. nx2_hook.h is what such a source sees; this is behind it.
#include <stdio.h>
#include <string.h>

#include "nx2.h"
#include "nx2_hook.h"

static const Nx2Module *module_named(const char *name) {
    for (unsigned k = 0; name && k < nx2_module_count; k++)
        if (!strcmp(nx2_modules[k].name, name)) return &nx2_modules[k];
    return NULL;
}

void *nx2_hook_at(const char *module, uint64_t offset) {
    const Nx2Module *m = module_named(module);
    return m && offset < m->end ? (void *)nx2_host(m, offset) : NULL;
}

int nx2_hook_swap(const char *module, uint64_t slot, uint64_t expect, uint32_t first, void *mine) {
    const Nx2Module *m = module_named(module);
    if (!m) {
        nx2_log("[hooks] %s+%x: the game has no module of that name: left as it is\n", module ? module : "?", slot);
        return 0;
    }
    // (a slot is of the module's data, which is writable, and whole in it; the function it holds is of the module's code)
    if (!mine || (slot & 7) || slot < m->data_off || slot + 8 > m->end || expect >= m->data_off) {
        nx2_log("[hooks] %s+%x: not a slot of the module's data holding a function of its code (%x): left as it is\n", module, slot, expect);
        return 0;
    }
    u64 *p = (u64 *)nx2_host(m, slot);
    const u8 *was = nx2_host(m, expect);
    if (*p != (u64)was) {
        nx2_log("[hooks] %s+%x: it doesn't hold %s+%x, as the game's build this was written for does: left as it is\n", module, slot, module, expect);
        return 0;
    }
    u32 word;
    memcpy(&word, was, 4);
    if (first && word != first) {
        nx2_log("[hooks] %s+%x: the function there begins with %x, not %x: another build of the game: left as it is\n", module, expect, (u64)word, (u64)first);
        return 0;
    }
    *p = (u64)mine;
    return 1;
}

int nx2_hook_on(const char *name) {
    // (a name, not a path: nothing of the card but that folder is asked about)
    if (!name || !*name || strlen(name) > 64 || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) return 0;
    char rel[96];
    snprintf(rel, sizeof rel, "mods/Code/%s", name);
    char path[sizeof NX2_DIR + sizeof rel];
    snprintf(path, sizeof path, NX2_DIR "%s", rel);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

uint64_t nx2_hook_title(void) {
#ifdef NX2_GAME_TITLE
    return NX2_GAME_TITLE;
#else
    return 0;
#endif
}

const char *nx2_hook_version(void) {
#ifdef NX2_GAME_BUILD
    return NX2_GAME_BUILD;
#else
    return "";
#endif
}

// ---- Hook files: the same kind of source built as a file of its own (tools/hooks.py: <name>.nxh), read from the port's folder
// under mods/Code/ and mapped as code, so that a mod can carry code with nothing built again.
//
// How it is mapped: as the game's whole modules are (whole.c). A homebrew program may not map code itself on this console; the
// loader service that started it does, and takes one more image: a page of the port's with the header that service reads, then
// the file's code, read-only data and data, and its bss. That service maps eight images for a program, the game's whole
// modules among them: a game that leaves none free has its hook files refused, which is said.
//
// What is checked before anything of a file runs: its form and sizes, and that it was made for this game and this version of
// it (a file's places are one build's alone). A file that fails any of it is left out, with one line saying why.
#ifdef __SWITCH2__
#include <dirent.h>
#include <stdlib.h>
#include <switch.h>

typedef struct {
    char magic[4];
    u32 format;
    u64 title;
    char version[32];
    u32 size, image, ro, data, bss, entry, relocs, unused;  // (ro, data, entry: from the start of the page before the image)
} NxhHeader;
#define NXH_MOST (4u << 20)
#define NXH_FILES 4

static const NxhApi api = {sizeof(NxhApi), NXH_API_VERSION, nx2_log, nx2_hook_at, nx2_hook_swap, nx2_hook_on, nx2_hook_title, nx2_hook_version};

// 0: the file's functions were called. Else why not, in words (said by the caller).
static const char *nxh_load(const char *path, const char *name) {
    const u64 page = 0x1000;
    FILE *f = fopen(path, "rb");
    if (!f) return "it can't be opened";
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    NxhHeader h;
    if (size < (long)sizeof h || size > (long)NXH_MOST || fread(&h, 1, sizeof h, f) != sizeof h) {
        fclose(f);
        return "not a hook file (its size)";
    }
    const char *why = NULL;
    const u64 end = page + h.image;
    if (memcmp(h.magic, "NXH1", 4)) why = "not a hook file";
    else if (h.format != 1) why = "made in a form this port doesn't know: a later tool's";
    else if (h.size != (u32)size || (u64)sizeof h + h.image + 8ull * h.relocs != (u64)size) why = "its size isn't what it says";
    else if (!h.image || (h.image & (page - 1)) || (h.ro & (page - 1)) || (h.data & (page - 1)) || (h.bss & (page - 1)) || h.bss > (16u << 20))
        why = "its parts don't begin where pages do";
    else if (h.ro <= page || h.ro > h.data || h.data > end || h.entry < page || h.entry >= h.ro || (h.entry & 3)) why = "its parts aren't in order";
    else if (!nx2_hook_title()) why = "this port doesn't know which game it is of (build it again with this version of the tool)";
    else if (h.title != nx2_hook_title()) why = "it was made for another game";
    else if (memchr(h.version, 0, sizeof h.version) == NULL || strcmp(h.version, nx2_hook_version())) why = "it was made for another version of the game";
    if (why) {
        fclose(f);
        return why;
    }
    const u64 nro_size = page + h.image, bss_size = h.bss ? h.bss : page;
    u8 *nro = aligned_alloc(page, nro_size), *bss = aligned_alloc(page, bss_size);
    u32 *relocs = h.relocs ? malloc(8ull * h.relocs) : NULL;
    if (!nro || !bss || (h.relocs && !relocs) || fread(nro + page, 1, h.image, f) != h.image || (h.relocs && fread(relocs, 8, h.relocs, f) != h.relocs)) {
        fclose(f);
        free(nro), free(bss), free(relocs);
        return "it couldn't be read whole";
    }
    fclose(f);
    // (every address the file asks to have settled: a place of its data, pointing into the file itself)
    for (u32 k = 0; k < h.relocs; k++)
        if (relocs[2 * k] < h.data || (u64)relocs[2 * k] + 8 > end || (relocs[2 * k] & 7) || relocs[2 * k + 1] < page || relocs[2 * k + 1] >= end + bss_size) {
            free(nro), free(bss), free(relocs);
            return "it asks for an address outside itself to be settled";
        }
    memset(nro, 0, page), memset(bss, 0, bss_size);
    u32 *w = (u32 *)nro;
    w[4] = 0x304F524E;  // "NRO0"
    w[6] = (u32)nro_size;
    w[8] = 0, w[9] = h.ro;                    // the code (with the page before it)
    w[10] = h.ro, w[11] = h.data - h.ro;      // the read-only data
    w[12] = h.data, w[13] = (u32)(end - h.data);  // the data
    w[14] = (u32)bss_size;
    memcpy(nro + 0x40, "nx2-hooks:", 10), strncpy((char *)nro + 0x40 + 10, name, 21);  // (a name of its own where an NRO has its number)
    const Handle session = smGetServiceOverride(smEncodeName("ldr:ro"));
    if (session == INVALID_HANDLE) {
        free(nro), free(bss), free(relocs);
        return "the loader service's session wasn't handed to this program: code can't be mapped";
    }
    Service ro;
    serviceCreate(&ro, session);
    ro.own_handle = 0;
    const struct {
        u64 pid, nro_address, nro_size, bss_address, bss_size;
    } in = {0, (u64)nro, nro_size, (u64)bss, bss_size};
    u64 at = 0;
    nx2_log("[hooks] %s: to be mapped by the loader service: %u KB, %u KB of bss\n", name, nro_size >> 10, bss_size >> 10);
    nx2_log_flush();
    const Result rc = serviceDispatchInOut(&ro, 0, in, at, .in_send_pid = true);
    if (R_FAILED(rc) || !at) {
        // (the memory stays as it is: what the service did with it before refusing isn't known)
        free(relocs);
        nx2_log("[hooks] %s: the loader service refused it (%x): it maps eight images for a program, the game's whole modules among them\n", name, (u64)rc);
        return "it couldn't be mapped";
    }
    for (u32 k = 0; k < h.relocs; k++) *(u64 *)(at + relocs[2 * k]) = at + relocs[2 * k + 1];
    free(relocs);
    nx2_log("[hooks] %s: mapped at %p; its functions are called\n", name, at);
    nx2_log_flush();
    const int said = ((int (*)(const NxhApi *))(at + h.entry))(&api);
    return said ? "it was made for a later port than this one: nothing of it was done" : NULL;
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void nxh_load_all(void) {
    const char *folder = NX2_DIR "mods/Code";
    DIR *d = opendir(folder);
    if (!d) return;
    static char names[NXH_FILES][64];
    unsigned count = 0, more = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        const size_t n = strlen(e->d_name);
        if (n < 5 || n >= sizeof names[0] || strcmp(e->d_name + n - 4, ".nxh")) continue;
        if (count < NXH_FILES) strcpy(names[count++], e->d_name);
        else more++;
    }
    closedir(d);
    qsort(names, count, sizeof names[0], by_name);
    if (more) nx2_log("[hooks] %u more hook files than the %u a port takes: left out\n", (u64)more, (u64)NXH_FILES);
    for (unsigned k = 0; k < count; k++) {
        static char path[400];
        snprintf(path, sizeof path, "%s/%s", folder, names[k]);
        const char *why = nxh_load(path, names[k]);
        if (why) nx2_log("[hooks] %s: NOT USED: %s\n", names[k], why);
        else nx2_log("[hooks] %s: in use (code from the card, made for this game and version)\n", names[k]);
    }
}
#else
static void nxh_load_all(void) {}
#endif

// Every source's function, in the order the sources were given to the build: each put its address in the section "nx2_hooks"
// (NX2_HOOKS), which the linker gives a start and an end. A port with none has no such section, and the two are null.
extern void (*__start_nx2_hooks[])(void) __attribute__((weak));
extern void (*__stop_nx2_hooks[])(void) __attribute__((weak));

void nx2_hooks_install(void) {
    // (the first way, kept: one source that defines this by name)
    if (nx2_port_hooks) {
        nx2_log("[hooks] a source of the port's own puts its changes in (nx2_port_hooks)\n");
        nx2_port_hooks();
    }
    void (**at)(void) = __start_nx2_hooks, (**end)(void) = __stop_nx2_hooks;
    if (at && end && at < end) {
        nx2_log("[hooks] %u source(s) of the port's own put their changes in\n", (u64)(end - at));
        for (; at < end; at++)
            if (*at) (*at)();
    }
    nxh_load_all();  // (after the port's own: a slot one of those took is found taken by a file's, which leaves it)
}
