// A Switch 2 game's own NVN, watched: the game draws through its SDK's driver (<folder>/native), and what it asks of the
// graphics processor goes by where this port can't see. A game was closed by the system between its twelfth second and
// its eighteenth with a black screen and no word (2026-11-24): to tell whether that follows its first work for the graphics
// processor, the driver's functions that hand work over (a queue's) and show a picture (a window's) are counted here as the game
// calls them, and with <folder>/nogpu there the lists of commands are never handed over and no picture is shown.
#ifdef __SWITCH2__
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);

typedef u64 (*Fn4)(u64, u64, u64, u64);
typedef void *(*Loader)(const char *);
typedef void *(*GetProc)(void *, const char *);

static GetProc real_get;
static int nogpu;
enum { WATCHED = 24 };
static struct {
    char name[56];
    Fn4 real;
    u32 calls;
    int held;  // (not done with <folder>/nogpu there)
} watched[WATCHED];
static u32 watched_count;

// Each watched call, as it begins and as it ends, kept in a ring that netwatch.c sends to a computer: the call a thread was in when
// the game ended is the one with a beginning and no end.
struct nx2_nvn_mark nx2_nvn_ring[NX2_NVN_RING];
u32 nx2_nvn_ring_in;
static void mark(u32 k, u32 n, u32 phase, u64 a, u64 b, u64 r) {
    const u32 at = __atomic_fetch_add(&nx2_nvn_ring_in, 1, __ATOMIC_RELAXED) % NX2_NVN_RING;
    u64 tls;
    __asm__("mrs %0, tpidrro_el0" : "=r"(tls));
    nx2_nvn_ring[at] = (struct nx2_nvn_mark){.thread = tls, .a = a, .b = b, .r = r, .call = n, .what = (u16)k, .phase = (u16)phase};
}

static u64 called(u32 k, u64 a, u64 b, u64 c, u64 d) {
    const u32 n = __atomic_add_fetch(&watched[k].calls, 1, __ATOMIC_RELAXED);
    mark(k, n, 0, a, b, 0);
    if (n <= 6 || (n & (n - 1)) == 0)
        nx2_log("[nvn] %s, call %u (%x, %x, %x, %x)%s\n", watched[k].name, (u64)n, a, b, c, d, watched[k].held ? ": not done (<folder>/nogpu)" : "");
    if (!strcmp(watched[k].name, "nvnMemoryPoolInitialize") && nx2_ipclog) {
        const u64 *w = (const u64 *)b;
        nx2_log("[nvn] pool %u: builder %x %x %x %x %x %x\n", (u64)n, w[0], w[1], w[2], w[3], w[4], w[5]);
    }
    if (watched[k].held) return 0;
    // (<folder>/nvnlock: one watched call at a time, whichever thread makes it. A try: the game ended with two of them under way.)
    static u64 owner;
    static u32 depth;
    u64 me;
    __asm__("mrs %0, tpidrro_el0" : "=r"(me));
    if (nx2_nvnlock && __atomic_load_n(&owner, __ATOMIC_ACQUIRE) != me) {
        u64 none = 0;
        while (!__atomic_compare_exchange_n(&owner, &none, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) none = 0, svcSleepThread(0);
    }
    if (nx2_nvnlock) depth++;
    const u64 r = watched[k].real(a, b, c, d);
    if (nx2_nvnlock && !--depth) __atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
    mark(k, n, 1, a, b, r);
    if (n <= 6) nx2_log("[nvn] %s, call %u: back (%x)\n", watched[k].name, (u64)n, r);
    // (the functions that make something answer whether they could: one that couldn't is always said, with what it was asked)
    if (!(r & 0xFF) && strstr(watched[k].name, "Initialize"))
        nx2_log("[nvn] %s, call %u, COULD NOT: (%x, %x, %x, %x) gave %x\n", watched[k].name, (u64)n, a, b, c, d, r);
    return r;
}
#define ENTRY(k) \
    static u64 entry##k(u64 a, u64 b, u64 c, u64 d) { return called(k, a, b, c, d); }
ENTRY(0) ENTRY(1) ENTRY(2) ENTRY(3) ENTRY(4) ENTRY(5) ENTRY(6) ENTRY(7) ENTRY(8) ENTRY(9) ENTRY(10) ENTRY(11)
ENTRY(12) ENTRY(13) ENTRY(14) ENTRY(15) ENTRY(16) ENTRY(17) ENTRY(18) ENTRY(19) ENTRY(20) ENTRY(21) ENTRY(22) ENTRY(23)
static const Fn4 entries[WATCHED] = {entry0,  entry1,  entry2,  entry3,  entry4,  entry5,  entry6,  entry7,  entry8,  entry9,  entry10, entry11,
                                     entry12, entry13, entry14, entry15, entry16, entry17, entry18, entry19, entry20, entry21, entry22, entry23};

static void *get_watched(void *device, const char *name) {
    void *p = real_get(device, name);
    // the queue's and the window's functions that take a few whole numbers or pointers, and nothing by value in a vector register
    const int queue = !strncmp(name, "nvnQueue", 8), window = !strncmp(name, "nvnWindow", 9);
    // (and what is made of the game's own memory and data: a pool of memory, a texture, a buffer, a program of shaders)
    const int made = !strcmp(name, "nvnMemoryPoolInitialize") || !strcmp(name, "nvnTextureInitialize") || !strcmp(name, "nvnBufferInitialize") ||
                     !strcmp(name, "nvnProgramInitialize") || !strcmp(name, "nvnProgramSetShaders") || !strcmp(name, "nvnSamplerInitialize") ||
                     !strcmp(name, "nvnCommandBufferInitialize") || !strcmp(name, "nvnTextureFinalize") || !strcmp(name, "nvnMemoryPoolFinalize");
    if (!p || !(queue || window || made)) return p;
    const int work = !strcmp(name, "nvnQueueSubmitCommands") || !strcmp(name, "nvnQueuePresentTexture") || !strcmp(name, "nvnQueuePresentTextureSync") ||
                     !strcmp(name, "nvnQueueFlush") || !strcmp(name, "nvnQueueFinish") || !strcmp(name, "nvnQueueFenceSync") ||
                     !strcmp(name, "nvnQueueInitialize") || !strcmp(name, "nvnWindowInitialize") || !strcmp(name, "nvnWindowAcquireTexture") || made;
    static u32 said;
    if (!work) {
        if (said++ < 80) nx2_log("[nvn] the game asks for %s\n", name);
        return p;
    }
    for (u32 k = 0; k < watched_count; k++)
        if (!strcmp(watched[k].name, name)) return (void *)entries[k];
    if (watched_count >= WATCHED) return p;
    const u32 k = watched_count++;
    strncpy(watched[k].name, name, sizeof watched[k].name - 1);
    watched[k].real = (Fn4)p;
    watched[k].held = ((nogpu & 1) && !strcmp(name, "nvnQueueSubmitCommands")) || ((nogpu & 2) && !strncmp(name, "nvnQueuePresentTexture", 22));
    nx2_log("[nvn] the game asks for %s: watched%s\n", name, watched[k].held ? ", and not done (<folder>/nogpu)" : "");
    return (void *)entries[k];
}

static void *loader_watched(const char *name) {
    static Loader real;
    if (!real) real = (Loader)nx2_symbol("nvnBootstrapLoader");
    void *p = real ? real(name) : NULL;
    nx2_log("[nvn] the game's own driver is asked for %s: %p\n", name, (u64)p);
    if (p && !strcmp(name, "nvnDeviceGetProcAddress")) {
        real_get = (GetProc)p;
        return (void *)get_watched;
    }
    return p;
}

// (gl2.c asks, for a game whose graphics are its own SDK's)
void *nx2_nvn_watch(const char *name) {
    if (strcmp(name, "nvnBootstrapLoader")) return NULL;
    FILE *f = fopen(NX2_DIR "nogpu", "rb");
    // (the file says which: "submit", "present", or anything else for both)
    if (f) {
        char what[16] = {0};
        fread(what, 1, sizeof what - 1, f);
        fclose(f);
        nogpu = !strncmp(what, "submit", 6) ? 1 : !strncmp(what, "present", 7) ? 2 : 3;
    }
    return (void *)loader_watched;
}
#endif
