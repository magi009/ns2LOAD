// What a game had just opened and compared when it ended itself (with <folder>/debug).
//
// A game that ends with nn::diag's abort at its start says in the log only which calls it made: Super Mario Bros. Wonder opened a
// file of its own, read 527 bytes, compared two strings and ended (2162-0002; a report, 2026-11-26), and nothing said which file or
// which strings. With the long log on, the paths the game opens and the strings it compares are kept, the last few of each, and
// written to the log when it ends itself; every path it opens is logged as it opens it too.
//
// The calls run on the game's threads: no C library there (see log.c). nx2_peek_override runs as the modules are bound.
#include <string.h>
#include <switch.h>

#include "nx2.h"

enum { KEEP = 16, TEXT = 96 };

static struct {
    char a[TEXT], b[TEXT];
    s32 result;
} compared[KEEP];
static u32 compared_n;

static void copy(char *to, const char *from) {
    u32 k = 0;
    if (from)
        for (; k < TEXT - 1 && from[k]; k++) to[k] = (from[k] >= 32 && from[k] < 127) ? from[k] : '?';
    to[k] = 0;
}

static void *real(const char *name, void **cache) {
    if (!*cache) *cache = nx2_symbol(name);
    return *cache;
}

#define OPEN_FILE "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
#define STRCMP "strcmp"
#define ABORT "_ZN2nn4diag6detail9AbortImplEv"

static void *real_open, *real_abort;

static u32 open_file(void *handle, const char *path, int mode) {
    char p[TEXT];
    copy(p, path);
    u32 (*f)(void *, const char *, int) = real(OPEN_FILE, &real_open);
    const u32 rc = f ? f(handle, path, mode) : 0x202;
    nx2_log("[peek] nn::fs::OpenFile \"%s\" (mode %u): %x\n", p, (u64)mode, (u64)rc);
    return rc;
}

static int compare(const char *a, const char *b) {
    int r = 0;
    if (a && b) {  // (as strcmp: no call, so the game's threads go on without the C library)
        const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
        while (*x && *x == *y) x++, y++;
        r = (int)*x - (int)*y;
    }
    const u32 at = __atomic_fetch_add(&compared_n, 1, __ATOMIC_RELAXED) % KEEP;
    copy(compared[at].a, a);
    copy(compared[at].b, b);
    compared[at].result = r;
    return r;
}

static void abort_impl(void) {
    const u32 n = __atomic_load_n(&compared_n, __ATOMIC_RELAXED);
    nx2_log("[peek] the game ends itself; the last strings it compared (of %u), the newest last:\n", (u64)n);
    for (u32 k = n > KEEP ? n - KEEP : 0; k < n; k++)
        nx2_log("[peek]   \"%s\" with \"%s\": %s\n", compared[k % KEEP].a, compared[k % KEEP].b, compared[k % KEEP].result ? "DIFFERENT" : "the same");
    void (*f)(void) = real(ABORT, &real_abort);
    if (f) f();
    nx2_exit();
}

void *nx2_peek_override(const char *name) {
    if (!nx2_debug) return NULL;
    if (!strcmp(name, OPEN_FILE)) return open_file;
    if (!strcmp(name, STRCMP)) return compare;
    if (!strcmp(name, ABORT)) return abort_impl;
    return NULL;
}
