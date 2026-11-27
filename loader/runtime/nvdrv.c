// "nvdrv", stood in for (phase 1): the graphics driver's service of a Switch game, answered here when the title the homebrew menu
// was started over may not open it.
//
// A game of the older kind draws through nvdrv: it opens the driver's devices (/dev/nvmap, /dev/nvhost-as-gpu, ...gpu, -ctrl,
// -ctrl-gpu) and sends them requests ("ioctls"): memory to make and map, a channel to the graphics processor, lists of commands
// for it. Under a title that may not open nvdrv (one made for the Switch 2 alone) the console refuses the service, and the game's
// SDK ended the program at nv::InitializeGraphics (2162-0002; log, 2026-11-26).
//
// Here the service is answered by the port: every device opens, every request is answered "done" with what a Switch's driver
// would have given (memory handles, addresses in the graphics processor's space, the processor's description, fences that are
// already passed), and nothing reaches a graphics processor. Nothing is drawn: the picture stays as it is. What this is for is
// to see how far a game goes past its start with that answered, and what it asks of the graphics processor on the way: every
// device opened and every kind of request is logged (with <folder>/debug, every request, up to a budget).
//
// Only when the console refuses nvdrv to the title started over: under one that has it, the game is given the console's own.
// <folder>/nonvdrv turns it off (the refusal then stands, as before).
//
// The answers run on the game's threads: no C library (see log.c).
#include <switch.h>

#include "nx2.h"

int nx2_nonvdrv;  // (<folder>/nonvdrv: no stand-in for nvdrv)

static void put32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v), put32(p + 4, (u32)(v >> 32)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }
static void copy(u8 *to, const u8 *from, u64 n) {
    for (u64 k = 0; k < n; k++) to[k] = from[k];
}
static void zero(u8 *to, u64 n) {
    for (u64 k = 0; k < n; k++) to[k] = 0;
}
static int same(const char *a, const char *b) {
    for (; *a && *a == *b; a++, b++) {}
    return *a == *b;
}

int nx2_nvdrv_stands(const char *service) { return !nx2_nonvdrv && same(service, "nvdrv"); }

// ---------------------------------------------------------------- the devices, by the numbers the game is given for them
enum { DEV_OTHER, DEV_NVMAP, DEV_CTRL, DEV_CTRL_GPU, DEV_AS_GPU, DEV_GPU };
static const struct {
    const char *path;
    u8 kind;
} devices[] = {
    {"/dev/nvmap", DEV_NVMAP},           {"/dev/nvhost-ctrl", DEV_CTRL}, {"/dev/nvhost-ctrl-gpu", DEV_CTRL_GPU},
    {"/dev/nvhost-as-gpu", DEV_AS_GPU},  {"/dev/nvhost-gpu", DEV_GPU},
};
enum { FDS = 64 };
static u8 fd_kind[FDS];
static u32 fd_next = 1;
static const char *kind_name(u32 kind) {
    static const char *const names[] = {"another device", "nvmap", "nvhost-ctrl", "nvhost-ctrl-gpu", "nvhost-as-gpu", "nvhost-gpu"};
    return kind < sizeof names / sizeof *names ? names[kind] : "?";
}

// memory the game makes (nvmap): its handles and their sizes
enum { MAPS = 8192 };
static u32 map_size[MAPS], map_next = 1;
// the graphics processor's address space, given out from the start of its region of large pages
static u64 va_next = 0x400000000ull;
static u64 va_take(u64 size, u64 align) {
    if (align < 0x20000) align = 0x20000;
    size = (size + align - 1) & ~(align - 1);
    if (!size) size = align;
    for (;;) {
        u64 at = __atomic_load_n(&va_next, __ATOMIC_RELAXED), start = (at + align - 1) & ~(align - 1);
        if (__atomic_compare_exchange_n(&va_next, &at, start + size, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return start;
    }
}
// the one point of the graphics processor's progress (a "syncpoint"): every list of commands moves it on, and it has always
// got there already
enum { SYNCPOINT = 30 };
static u32 syncpoint_value;
static u32 submitted, requests;

// what a Switch's driver says of its graphics processor (a Tegra X1's GM20B), as the processor's description is read
static void characteristics(u8 *c) {
    zero(c, 0xA0);
    put32(c + 0x00, 0x120), put32(c + 0x04, 0xB), put32(c + 0x08, 0xA1), put32(c + 0x0C, 1);  // arch, impl, rev, GPCs
    put64(c + 0x10, 0x40000), put64(c + 0x18, 0);                                             // L2, memory on the board
    put32(c + 0x20, 2), put32(c + 0x24, 0x20), put32(c + 0x28, 0x20000), put32(c + 0x2C, 0x20000);  // TPCs, bus, pages
    put32(c + 0x30, 0x1B), put32(c + 0x34, 0x30000), put32(c + 0x38, 1);                      // PDE bits, big pages, GPC mask
    put32(c + 0x3C, 0x503), put32(c + 0x40, 0x503), put32(c + 0x44, 0x80), put32(c + 0x48, 0x28);  // SM 5.3, warps, VA bits
    put64(c + 0x50, 0x55);                                                                    // flags
    put32(c + 0x58, 0x902D), put32(c + 0x5C, 0xB197), put32(c + 0x60, 0xB1C0), put32(c + 0x64, 0xB06F);  // 2D, 3D, compute, GPFIFO
    put32(c + 0x68, 0xA140), put32(c + 0x6C, 0xB0B5), put32(c + 0x70, 1), put32(c + 0x74, 0);  // inline, copy, FBPs
    put32(c + 0x78, 2), put32(c + 0x7C, 1), put32(c + 0x80, 0), put32(c + 0x84, 1);           // LTCs, LTSs, tex, GPCs
    put32(c + 0x88, 0x21D70), put32(c + 0x8C, 0);
    copy(c + 0x90, (const u8 *)"gm20b\0\0", 8);
}

// One request ("ioctl") to a device. `b` is its bytes, in and out (the request's own size); `extra` the second buffer of the newer
// forms (Ioctl2's second input, Ioctl3's second output), or null. Returns the driver's own result (0: done).
static u32 ioctl(u32 kind, u32 cmd, u8 *b, u32 size, u8 *extra, u32 extra_size) {
    const u32 nr = cmd & 0xFF, group = cmd >> 8 & 0xFF;
    (void)group;
    switch (kind) {
    case DEV_NVMAP:
        switch (nr) {
        case 1: {  // Create {size; handle}
            const u32 h = __atomic_fetch_add(&map_next, 1, __ATOMIC_RELAXED);
            if (h < MAPS) map_size[h] = rd32(b);
            if (size >= 8) put32(b + 4, h);
            return 0;
        }
        case 3:  // FromId {id; handle}
            if (size >= 8) put32(b + 4, rd32(b));
            return 0;
        case 4: return 0;  // Alloc
        case 5: {  // Free {handle; pad; address; size; flags}
            const u32 h = rd32(b);
            if (size >= 0x18) put64(b + 8, 0), put32(b + 0x10, h < MAPS ? map_size[h] : 0), put32(b + 0x14, 0);
            return 0;
        }
        case 9: {  // Param {handle; which; value}
            const u32 h = rd32(b), which = rd32(b + 4);
            u32 v = 0;
            if (which == 1) v = h < MAPS ? map_size[h] : 0;  // size
            else if (which == 2) v = 0x1000;                // alignment
            else if (which == 4) v = 0x40000000;            // heap
            if (size >= 12) put32(b + 8, v);
            return 0;
        }
        case 0xE:  // GetId {id; handle}
            if (size >= 8) put32(b, rd32(b + 4));
            return 0;
        }
        return 0;
    case DEV_AS_GPU:
        switch (nr) {
        case 2: {  // AllocSpace {pages; page size; flags; pad; offset}
            const u64 bytes = (u64)rd32(b) * rd32(b + 4);
            if (!(rd32(b + 8) & 1) && size >= 0x18) put64(b + 0x10, va_take(bytes, rd32(b + 4)));  // (1: the game says where)
            return 0;
        }
        case 6: {  // MapBufferEx {flags; kind; handle; page size; buffer offset; mapping size; offset}
            const u32 h = rd32(b + 8);
            u64 bytes = rd64(b + 0x18);
            if (!bytes && h < MAPS) bytes = map_size[h];
            if (!(rd32(b) & 1) && size >= 0x28) put64(b + 0x20, va_take(bytes, rd32(b + 0xC)));
            return 0;
        }
        case 8:  // GetVARegions {buffer; size; pad; two regions: offset, page size, pad, pages}
            if (size >= 0x40) {
                put64(b + 0x10, 0x04000000), put32(b + 0x18, 0x1000), put64(b + 0x20, 0x3FBFFF);
                put64(b + 0x28, 0x400000000ull), put32(b + 0x30, 0x10000), put64(b + 0x38, 0x1BFFFF);
            }
            return 0;
        }
        return 0;
    case DEV_CTRL_GPU:
        switch (nr) {
        case 1:  // ZCullGetCtxSize
            if (size >= 4) put32(b, 0x20000);
            return 0;
        case 2:  // ZCullGetInfo
            if (size >= 0x28) {
                static const u32 info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
                for (u32 k = 0; k < 10; k++) put32(b + 4 * k, info[k]);
            }
            return 0;
        case 5:  // GetCharacteristics {size; address; the description}
            if (size >= 0xB0) put64(b, 0xA0), characteristics(b + 0x10);
            if (extra && extra_size >= 0xA0) characteristics(extra);
            return 0;
        case 6:  // GetTPCMasks {size; pad[3]; address; mask}
            if (size >= 0x20) put64(b + 0x18, 3);
            if (extra && extra_size >= 4) put32(extra, 3);
            return 0;
        case 0x14:  // GetActiveSlotMask {slot; mask}
            if (size >= 8) put32(b, 7), put32(b + 4, 1);
            return 0;
        case 0x1C:  // GetGpuTime {nanoseconds}
            if (size >= 8) put64(b, armGetSystemTick() * 625 / 12);
            return 0;
        }
        return 0;
    case DEV_GPU:
        switch (nr) {
        case 8:   // SubmitGpfifo {address; entries; flags; fence}
        case 0x1B: {  // KickoffPB, the same
            const u32 v = __atomic_add_fetch(&syncpoint_value, 1, __ATOMIC_RELAXED);
            if (size >= 0x18) put32(b + 0x10, SYNCPOINT), put32(b + 0x14, v);
            const u32 n = __atomic_add_fetch(&submitted, 1, __ATOMIC_RELAXED);
            if (n == 1 || n == 60 || n % 600 == 0)
                nx2_log("[nvdrv] commands for the graphics processor: %u lists so far (this one of %u entries); nothing is drawn\n", (u64)n,
                        (u64)rd32(b + 8));
            return 0;
        }
        case 9:  // AllocObjCtx {class; flags; object}
            if (size >= 0x10) put64(b + 8, rd32(b));
            return 0;
        case 0x1A:  // AllocGPFIFOEx2 {entries; flags; ...; fence}
            if (size >= 0x18) put32(b + 0x10, SYNCPOINT), put32(b + 0x14, __atomic_load_n(&syncpoint_value, __ATOMIC_RELAXED));
            return 0;
        }
        return 0;
    case DEV_CTRL:
        switch (nr) {
        case 0x14:  // SyncptRead {id; value}
            if (size >= 8) put32(b + 4, __atomic_load_n(&syncpoint_value, __ATOMIC_RELAXED));
            return 0;
        case 0x1B:  // GetConfig {domain; parameter; value}: nothing set
            if (size >= 0x182) zero(b + 0x82, 0x100);
            return 0;
        case 0x1D:  // EventWait {fence; timeout; value}
        case 0x1E:  // EventWaitAsync: already got there
            if (size >= 0x10) put32(b + 0xC, rd32(b + 4));
            return 0;
        }
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------- the service's own requests
typedef struct {
    u8 *at;
    u64 size;
} buffer;
// the buffers a request brings: those it sends (A, or X for small ones) and those it wants filled (B, or C), by their order
static u32 buffers(const u8 *m, buffer *in, buffer *out, u32 most) {
    const u32 w0 = rd32(m), w1 = rd32(m + 4);
    const u32 nx = w0 >> 16 & 15, na = w0 >> 20 & 15, nb = w0 >> 24 & 15, nw = w0 >> 28 & 15;
    u32 o = 8;
    if (w1 >> 31) {
        const u32 d = rd32(m + 8);
        o = 12 + ((d & 1) ? 8 : 0) + 4 * ((d >> 1 & 15) + (d >> 5 & 15));
    }
    for (u32 k = 0; k < most; k++) in[k].at = out[k].at = NULL, in[k].size = out[k].size = 0;
    for (u32 k = 0; k < nx; k++, o += 8) {
        const u32 a = rd32(m + o), lo = rd32(m + o + 4);
        const u64 at = lo | (u64)(a >> 12 & 15) << 32 | (u64)(a >> 6 & 7) << 36;
        if (k < most && a >> 16 && !in[k].size) in[k].at = (u8 *)at, in[k].size = a >> 16;
    }
    for (u32 k = 0; k < na + nb + nw; k++, o += 12) {
        const u32 s = rd32(m + o), lo = rd32(m + o + 4), c = rd32(m + o + 8);
        const u64 size = s | (u64)(c >> 24 & 15) << 32, at = lo | (u64)(c >> 28 & 15) << 32 | (u64)(c >> 2 & 7) << 36;
        buffer *to = k < na ? &in[k] : k < na + nb ? &out[k - na] : NULL;
        if (to && k - (k < na ? 0 : na) < most && size) to->at = (u8 *)at, to->size = size;
    }
    const u32 words = w1 & 0x3FF, mode = w1 >> 10 & 15, cs = mode > 2 ? mode - 2 : mode == 2;
    o += words * 4;
    for (u32 k = 0; k < cs && o + 8 <= 0x100; k++, o += 8) {
        const u32 lo = rd32(m + o), hi = rd32(m + o + 4);
        if (k < most && hi >> 16 && !out[k].size) out[k].at = (u8 *)(lo | (u64)(hi & 0xFFFF) << 32), out[k].size = hi >> 16;
    }
    return 1;
}

// the answer: "done", an event if there is one, and the words of the answer (the driver's own result first)
static void answer(u64 *r, u8 *m, int domain, u32 event, const u32 *words, u32 count) {
    for (u32 k = 0; k < 0x100; k++) m[k] = 0;
    u32 o = 8;
    if (event) put32(m + 8, 1u << 1), put32(m + 12, event), o = 16;
    u8 *d = m + ((o + 15) & ~15u);
    if (domain) d += 16;  // (a domain's answer: how many objects it hands back, none)
    put32(d, 0x4F434653);   // "SFCO"
    for (u32 k = 0; k < count; k++) put32(d + 16 + 4 * k, words[k]);
    put32(m + 4, (event ? 0x80000000u : 0) | (0xF0 - o) / 4);
    r[0] = 0;
}

static void request_said(u32 kind, u32 fd, u32 cmd, u32 size) {
    static u8 seen[6][256];
    const u32 n = __atomic_add_fetch(&requests, 1, __ATOMIC_RELAXED);
    u8 *first = &seen[kind < 6 ? kind : 0][cmd & 0xFF];
    if (!*first || (nx2_debug && n <= 2000)) {
        *first = 1;
        nx2_log("[nvdrv] %s (fd %u): request %x (number %x, %u bytes)%s\n", kind_name(kind), (u64)fd, (u64)cmd, (u64)(cmd & 0xFF), (u64)size,
                nx2_debug ? "" : " (the first of its kind; <folder>/debug: all)");
    }
}

int nx2_nvdrv_answer(u64 *r, u8 *m, int domain) {
    const u32 type = rd32(m) & 0xFFFF;
    if (type != 4 && type != 6) return 0;  // (control and close: the stand-in's own)
    const u8 *q = NULL;
    for (u32 o = 8; o < 0xF0 && !q; o += 4)
        if (m[o] == 'S' && m[o + 1] == 'F' && m[o + 2] == 'C' && m[o + 3] == 'I') q = m + o;
    if (!q) return 0;
    const u32 cmd = rd32(q + 8);
    const u8 *args = q + 16;
    buffer in[2], out[2];
    buffers(m, in, out, 2);
    u32 words[4] = {0};
    switch (cmd) {
    case 0: {  // Open(path) -> fd, result
        char path[48] = {0};
        for (u32 k = 0; k < sizeof path - 1 && k < in[0].size && in[0].at; k++) path[k] = (char)in[0].at[k];
        u32 kind = DEV_OTHER;
        for (u32 k = 0; k < sizeof devices / sizeof *devices; k++)
            if (same(path, devices[k].path)) kind = devices[k].kind;
        const u32 fd = __atomic_fetch_add(&fd_next, 1, __ATOMIC_RELAXED);
        if (fd < FDS) fd_kind[fd] = (u8)kind;
        nx2_log("[nvdrv] the game opens \"%s\": fd %u (%s)\n", path, (u64)fd, kind == DEV_OTHER ? "a device answered with nothing" : "stood in for");
        words[0] = fd, words[1] = 0;
        answer(r, m, domain, 0, words, 2);
        return 1;
    }
    case 1:     // Ioctl(fd, cmd; in; out) -> result
    case 11:    // Ioctl2(fd, cmd; in, in 2; out)
    case 12: {  // Ioctl3(fd, cmd; in; out, out 2)
        const u32 fd = rd32(args), request = rd32(args + 4), kind = fd < FDS ? fd_kind[fd] : DEV_OTHER;
        static u8 b[0x400];  // (the requests are made one at a time by a game's driver: its own lock)
        static int busy;
        while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
        const u32 size = (u32)(in[0].size > out[0].size ? in[0].size : out[0].size) & 0x3FF;
        zero(b, sizeof b);
        if (in[0].at) copy(b, in[0].at, in[0].size < sizeof b ? in[0].size : sizeof b);
        u8 *extra = cmd == 12 ? out[1].at : cmd == 11 ? in[1].at : NULL;
        const u64 extra_size = cmd == 12 ? out[1].size : cmd == 11 ? in[1].size : 0;
        request_said(kind, fd, request, size);
        words[0] = ioctl(kind, request, b, size ? size : (u32)(request >> 16 & 0x3FFF), cmd == 12 ? extra : NULL, (u32)extra_size);
        if (out[0].at) copy(out[0].at, b, out[0].size < sizeof b ? out[0].size : sizeof b);
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        answer(r, m, domain, 0, words, 1);
        return 1;
    }
    case 4: {  // QueryEvent(fd, which) -> result, an event (one that never comes: what is waited for has always got there)
        nx2_log("[nvdrv] an event of fd %u (number %u) is asked for: one that never comes is given\n", (u64)rd32(args), (u64)rd32(args + 4));
        answer(r, m, domain, nx2_stood_handle(), words, 1);
        return 1;
    }
    case 3:  // Initialize(transfer memory's size; the process, the transfer memory)
        nx2_log("[nvdrv] THE PORT STANDS IN FOR \"nvdrv\" (phase 1): the game's graphics driver is answered here and nothing is drawn. "
                "%u KB of transfer memory offered (<folder>/nonvdrv: not)\n", (u64)(rd32(args) >> 10));
        answer(r, m, domain, 0, words, 1);
        return 1;
    default:  // Close, MapSharedMemory, GetStatus, SetAruid, DumpGraphicsMemoryInfo, InitializeDevtools, ...: done
        if (cmd != 2) nx2_log("[nvdrv] the service's command %u: done\n", (u64)cmd);
        answer(r, m, domain, 0, words, cmd == 6 ? 4 : 1);
        return 1;
    }
}
