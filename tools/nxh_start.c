// What every hook file (.nxh, tools/hooks.py) begins with: the function a port calls after mapping the file, and the few
// functions of the C library a source, or the compiler on its behalf, may call. The C library itself isn't linked.
#include <stddef.h>
#include <stdint.h>

#define NXH
#include "nx2_hook.h"

const NxhApi *nxh_api;

extern void (*__start_nx2_hooks[])(void) __attribute__((weak));
extern void (*__stop_nx2_hooks[])(void) __attribute__((weak));

// 0: every source's function was called. Not 0: the port's table is older than the one this was made for; nothing was done.
__attribute__((section(".text.nxh_install"), used)) int nxh_install(const NxhApi *api) {
    if (!api || api->version < NXH_API_VERSION || api->size < sizeof(NxhApi)) return 1;
    nxh_api = api;
    for (void (**at)(void) = __start_nx2_hooks; at && at < __stop_nx2_hooks; at++)
        if (*at) (*at)();
    return 0;
}

void *memcpy(void *to, const void *from, size_t n) {
    uint8_t *t = to;
    const uint8_t *f = from;
    while (n--) *t++ = *f++;
    return to;
}
void *memmove(void *to, const void *from, size_t n) {
    uint8_t *t = to;
    const uint8_t *f = from;
    if (t < f)
        while (n--) *t++ = *f++;
    else
        while (n--) t[n] = f[n];
    return to;
}
void *memset(void *to, int c, size_t n) {
    volatile uint8_t *t = to;  // (volatile: or the compiler makes this loop a call to memset)
    while (n--) *t++ = (uint8_t)c;
    return to;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++)
        if (*a != *b || !*a) return (uint8_t)*a - (uint8_t)*b;
    return 0;
}
// (math.h's functions set this where the C library keeps it for each thread; here it is one number nobody reads)
int *__errno(void) {
    static int e;
    return &e;
}
