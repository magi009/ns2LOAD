// The SDK's "virtual address memory" (nn::os::AllocateAddressRegion and the like), answered here when the title this was started
// over has no "system resource" (<folder>/vam).
//
// A game made for a system resource reserves address space as it runs and maps memory into it page by page; on its console the
// kernel does that with MapPhysicalMemory, which needs the system resource. A title that has none (0 MB, in the log's line about
// it) has every such mapping refused, and a game that calls nn::os::AllocateAddressRegion there ended with a data abort just
// after the call (2168-0002; a report, 2026-11-26).
//
// Here those calls never reach the SDK. A pool is taken from the kernel's heap before the game starts, between the host's part and
// the game's heap ([host][pool][game's heap]), and an address region is a stretch of that pool: real memory from the start, so
// "mapping pages" into it has nothing to do but say yes. The cost is that a region's whole size is memory in use from the moment
// it is reserved, not only the pages the game maps: <folder>/vam says how many MB the pool has (1024 when the file is empty).
//
// The calls run on the game's threads: no C library there (see log.c). nx2_vam_override runs as the modules are bound, where it may.
#include <switch.h>

#include <string.h>

#include "nx2.h"

// (Regions start on 64 KB, as the console's own do. 2 MB each, at first, made 296 of a game's small regions (64 KB the most of
// them) fill a pool of 1024 MB, and the rest were refused; a report, 2026-11-26.)
enum { VAM_MAX = 8192, VAM_ALIGN = 0x10000 };

int nx2_vam;          // the pool is in use (<folder>/vam)
u64 nx2_sysres_made_for;  // the game's own "system resource" (main.c): what it is told it has while the pool stands in for it
static u64 pool_base, pool_size;
static struct {
    u64 at, size;     // a region: where it is in the pool, how large (rounded to VAM_ALIGN)
} regions[VAM_MAX];
static u32 count;     // regions in use, kept in the order of their addresses
static u64 mapped;    // bytes the game has asked to be mapped, for the usage it may ask for
static int lock_word;

static void lock(void) {
    while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
}
static void unlock(void) { __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE); }

static void zero(u64 at, u64 size) {
    for (u64 *p = (u64 *)at, *end = (u64 *)(at + size); p < end; p++) *p = 0;
}

// The results the game is given: os's own (module 3) "out of memory" and "invalid parameter", the SDK's words for them.
#define VAM_OUT_OF_MEMORY 0x1403u
#define VAM_INVALID 0x0E03u

// (main.c, before the game is started: `base` and `host` as nx2_heap_live was given them. Returns the bytes taken for the pool,
// which are the host's part from then on, as the game's SetHeapSize sees it; 0 when the pool couldn't be had.)
u64 nx2_vam_init(u64 base, u64 host, u64 mb) {
    if (!mb) mb = 1024;
    const u64 want = (mb << 20) & ~(u64)(VAM_ALIGN - 1);
    void *at = NULL;
    const u32 rc = svcSetHeapSize(&at, host + want);
    nx2_log("[vam] a pool of %u MB for the game's address regions, after the host's %u MB of the kernel's heap: %x%s\n", want >> 20, host >> 20, (u64)rc,
            rc ? " (REFUSED: the game's calls go to its SDK, as without <folder>/vam)" : "");
    if (rc) {
        svcSetHeapSize(&at, host);
        return 0;
    }
    pool_base = base + host;
    pool_size = want;
    nx2_vam = 1;
    return want;
}

static u32 allocate_region(u64 *out, u64 size) {
    if (!out || !size) return VAM_INVALID;
    const u64 need = (size + VAM_ALIGN - 1) & ~(u64)(VAM_ALIGN - 1);
    lock();
    u64 at = pool_base;
    u32 k = 0;
    for (; k < count; k++) {  // the first gap that is large enough
        if (regions[k].at - at >= need) break;
        at = regions[k].at + regions[k].size;
    }
    const int fits = count < VAM_MAX && at + need <= pool_base + pool_size;
    if (fits) {
        for (u32 j = count; j > k; j--) regions[j] = regions[j - 1];
        regions[k].at = at, regions[k].size = need;
        count++;
    }
    unlock();
    if (!fits) {
        static u32 told;
        if (told++ < 8)
            nx2_log("[vam] AllocateAddressRegion of %x bytes: the pool of %u MB has no room for it (%u regions); a larger number in <folder>/vam\n", size,
                pool_size >> 20, (u64)count);
        return VAM_OUT_OF_MEMORY;
    }
    *out = at;
    if (nx2_debug || count <= 16) nx2_log("[vam] AllocateAddressRegion of %x bytes: %x (region %u)\n", size, at, (u64)count);
    return 0;
}

static u32 free_region(u64 address) {
    lock();
    u32 k = 0;
    while (k < count && regions[k].at != address) k++;
    if (k == count) {
        unlock();
        nx2_log("[vam] FreeAddressRegion of %x: not a region of the pool\n", address);
        return VAM_INVALID;
    }
    const u64 size = regions[k].size;
    for (; k + 1 < count; k++) regions[k] = regions[k + 1];
    count--;
    unlock();
    zero(address, size);  // (as memory the kernel maps is: the next region here starts clear)
    if (nx2_debug) nx2_log("[vam] FreeAddressRegion of %x (%x bytes)\n", address, size);
    return 0;
}

static int in_pool(u64 address, u64 size) { return address >= pool_base && size <= pool_size && address - pool_base <= pool_size - size; }

static u32 allocate_pages(u64 address, u64 size) {
    if (!in_pool(address, size)) {
        nx2_log("[vam] AllocateMemoryPages of %x bytes at %x: outside the pool\n", size, address);
        return VAM_INVALID;
    }
    __atomic_add_fetch(&mapped, size, __ATOMIC_RELAXED);
    return 0;  // (the pool's memory is there already)
}

static u32 free_pages(u64 address, u64 size) {
    if (!in_pool(address, size)) {
        nx2_log("[vam] FreeMemoryPages of %x bytes at %x: outside the pool\n", size, address);
        return VAM_INVALID;
    }
    zero(address, size);
    __atomic_sub_fetch(&mapped, size, __ATOMIC_RELAXED);
    return 0;
}

static bool enabled(void) { return true; }

typedef struct {
    u64 assigned, used;
} usage_t;
static usage_t usage(void) {
    usage_t u = {pool_size, __atomic_load_n(&mapped, __ATOMIC_RELAXED)};
    return u;
}

void *nx2_vam_override(const char *name) {
    if (!nx2_vam) return NULL;
    static const struct {
        const char *name;
        void *with;
    } calls[] = {
        {"_ZN2nn2os21AllocateAddressRegionEPmm", allocate_region},
        {"_ZN2nn2os17FreeAddressRegionEm", free_region},
        {"_ZN2nn2os19AllocateMemoryPagesEmm", allocate_pages},
        {"_ZN2nn2os15FreeMemoryPagesEmm", free_pages},
        {"_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", enabled},
        {"_ZN2nn2os37GetVirtualAddressMemoryResourceUsageEv", usage},
    };
    for (u32 k = 0; k < sizeof calls / sizeof *calls; k++)
        if (!strcmp(name, calls[k].name)) {
            nx2_log("[vam] %s: answered by the pool\n", name);
            return calls[k].with;
        }
    return NULL;
}

u64 nx2_vam_mapped(void) { return __atomic_load_n(&mapped, __ATOMIC_RELAXED); }
