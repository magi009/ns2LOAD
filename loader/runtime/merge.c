// The game loader's way with a game that has more modules than the loader service maps: the ones that needn't be whole are made
// into ONE image, here, on the console, as the program starts.
//
// kaiHEN's loader service maps eight images for a program, this one's own among them (whole.c, ro.c). A module kept whole is an
// image; so is each module a game loads as it runs. A port (tools/relink.py) gets around that on a computer: the modules that
// needn't be whole are made part of the port's own code and data. The loader has no such step, so it does the same work in memory:
//
//   the image's code            each such module's code and read-only data, as they are, one after another, each followed by its
//                               thunks (all of it is mapped as code: read-only data that could be run is no harm to the game)
//   the image's data            each module's data and bss, one after another, a page of zeros after each (the place of a
//                               program's arguments on the console)
//
// What depended on the distance between a module's code and its data is rewritten, as the relinker does it:
//
//   adrp xN, <page in the data>      its distance worked out again (the twelve low bits that follow stay: both parts keep their
//                                    place in a page)
//   adr  xN, <address in the data>   out of adr's reach now: a branch to a thunk that makes the address with adrp + add
//   svc  #n                          a branch to a thunk that asks the runtime first (nx2_svc_entry), as in a module kept whole
//
// Pointers kept in the data are the loader's to set (loader.c), from the modules' own tables, with each module's code and data where
// they are now (nx2_host). To loader.c such a module is one that was relinked: it has no image of its own (nx2_images[k].image is
// null) and its text and data are apart.
//
// Which modules stay whole is the relinker's rule (choose_whole), each kind met on the console: main and sdk (a C++ exception is
// thrown in one and unwound by the other, and the unwinder finds a module by where its code begins in the memory map), and any
// module with thread-local storage of its own. What is new here against a port: a module merged is less surely right than one kept
// whole (a compiler's form the rewriting doesn't know reads the wrong place), so nothing is merged unless it has to be.
#ifdef NX2_LOADER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);
void nx2_log_flush(void);
extern char nx2_svc_entry[];

#define PAGE 0x1000ull
#define MERGE_MAX 16
#define NOT_ASKED 0x36  // (SynchronizePreemptionState: left as the game has it; tools/relink.py says why)

static struct {
    u8 *image;
    u64 size, text_end, data_start;
    unsigned count, of[MERGE_MAX];
    u64 text_at[MERGE_MAX], data_at[MERGE_MAX];
} merged;

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | (u64)rd32(p + 4) << 32; }
static s64 sx(u64 v, int bits) { return (s64)(v << (64 - bits)) >> (64 - bits); }
static u64 up(u64 v) { return (v + PAGE - 1) & ~(PAGE - 1); }

// Whether a module asks for the SDK's function that loads a module as the game runs (nn::ro::LoadModule): a name of its table
// of symbols that it doesn't define itself.
static int loads_modules(unsigned k) {
    const Nx2Module *m = &nx2_modules[k];
    const u8 *img = nx2_images[k].image;
    const u64 size = nx2_images[k].image_size;
    static const char want[] = "_ZN2nn2ro10LoadModule";
    for (u64 s = 0; s < m->nsyms; s++) {
        const u64 at = m->dynsym + s * 24;
        if (at + 24 > size) break;
        const u64 name = m->dynstr + rd32(img + at);
        if ((img[at + 6] | img[at + 7] << 8) == 0 && name + sizeof want <= size && !memcmp(img + name, want, sizeof want - 1)) return 1;
    }
    return 0;
}

// Whether a module has thread-local storage that is found at run time (a relocation of the TLS kinds).
static int has_own_tls(unsigned k) {
    const Nx2Module *m = &nx2_modules[k];
    const u8 *img = nx2_images[k].image;
    const u64 size = nx2_images[k].image_size;
    u64 tag[32] = {0};
    for (u64 a = m->dynamic; a + 16 <= size; a += 16) {
        const u64 t = rd64(img + a);
        if (!t) break;
        if (t < 32) tag[t] = rd64(img + a + 8);
    }
    static const int tables[2][2] = {{7, 8}, {23, 2}};  // (DT_RELA and its size; DT_JMPREL and its)
    for (int t = 0; t < 2; t++) {
        const u64 at = tag[tables[t][0]], bytes = tag[tables[t][1]];
        if (!at || at + bytes > size) continue;
        for (u64 r = 0; r + 24 <= bytes; r += 24) {
            const u32 type = rd32(img + at + r + 8);
            if (type >= 1028 && type <= 1031) return 1;
        }
    }
    return 0;
}

// What a module's code needs rewritten: how many of its adr reach into its data, and how many system calls it has that are asked
// of the runtime (0 and 1 of the answer); how many literal loads reach into the data (2), which nothing is done about.
static void count_sites(unsigned k, u32 *counts) {
    const Nx2Module *m = &nx2_modules[k];
    const Nx2Image *im = &nx2_images[k];
    counts[0] = counts[1] = counts[2] = 0;
    for (u64 a = 0; a + 4 <= im->text_size; a += 4) {
        const u32 w = rd32(im->image + a);
        if ((w & 0x9F000000u) == 0x10000000u) {
            const u64 target = a + sx((u64)(w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3), 21);
            counts[0] += target >= m->data_off && target < m->end;
        } else if ((w & 0xFFE0001Fu) == 0xD4000001u) {
            counts[1] += (w >> 5 & 0xFFFF) != NOT_ASKED;
        } else if ((w & 0x3B000000u) == 0x18000000u) {
            const u64 target = a + (sx(w >> 5 & 0x7FFFF, 19) << 2);
            counts[2] += target >= m->data_off && target < m->end;
        }
    }
}

static u32 adrp_to(u32 reg, u64 from, u64 to) {
    const s64 pages = ((s64)(to & ~(PAGE - 1)) - (s64)(from & ~(PAGE - 1))) >> 12;
    return 0x90000000u | reg | (u32)(pages & 3) << 29 | (u32)(pages >> 2 & 0x7FFFF) << 5;
}
static u32 branch(u64 from, u64 to) { return 0x14000000u | (u32)(((s64)to - (s64)from) / 4 & 0x3FFFFFF); }

// One module's code, already in the image at text_at with its data at data_at, rewritten. thunks: where its thunks go (in the image).
static int rewrite(unsigned k, u64 text_at, u64 data_at, u64 thunks, const u32 *counts) {
    const Nx2Module *m = &nx2_modules[k];
    const u64 text_size = nx2_images[k].text_size, far = 127ull << 20;
    u8 *img = merged.image;
    // (the thunks of the system calls first, and the address they all load right after them: a literal load reaches a megabyte)
    const u64 literal = thunks + counts[1] * 24ull;
    u64 next_svc = thunks, next_adr = literal + 8;
    u32 adrp = 0;
    if (counts[1] * 24ull > (1u << 20) - 64 || next_adr + counts[0] * 12ull - text_at > far) {
        nx2_log("[merge] %s: too large to be merged (%u system calls, %u adr, %u KB of code)\n", m->name, (u64)counts[1], (u64)counts[0], text_size >> 10);
        return -1;
    }
    *(u64 *)(img + literal) = (u64)nx2_svc_entry;
    for (u64 a = 0; a + 4 <= text_size; a += 4) {
        const u64 site = text_at + a;
        u32 *at = (u32 *)(img + site);
        const u32 w = *at;
        if ((w & 0x9F000000u) == 0x90000000u) {  // adrp
            const u64 target = (a & ~(PAGE - 1)) + ((u64)sx((u64)(w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3), 21) << 12);
            if (target >= m->data_off && target < m->end) *at = adrp_to(w & 31, site, data_at + (target - m->data_off)), adrp++;
        } else if ((w & 0x9F000000u) == 0x10000000u) {  // adr
            const u64 target = a + sx((u64)(w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3), 21);
            if (target >= m->data_off && target < m->end) {
                const u64 now = data_at + (target - m->data_off);
                const u32 reg = w & 31;
                u32 *t = (u32 *)(img + next_adr);
                t[0] = adrp_to(reg, next_adr, now);
                t[1] = 0x91000000u | (u32)(now & 0xFFF) << 10 | reg << 5 | reg;  // add xN, xN, #<the low twelve bits>
                t[2] = branch(next_adr + 8, site + 4);
                *at = branch(site, next_adr);
                next_adr += 12;
            }
        } else if ((w & 0xFFE0001Fu) == 0xD4000001u && (w >> 5 & 0xFFFF) != NOT_ASKED) {  // svc
            u32 *t = (u32 *)(img + next_svc);
            t[0] = 0xA9BF7BF0;                                                    // stp x16, x30, [sp, #-16]!
            t[1] = 0x52800010 | (w >> 5 & 0xFFFF) << 5;                           // mov w16, #n
            t[2] = 0x5800001E | (u32)((literal - (next_svc + 8)) / 4) << 5;       // ldr x30, <nx2_svc_entry's address>
            t[3] = 0xD63F03C0;                                                    // blr x30
            t[4] = 0xA8C17BF0;                                                    // ldp x16, x30, [sp], #16
            t[5] = branch(next_svc + 20, site + 4);
            *at = branch(site, next_svc);
            next_svc += 24;
        }
    }
    nx2_log("[merge] %s: code and read-only data %x bytes at %x of the image, data and bss %x at %x; %u adrp and %u adr into the data moved, %u system calls\n",
            m->name, m->data_off, text_at, m->end - m->data_off, data_at, (u64)adrp, (u64)counts[0], (u64)counts[1]);
    if (counts[2]) nx2_log("[merge] %s: WARNING: %u literal loads reach into its data: they are not moved, and will read the wrong place\n", m->name, (u64)counts[2]);
    return 0;
}

// Decides which of the game's modules stay whole and makes one image of the others. wish: game.cfg's "modules" ("whole": none is
// merged; "merge": all that may be are; anything else: only when the game needs the room). 0: done (merged or not); -1: the game
// can't be loaded.
int nx2_merge_plan(const char *wish) {
    const unsigned n = nx2_module_count;
    int loads = 0, must[MERGE_MAX] = {0};
    unsigned kept = 0, free_ones = 0;
    if (n > MERGE_MAX) return -1;
    for (unsigned k = 0; k < n; k++) {
        const char *name = nx2_modules[k].name;
        loads |= loads_modules(k);
        must[k] = !strcmp(name, "main") || !strcmp(name, "sdk") || has_own_tls(k);
        // (a module whose data doesn't begin at a page can't keep the low bits of its addresses: left whole)
        must[k] |= (nx2_modules[k].data_off & (PAGE - 1)) != 0;
        kept += must[k], free_ones += !must[k];
    }
    const int never = !strcmp(wish, "whole"), always = !strcmp(wish, "merge");
    // When the room is needed: the game's modules are more than seven, or it loads modules of its own as it runs (how many at a
    // time can't be known here: as a port is made, as few as can be are kept whole then).
    const int needed = n > 7 || loads;
    if (never || free_ones < 2 || !(needed || always)) {
        if (n > 7) {
            nx2_log("[game] the game has %u modules, and the loader service maps seven for a program besides its own%s: this game needs a port made for it\n",
                    (u64)n, never ? " (game.cfg says to merge none)" : free_ones < 2 ? ", and none of them can be merged with another" : "");
            return -1;
        }
        nx2_log("[game] %u modules, all kept whole%s; %u more can be loaded as the game runs\n", (u64)n, loads ? " (the game loads modules of its own as it runs)" : "",
                (u64)(7 - n));
        return 0;
    }
    if (kept + 1 > 7) {
        nx2_log("[game] %u of the game's %u modules have to be whole, and the loader service maps seven besides the program's own: this game needs a port made for it\n",
                (u64)kept, (u64)n);
        return -1;
    }

    // where each goes in the image: a page for the image's own header, the modules' code, a page of read-only nothing, their data
    u32 counts[MERGE_MAX][3];
    u64 thunks[MERGE_MAX], at = PAGE;
    merged.count = 0;
    for (unsigned k = 0; k < n; k++) {
        if (must[k]) continue;
        const unsigned i = merged.count++;
        merged.of[i] = k;
        count_sites(k, counts[i]);
        merged.text_at[i] = at;
        thunks[i] = at + up(nx2_modules[k].data_off);
        at = thunks[i] + up(counts[i][1] * 24ull + 8 + counts[i][0] * 12ull + 8);
    }
    merged.text_end = at;
    merged.data_start = at += PAGE;
    for (unsigned i = 0; i < merged.count; i++) {
        const Nx2Module *m = &nx2_modules[merged.of[i]];
        merged.data_at[i] = at;
        at += up(m->end - m->data_off) + PAGE;
    }
    merged.size = at;
    merged.image = merged.size < (1ull << 31) ? nx2_big_alloc(merged.size, 1) : NULL;
    if (!merged.image) {
        nx2_log("[merge] no memory for the image of %u modules (%u KB)\n", (u64)merged.count, merged.size >> 10);
        return -1;
    }
    memset(merged.image, 0, merged.size);
    for (unsigned i = 0; i < merged.count; i++) {
        const unsigned k = merged.of[i];
        Nx2Module *m = &nx2_modules[k];
        Nx2Image *im = &nx2_images[k];
        memcpy(merged.image + merged.text_at[i], im->image, m->data_off);
        memcpy(merged.image + merged.data_at[i], im->image + m->data_off, im->image_size - m->data_off);
        if (rewrite(k, merged.text_at[i], merged.data_at[i], thunks[i], counts[i])) return -1;
        // (the module's own memory was taken with a page before it, for an image of its own: game.c's module_load)
        nx2_big_free((u8 *)im->image - PAGE), free((void *)im->svcs);
        im->image = NULL, im->svcs = NULL, im->nsvcs = 0;
    }
    nx2_log("[game] %u modules: %u kept whole, %u made into one image of %u KB (%s); %u more can be loaded as the game runs\n", (u64)n, (u64)kept, (u64)merged.count,
            merged.size >> 10, n > 7 ? "the game has more than the loader service maps" : always ? "game.cfg says so" : "the game loads modules of its own as it runs",
            (u64)(7 - kept - 1));
    return 0;
}

// The image handed to the loader service, after the modules kept whole (whole.c), and each merged module told where it is.
int nx2_merge_map(void *ro_service) {
    Service *ro = ro_service;
    if (!merged.count) return 0;
    u8 *bss = aligned_alloc(PAGE, PAGE);
    if (!bss) return -1;
    memset(bss, 0, PAGE);
    u32 *w = (u32 *)merged.image;
    w[4] = 0x304F524E;  // "NRO0"
    w[6] = (u32)merged.size;
    w[8] = 0, w[9] = (u32)merged.text_end;                                         // the code (and the modules' read-only data)
    w[10] = (u32)merged.text_end, w[11] = (u32)PAGE;                               // read-only data of its own: a page of nothing
    w[12] = (u32)merged.data_start, w[13] = (u32)(merged.size - merged.data_start);  // the data, the modules' bss in it
    w[14] = (u32)PAGE;
    memcpy(merged.image + 0x40, "nx2-recomp:merged", 17);
    nx2_log("[load] %u modules to be mapped as one image by the loader service: %u KB at %p\n", (u64)merged.count, merged.size >> 10, (u64)merged.image);
    nx2_log_flush();
    const struct {
        u64 pid, nro_address, nro_size, bss_address, bss_size;
    } in = {0, (u64)merged.image, merged.size, (u64)bss, PAGE};
    u64 at = 0;
    const Result rc = serviceDispatchInOut(ro, 0, in, at, .in_send_pid = true);
    if (R_FAILED(rc) || !at) {
        nx2_log("[load] the merged image: the loader service refused it (%x)\n", (u64)rc);
        nx2_log_flush();
        return -1;
    }
    for (unsigned i = 0; i < merged.count; i++) {
        Nx2Module *m = &nx2_modules[merged.of[i]];
        m->text = (u8 *)at + merged.text_at[i], m->data = (u8 *)at + merged.data_at[i];
        nx2_log("[load] %s: merged, its code at %p and its data at %p (the image at %p)\n", m->name, (u64)m->text, (u64)m->data, at);
    }
    nx2_log_flush();
    return 0;
}
#endif
