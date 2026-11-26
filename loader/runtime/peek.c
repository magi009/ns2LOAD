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
#define QUERY_MEMORY "_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE"
#define READ_FILE "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE"

static void *real_open, *real_abort, *real_query, *real_read;
static int dump_next;  // (a HeapDef file was just opened: what is read from it next is written to the log, in hex)

typedef struct {
    u64 available, used, heap, heap_allocated, program, stacks;
    s32 threads;
} memory_info;
static void query_memory(memory_info *info) {  // (what the game is told of memory, as nn::os sums it up)
    void (*f)(memory_info *) = real(QUERY_MEMORY, &real_query);
    if (f) f(info);
    if (info)
        nx2_log("[peek] nn::os::QueryMemoryInfo: available %u MB, used %u MB, heap %u MB (%u MB of it given out), program %u MB, stacks %u KB, %u threads\n",
                info->available >> 20, info->used >> 20, info->heap >> 20, info->heap_allocated >> 20, info->program >> 20, info->stacks >> 10,
                (u64)info->threads);
}

static u32 open_file(void *handle, const char *path, int mode) {
    char p[TEXT];
    copy(p, path);
    u32 (*f)(void *, const char *, int) = real(OPEN_FILE, &real_open);
    const u32 rc = f ? f(handle, path, mode) : 0x202;
    nx2_log("[peek] nn::fs::OpenFile \"%s\" (mode %u): %x\n", p, (u64)mode, (u64)rc);
    for (const char *c = p; *c; c++)
        if (c[0] == 'H' && c[1] == 'e' && c[2] == 'a' && c[3] == 'p' && c[4] == 'D' && c[5] == 'e' && c[6] == 'f') dump_next = 1;
    return rc;
}

// (The file as the game read it, so that it can be decoded off the console: up to 4 KB, 32 bytes a line.)
static u32 read_file(u64 *out, u64 handle, s64 offset, void *buf, u64 size, const void *option) {
    u32 (*f)(u64 *, u64, s64, void *, u64, const void *) = real(READ_FILE, &real_read);
    const u32 rc = f ? f(out, handle, offset, buf, size, option) : 0x202;
    if (dump_next && !rc && buf) {
        dump_next = 0;
        static const char hex[] = "0123456789abcdef";
        const u64 n = size < 4096 ? size : 4096;
        nx2_log("[peek] read %u bytes at %x of it; the file follows, in hex:\n", size, (u64)offset);
        for (u64 at = 0; at < n; at += 32) {
            char line[80];
            u32 len = 0;
            for (u64 k = at; k < n && k < at + 32; k++) line[len++] = hex[((const u8 *)buf)[k] >> 4], line[len++] = hex[((const u8 *)buf)[k] & 15];
            line[len] = 0;
            nx2_log("[hex] %s\n", line);
        }
    }
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
    if (!strcmp(name, QUERY_MEMORY)) return query_memory;
    if (!strcmp(name, READ_FILE)) return read_file;
    return NULL;
}
