// The modules kept whole (tools/relink.py --whole), put in memory as the console's loader puts them: each one's code, read-only
// data, data and bss in one stretch, at the distances they were built with, and each part with the access it has on the console
// (code that runs, data that is read, data that is written).
//
// Why: with a module's data kept apart from its code (the other way, relink.py's first), every place in the code that reaches the
// data has to be found and rewritten at build time, and one the relinker doesn't know (a compiler's newer forms: tables of
// distances to data) reads or writes somewhere else, now and then. A Switch 2 game's SDK also looks for its own modules in the
// memory map, by where a stretch of code begins. Whole, a module is as it was built, and is found.
//
// How, on a Switch 2 (kaiHEN): a homebrew program may not map code itself (the kernel's calls for it are not this process's to
// make: tried, and the program was ended at once, 2026-11-24). kaiHEN's own loader service does it for the programs it starts, and
// hands each one its session (the service "ldr:ro", as an override): the same request that loaded this program loads an image of
// the game's module. The image is an NRO: a page of this port's first (an NRO's header is at bytes 0x10 to 0x80 of its code, and a
// game's module has code of its own there), then the module. So a module's first byte is one page after where the image is mapped.
// That first page has, besides the header:
//   - at its start, what the SDK reads at the start of a stretch of code to find the module's own header (three words: the module's
//     first, and where its two headers are, from here);
//   - the thunks: a system call in the code becomes a branch to one, which calls nx2_svc_entry (svc_entry.s) with the call's number
//     in w16, as the relinker's thunks do, and goes back. A branch reaches 128 MB either way: a module with more code than that
//     has the thunks of its far system calls after its code, in what is left of the code's last page.
#ifdef __SWITCH2__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);
void nx2_log_flush(void);
extern char nx2_svc_entry[];

int nx2_map_modules(void) {
    const u64 page = 0x1000, shift = page;
    const Handle session = smGetServiceOverride(smEncodeName("ldr:ro"));
    if (session == INVALID_HANDLE) {
        nx2_log("[load] the loader service's session (ldr:ro) wasn't handed to this program: the modules can't be mapped whole\n");
        return -1;
    }
    Service ro;
    serviceCreate(&ro, session);
    ro.own_handle = 0;
    for (unsigned k = 0; k < nx2_module_count; k++) {
        Nx2Module *m = &nx2_modules[k];
        const Nx2Image *im = &nx2_images[k];
        if (!im->image) continue;  // (a module that was relinked: it is in the program's own code and data)
        const u64 rw_end = (im->image_size + page - 1) & ~(page - 1);  // (where the data ends and the bss begins, in the module)
        const u64 nro_size = shift + rw_end;
        const u64 bss_size = (m->end > rw_end ? m->end - rw_end : 0) + page;  // (and a page of zeros: the program's arguments' place)
        const u64 thunks_end = 0x80 + im->nsvcs * 24ull + 8;
        if (thunks_end > shift || (im->ro_off & (page - 1)) || (m->data_off & (page - 1))) {
            nx2_log("[load] %s: not of a shape this maps (%u system calls, read-only data at %x, data at %x)\n", m->name, (u64)im->nsvcs, im->ro_off, m->data_off);
            return -1;
        }
#ifdef NX2_LOADER
        // (the game loader unpacked the module where it is to be mapped from, a page after the start of memory of its own, so that
        // it is never in memory twice: game.c's module_load)
        u8 *nro = (u8 *)im->image - shift, *bss = aligned_alloc(page, bss_size);
#else
        u8 *nro = aligned_alloc(page, nro_size), *bss = aligned_alloc(page, bss_size);
#endif
        if (!nro || !bss) {
            nx2_log("[load] %s: no memory for its %u KB\n", m->name, (nro_size + bss_size) >> 10);
            return -1;
        }
        memset(nro, 0, shift);
#ifndef NX2_LOADER
        memcpy(nro + shift, im->image, im->image_size);
#endif
        memset(nro + shift + im->image_size, 0, rw_end - im->image_size);
        memset(bss, 0, bss_size);
        // the first page: what the SDK reads to find the module's headers, the NRO's header, the thunks
        u32 *w = (u32 *)nro;
        const u32 *first = (const u32 *)im->image;
        w[0] = first[0];
        w[1] = (u32)(shift + first[1]);  // (the module's header, MOD0)
        w[2] = first[2] ? (u32)(shift + first[2]) : 0;  // (a later SDK's second header)
        w[4] = 0x304F524E;  // "NRO0"
        w[6] = (u32)nro_size;
        w[8] = 0, w[9] = (u32)(shift + im->ro_off);                          // the code
        w[10] = (u32)(shift + im->ro_off), w[11] = (u32)(m->data_off - im->ro_off);  // the read-only data
        w[12] = (u32)(shift + m->data_off), w[13] = (u32)(rw_end - m->data_off);     // the data
        w[14] = (u32)bss_size;
        memcpy(nro + 0x40, "nx2-recomp:", 11), strncpy((char *)nro + 0x40 + 11, m->name, 20);  // (a name of its own where an NRO has its number)
        // A branch reaches 128 MB either way, and the thunks are in the image's first page: a system call further into a module's
        // code than that had its branch wrap, and the game jumped to the wrong place (a main of 192 MB of code: an instruction abort
        // at its start, with nothing in the log; nx2-relink's pull request 24, whose change this is). Such a call's thunk is put
        // after the code instead, in what is left of the code's last page before the read-only data, which is mapped with the code.
        // A module with less code than the reach has every thunk where it was.
        // (<folder>/farthunks: a try, the reach taken as 1 MB, so that a module of the usual size has thunks of the far kind.)
        s64 reach = 0x8000000;
        {
            FILE *t = fopen(NX2_F("farthunks"), "rb");
            if (t) fclose(t), reach = 0x100000;
        }
        u32 far_count = 0;
        for (u32 i = 0; i < im->nsvcs; i++)
            if ((s64)(shift + im->svcs[2 * i]) - 0x80 >= reach) far_count++;
        const u64 far_base = (shift + im->text_size + 7) & ~7ull, far_literal = far_base + far_count * 24ull;
        if (far_count && far_literal + 8 > shift + im->ro_off) {
            nx2_log("[load] %s: %u system calls are further into its code than a branch reaches, and the code's last page has no room for "
                    "their thunks (%x bytes left)\n", m->name, (u64)far_count, shift + im->ro_off - far_base);
            return -1;
        }
        const u32 near_count = im->nsvcs - far_count;
        const u64 near_literal = 0x80 + near_count * 24ull;
        u32 near_at = 0, far_at = 0;
        for (u32 i = 0; i < im->nsvcs; i++) {
            const u64 site = shift + im->svcs[2 * i], n = im->svcs[2 * i + 1];
            const int far = (s64)site - 0x80 >= reach;
            const u64 here = far ? far_base + far_at++ * 24ull : 0x80 + near_at++ * 24ull, literal = far ? far_literal : near_literal;
            if (far && (s64)(here - site) >= 0x8000000) {  // (more than 256 MB of code: neither place is within a branch of it)
                nx2_log("[load] %s: a system call at %x is out of a branch's reach from both ends of its code\n", m->name, site - shift);
                return -1;
            }
            u32 *t = (u32 *)(nro + here);
            t[0] = 0xA9BF7BF0;                                             // stp x16, x30, [sp, #-16]!
            t[1] = 0x52800010 | (u32)n << 5;                               // mov w16, #n
            t[2] = 0x5800001E | (u32)((literal - (here + 8)) / 4) << 5;    // ldr x30, <nx2_svc_entry's address, after the thunks>
            t[3] = 0xD63F03C0;                                             // blr x30
            t[4] = 0xA8C17BF0;                                             // ldp x16, x30, [sp], #16
            t[5] = 0x14000000 | (u32)(((s64)(site + 4) - (s64)(here + 20)) / 4 & 0x3FFFFFF);  // b <after the call>
            // (SynchronizePreemptionState is left as it is, see tools/relink.py: it is the one call the kernel lets a thread make
            // in the state it is made in, and anything the runtime did around it, a line in the log, ended the program)
            if (n != 0x36) *(u32 *)(nro + site) = 0x14000000 | (u32)(((s64)here - (s64)site) / 4 & 0x3FFFFFF);
        }
        *(u64 *)(nro + near_literal) = (u64)nx2_svc_entry;
        if (far_count) {
            *(u64 *)(nro + far_literal) = (u64)nx2_svc_entry;
            nx2_log("[load] %s: %u of its %u system calls are further into its code than a branch reaches: their thunks are after the code\n", m->name,
                    (u64)far_count, (u64)im->nsvcs);
        }
        nx2_log("[load] %s: to be mapped whole by the loader service: an image of %u KB at %p, %u KB of bss at %p\n", m->name, nro_size >> 10, (u64)nro, bss_size >> 10,
                (u64)bss);
        nx2_log_flush();
        const struct {
            u64 pid, nro_address, nro_size, bss_address, bss_size;
        } in = {0, (u64)nro, nro_size, (u64)bss, bss_size};
        u64 at = 0;
        const Result rc = serviceDispatchInOut(&ro, 0, in, at, .in_send_pid = true);
        if (R_FAILED(rc) || !at) {
            nx2_log("[load] %s: the loader service refused it (%x)\n", m->name, (u64)rc);
            nx2_log_flush();
            return -1;
        }
        m->text = (u8 *)at + shift, m->data = m->text + m->data_off;
        nx2_log("[load] %s: whole, at %p (its image at %p): code %x bytes, read-only data from %x, data from %x, bss from %x, ends at %x; %u system calls\n", m->name,
                (u64)m->text, at, im->ro_off, im->ro_off, m->data_off, rw_end, m->end, (u64)im->nsvcs);
        nx2_log_flush();
    }
#ifdef NX2_LOADER
    if (nx2_merge_map(&ro)) return -1;
#endif
    {
        // <folder>/rocount: how many images the loader service takes for one program, measured: three-page images of nothing are
        // asked for until one is refused, and all of them are taken away again.
        FILE *probe = fopen(NX2_F("rocount"), "rb");
        if (probe) {
            fclose(probe);
            static u64 put_at[64];
            unsigned taken = 0;
            Result last = 0;
            for (; taken < 64; taken++) {
                u8 *nro = aligned_alloc(page, 3 * page);
                if (!nro) break;
                memset(nro, 0, 3 * page);
                u32 *w = (u32 *)nro;
                w[4] = 0x304F524E, w[6] = (u32)(3 * page);
                w[8] = 0, w[9] = (u32)page, w[10] = (u32)page, w[11] = (u32)page, w[12] = (u32)(2 * page), w[13] = (u32)page;
                memcpy(nro + 0x40, &taken, sizeof taken);  // (a number of its own each: a module is known by this)
                const struct {
                    u64 pid, nro_address, nro_size, bss_address, bss_size;
                } in = {0, (u64)nro, 3 * page, 0, 0};
                u64 at = 0;
                last = serviceDispatchInOut(&ro, 0, in, at, .in_send_pid = true);
                if (R_FAILED(last)) break;
                put_at[taken] = at;
            }
            nx2_log("[load] the loader service took %u more images after the game's %u, and refused the next with %x\n", (u64)taken, (u64)nx2_module_count, (u64)last);
            for (unsigned k = 0; k < taken; k++) {
                const struct {
                    u64 pid, address;
                } in = {0, put_at[k]};
                const Result rc = serviceDispatchIn(&ro, 1, in, .in_send_pid = true);
                if (R_FAILED(rc)) nx2_log("[load]   image %u not taken away again: %x\n", (u64)k, (u64)rc);
            }
            nx2_log_flush();
        }
    }
    return 0;
}
#else
int nx2_map_modules(void) { return -1; }
#endif
