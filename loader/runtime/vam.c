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

// ---------------------------------------------------------------- MapPhysicalMemory, stood in for
// A later SDK maps memory into its address regions itself, with the kernel's MapPhysicalMemory, once it is told the program
// has a system resource (above, svc.c). Over a title that has none the kernel refuses it (fa01, "out of resource"), and the SDK
// ended the game at its start: Kirby and the Forgotten Land, Switch 2 Edition, over Pokopia; log, 2026-11-27. So where the kernel
// refuses, the memory is put there from the pool another way: as "code memory" of the program's own, mapped at the address asked
// (the kernel keeps that kind in the same region of the address space as what MapPhysicalMemory maps), or else with MapMemory.
// Memory is put there by 2 MB at a time, so that a game's many small mappings cost few handles; it is kept, and only cleared,
// when the game unmaps it.
enum { PM_BLOCK = 0x200000, PM_MAX = 4096 };
static struct {
    u64 at, size;
    u8 how;  // 1: code memory, 2: MapMemory
} pm[PM_MAX];
static u32 pm_count;
static int pm_lock;
static u32 pm_said;

static int pm_covers(u64 a) {
    for (u32 k = 0; k < pm_count; k++)
        if (a - pm[k].at < pm[k].size) return 1;
    return 0;
}
static int pm_touches(u64 a, u64 size) {
    for (u32 k = 0; k < pm_count; k++)
        if (pm[k].at < a + size && a < pm[k].at + pm[k].size) return 1;
    return 0;
}
// one piece, from the pool, put at `at`: how it was done (0: it couldn't be)
static int pm_put(u64 at, u64 size) {
    if (pm_count >= PM_MAX) return 0;
    u64 src = 0;
    if (allocate_region(&src, size)) return 0;
    Handle h = 0;
    u32 rc = svcCreateCodeMemory(&h, (void *)src, size);
    u32 rc2 = rc ? rc : svcControlCodeMemory(h, CodeMapOperation_MapOwner, (void *)at, size, Perm_Rw);
    int how = 0;
    if (!rc && !rc2) how = 1;
    else {
        if (!rc) svcCloseHandle(h);
        if (!svcMapMemory((void *)at, (void *)src, size)) how = 2;
    }
    if (pm_said < 12 || !how) {
        pm_said++;
        nx2_log("[vam] MapPhysicalMemory refused by the kernel: %x bytes at %x put there from the pool %s (code memory: %x, %x)\n", size, at,
                how == 1 ? "as code memory" : how == 2 ? "with MapMemory" : "NOT: neither way was let", (u64)rc, (u64)rc2);
    }
    if (!how) {
        free_region(src);
        return 0;
    }
    pm[pm_count].at = at, pm[pm_count].size = size, pm[pm_count].how = (u8)how;
    pm_count++;
    __atomic_add_fetch(&mapped, size, __ATOMIC_RELAXED);
    return how;
}

// (svc.c, after the kernel refused MapPhysicalMemory(address, size): 0 when it is mapped now, or the kernel's result again)
u32 nx2_vam_map_physical(u64 address, u64 size, u32 refused) {
    if (!nx2_vam || !size || (address | size) & 0xFFF) return refused;
    while (__atomic_exchange_n(&pm_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    u32 rc = 0;
    for (u64 p = address, end = address + size; p < end && !rc;) {
        if (pm_covers(p)) {
            p += 0x1000;
            continue;
        }
        const u64 block = p & ~(u64)(PM_BLOCK - 1);
        if (!pm_touches(block, PM_BLOCK) && pm_put(block, PM_BLOCK)) {
            p = block + PM_BLOCK;
            continue;
        }
        u64 q = p;  // (the block couldn't be had whole: what is asked of it alone)
        while (q < end && q < block + PM_BLOCK && !pm_covers(q)) q += 0x1000;
        if (pm_put(p, q - p)) p = q;
        else rc = refused;
    }
    __atomic_store_n(&pm_lock, 0, __ATOMIC_RELEASE);
    return rc;
}

// (svc.c, before UnmapPhysicalMemory: 1 when the range is all memory put here, now cleared; 0: the kernel's to answer)
int nx2_vam_unmap_physical(u64 address, u64 size) {
    if (!nx2_vam || !pm_count || !size) return 0;
    while (__atomic_exchange_n(&pm_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    int all = 1;
    for (u64 p = address & ~0xFFFull; p < address + size && all; p += 0x1000) all = pm_covers(p);
    __atomic_store_n(&pm_lock, 0, __ATOMIC_RELEASE);
    if (all) zero(address, size);
    return all;
}
