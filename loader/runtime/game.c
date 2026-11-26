// The game loader's own part: a game read from its NUP file as the program starts, where a port has the game built into it.
//
// A port (tools/build.py) is one game: its modules are in the program, its files are an image on the card that a computer made,
// and what it is told of memory was worked out as it was built. The loader is one program for any game. Given a NUP file (the
// container tools/nup.py reads; the layout is said there), it finds here, each time:
//   - which packages make the game: the game's own and its updates, the later over the earlier (added content isn't taken yet);
//   - the program: the modules of the newest package that has one, unpacked into memory for the loader service to map (whole.c);
//   - the files: every package's, laid over one another, and then a folder of the user's own (mods). No image of them is written
//     anywhere: its header and its tables are made in memory, and a read of the image is answered from the NUP file's own bytes;
//   - the game's folder on the card, named by its title id, where its saved data, its logs and its settings are.
// The loader service maps seven images for a program besides its own. A game with more modules than that, or one that loads
// modules of its own as it runs, has the ones that needn't be whole made into one image here (merge.c), as a port has them made
// part of its own code on a computer; a game with more than seven that have to be whole still needs a port.
#ifdef NX2_LOADER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#define ZSTD_STATIC_LINKING_ONLY  // (for the decoder to write straight into the file's own memory: unpack_one)
#include <zstd.h>  // (third_party/zstd: Zstandard's own decoder)

#include "nx2.h"
void nx2_log(const char *fmt, ...);
void nx2_log_flush(void);
s64 nx2_zbic_to_zstd(const u8 *data, u64 size, u8 *out, u64 room);  // zbic.c

// ---------------------------------------------------------------- what a port is built with
#define MODULES 12
char nx2_game_id[17] = "unknown";
// (What the game is told of memory, as a port is built: with no figure given and nothing kept apart, what there really is less
// 400 MB, 8192 at most; the console's 8992 for a game whose table of heaps keeps a stretch apart; game.cfg's figure otherwise.
// tools/build.py and main.c have the story.)
u32 nx2_game_mb = 8192, nx2_game_room_mb = 400, nx2_game_apart_mb, nx2_game_remote_mb, nx2_game_sysres_kb;
Nx2Module nx2_modules[MODULES];
Nx2Image nx2_images[MODULES];
unsigned nx2_module_count;
int nx2_whole;
static char home[40] = "/nx2-flow/unknown";
static char module_names[MODULES][12];
static char modset[64];  // (game.cfg: the folder under modsets/ to lay over the game; empty: mods/)
static char save[80] = "save";  // (game.cfg: the slot of saved data in use: "save", or "saves/<slot>")
const char *nx2_game_save(void) { return save; }

const char *nx2_home(void) { return home; }
const char *nx2_file(const char *name) {
    static char ring[8][192];
    static u32 next;
    char *to = ring[__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) & 7];
    u32 n = 0;
    for (const char *c = "sdmc:"; *c; c++) to[n++] = *c;
    for (const char *c = home; *c; c++) to[n++] = *c;
    to[n++] = '/';
    for (const char *c = name; *c && n < 190; c++) to[n++] = *c;
    to[n] = 0;
    return to;
}

static u32 r32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 r64(const u8 *p) { return r32(p) | (u64)r32(p + 4) << 32; }
static void w32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static void w64(u8 *p, u64 v) { w32(p, (u32)v), w32(p + 4, (u32)(v >> 32)); }

// ---------------------------------------------------------------- the NUP file
static struct {
    FsFile file;
    u8 *toc;
    u64 toc_size, ndirs, nfiles, children_at, pool_size;
    u32 npackages;
    const u8 *packages, *dirs, *files, *pool;
} nup;

// so many of the file's bytes, from a place in it: 0, or the file system's word for what went wrong
static u32 nup_read(u64 at, void *to, u64 size) {
    u8 *out = to;
    while (size) {
        const u64 take = size < (4u << 20) ? size : 4u << 20;
        u64 got = 0;
        const u32 rc = fsFileRead(&nup.file, (s64)at, out, take, FsReadOption_None, &got);
        if (rc) return rc;
        if (got != take) return 0x2EE202;
        at += take, out += take, size -= take;
    }
    return 0;
}

typedef struct {
    const u8 *name;
    u32 name_len;
    u64 size, at, kept;
    u8 codec;  // 0: its bytes as they are; 1: one zstd frame
} NupFile;
static NupFile nup_file(u64 k) {
    const u8 *f = nup.files + 0x50 * k;
    return (NupFile){nup.pool + r32(f), r32(f + 4), r64(f + 8), r64(f + 16), r64(f + 24), f[0x40]};
}
static u32 nup_child(u32 first, u32 i) { return r32(nup.toc + nup.children_at + 4ull * (first + i)); }

typedef struct {
    u64 title, meta_at;
    u32 root, exefs, romfs, version, meta_size;
    u8 kind;
} Package;
static Package nup_package(u32 k) {
    const u8 *p = nup.packages + 0x40 * k;
    return (Package){r64(p), r64(p + 0x20), r32(p + 8), r32(p + 12), r32(p + 16), r32(p + 20), r32(p + 0x28), p[24]};
}
// a file of a folder, by name: its number, or -1
static s64 nup_find(u32 dir, const char *name) {
    if (dir >= nup.ndirs) return -1;
    const u8 *d = nup.dirs + 0x18 * dir;
    const u32 first = r32(d + 8), count = r32(d + 12), len = (u32)strlen(name);
    for (u32 i = 0; i < count; i++) {
        const u32 node = nup_child(first, i);
        if (node < nup.ndirs) continue;
        const NupFile f = nup_file(node - nup.ndirs);
        if (f.name_len == len && !memcmp(f.name, name, len)) return (s64)(node - nup.ndirs);
    }
    return -1;
}

static int nup_open(const char *path) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    static char inner[FS_MAX_PATH];
    const char *colon = strchr(path, ':');
    strncpy(inner, colon ? colon + 1 : path, sizeof inner - 1);
    if (!sd || fsFsOpenFile(sd, inner, FsOpenMode_Read, &nup.file)) {
        nx2_log("[game] %s can't be opened\n", path);
        return -1;
    }
    u8 head[0x80];
    if (nup_read(0, head, sizeof head) || memcmp(head, "NUP\x01", 4)) {
        nx2_log("[game] %s isn't a NUP container\n", path);
        return -1;
    }
    const u64 toc_at = r64(head + 8);
    nup.toc_size = r64(head + 16), nup.npackages = r32(head + 0x48);
    if (nup.toc_size < 0x30 || nup.toc_size > (512u << 20) || !(nup.toc = nx2_big_alloc(nup.toc_size, 1)) || nup_read(toc_at, nup.toc, nup.toc_size) ||
        memcmp(nup.toc, "TOC\x01", 4)) {
        nx2_log("[game] its table of contents (%u KB) can't be read\n", nup.toc_size >> 10);
        return -1;
    }
    const u32 n = r32(nup.toc + 4);
    nup.ndirs = r64(nup.toc + 8), nup.nfiles = r64(nup.toc + 16);
    const u64 pool_at = r64(nup.toc + 24);
    nup.pool_size = r64(nup.toc + 32);
    nup.packages = nup.toc + 0x30;
    nup.dirs = nup.packages + 0x40ull * n;
    nup.files = nup.dirs + 0x18 * nup.ndirs;
    nup.children_at = (u64)(nup.files - nup.toc) + 0x50 * nup.nfiles;
    nup.pool = nup.toc + pool_at;
    if (n != nup.npackages || nup.children_at > nup.toc_size || pool_at + nup.pool_size > nup.toc_size) {
        nx2_log("[game] its table of contents doesn't hold together (%u packages, %u folders, %u files)\n", (u64)n, nup.ndirs, nup.nfiles);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------- the program: an NSO's three parts, unpacked
// LZ4, one block (an NSO's parts are single blocks): 0 if what came out is as long as it was said to be
static int lz4_block(const u8 *src, u64 n, u8 *out, u64 out_size) {
    u64 i = 0, o = 0;
    while (i < n) {
        const u8 token = src[i++];
        u64 lit = token >> 4;
        if (lit == 15)
            for (u8 b = 255; b == 255 && i < n;) b = src[i++], lit += b;
        if (i + lit > n || o + lit > out_size) return -1;
        memcpy(out + o, src + i, lit);
        i += lit, o += lit;
        if (i >= n) break;
        if (i + 2 > n) return -1;
        const u64 off = src[i] | (u64)src[i + 1] << 8;
        i += 2;
        u64 len = token & 15;
        if (len == 15)
            for (u8 b = 255; b == 255 && i < n;) b = src[i++], len += b;
        len += 4;
        if (!off || off > o || o + len > out_size) return -1;
        for (u64 k = 0; k < len; k++) out[o + k] = out[o - off + k];  // (it may overlap: the pattern repeats)
        o += len;
    }
    return o == out_size ? 0 : -1;
}

// ---------------------------------------------------------------- the loader's own large pieces of memory
// A game's modules and its files unpacked as they are read are tens of MB each, and the allocator of the library this is built
// with (mimalloc; libounce's heap.h) serves pieces of that size badly: of a heap of 512 MB with 481 free, 360 MB could be had in
// pieces of 8 MB, and with a game's 174 MB of modules in it a file of 65 MB had no place though 279 MB were free; the game
// ended there (measured on the console, 2026-11-25). So the top of the program's own memory, as much as the "room" is, is kept
// from that allocator (main.c) and given out here: whole pages, the pieces that stay (modules) from the bottom, the ones that
// come and go from the top, and neighbours joined again when they are given back.
u8 *nx2_big_at;
u64 nx2_big_size;
#define PIECES 512
static struct {
    u64 at, size;
    u32 used;
} pieces[PIECES];
static u32 npieces, pieces_lock;
void *nx2_big_alloc(u64 size, int low) {
    size = (size + 0xFFF) & ~0xFFFull;
    if (!size) size = 0x1000;
    if (!nx2_big_size) return aligned_alloc(0x1000, size);  // (nothing was kept for it: the allocator's own, as before)
    while (__atomic_exchange_n(&pieces_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    if (!npieces) pieces[0].at = (u64)nx2_big_at, pieces[0].size = nx2_big_size, pieces[0].used = 0, npieces = 1;
    int found = -1;
    for (u32 k = 0; k < npieces; k++)
        if (!pieces[k].used && pieces[k].size >= size && (found < 0 || !low)) found = (int)k;  // (kept in order: the first, or the last)
    u64 at = 0;
    if (found >= 0) {
        const u64 left = pieces[found].size - size;
        if (left && npieces < PIECES) {
            // the piece split in two: what is asked for at its bottom (low) or at its top
            for (u32 k = npieces; k > (u32)found + 1; k--) pieces[k] = pieces[k - 1];
            npieces++;
            const u64 start = pieces[found].at;
            const int taken = low ? found : found + 1, rest = low ? found + 1 : found;
            pieces[taken].at = low ? start : start + left, pieces[taken].size = size, pieces[taken].used = 1;
            pieces[rest].at = low ? start + size : start, pieces[rest].size = left, pieces[rest].used = 0;
            at = pieces[taken].at;
        } else {
            pieces[found].used = 1, at = pieces[found].at;
        }
    }
    __atomic_store_n(&pieces_lock, 0, __ATOMIC_RELEASE);
    return (void *)at;
}
void nx2_big_free(void *p) {
    if (!p) return;
    if (!nx2_big_size || (u64)p < (u64)nx2_big_at || (u64)p >= (u64)nx2_big_at + nx2_big_size) {
        free(p);
        return;
    }
    while (__atomic_exchange_n(&pieces_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    for (u32 k = 0; k < npieces; k++) {
        if (pieces[k].at != (u64)p || !pieces[k].used) continue;
        pieces[k].used = 0;
        if (k + 1 < npieces && !pieces[k + 1].used) {
            pieces[k].size += pieces[k + 1].size;
            for (u32 j = k + 1; j + 1 < npieces; j++) pieces[j] = pieces[j + 1];
            npieces--;
        }
        if (k && !pieces[k - 1].used) {
            pieces[k - 1].size += pieces[k].size;
            for (u32 j = k; j + 1 < npieces; j++) pieces[j] = pieces[j + 1];
            npieces--;
        }
        break;
    }
    __atomic_store_n(&pieces_lock, 0, __ATOMIC_RELEASE);
}
// a table that grows: a larger piece, with what it had copied over (null: none to be had, and the old one is as it was)
static void *big_grown(void *old, u64 had, u64 want) {
    void *p = nx2_big_alloc(want, 1);
    if (!p) return NULL;
    if (old) memcpy(p, old, had < want ? had : want), nx2_big_free(old);
    return p;
}
// Where the stretch may end, once the game's modules and tables are in it: after the last piece in use, and after that the room
// the files unpacked as they are read need (cache: none for a game whose files are all kept as they are). 0: no less than now.
// What is past that is the game's to have: main.c makes the program's heap that much smaller, and says so here (trimmed).
// (A game that maps memory as it runs and uses all there is had 190 MB less from the loader than from its port, of which the
// loader used none: it stopped for a moment every twenty or thirty seconds, which its port did not; 2026-11-26.)
static u64 big_end_wanted(u64 cache) {
    u64 end = 0;
    while (__atomic_exchange_n(&pieces_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    if (npieces && nx2_big_size && !pieces[npieces - 1].used) {
        const u64 want = pieces[npieces - 1].at + ((cache + 0xFFF) & ~0xFFFull);
        if (want < pieces[npieces - 1].at + pieces[npieces - 1].size) end = want;
    }
    __atomic_store_n(&pieces_lock, 0, __ATOMIC_RELEASE);
    return end;
}
void nx2_game_room_trimmed(u64 end) {
    while (__atomic_exchange_n(&pieces_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    if (npieces && !pieces[npieces - 1].used && end > pieces[npieces - 1].at && end < pieces[npieces - 1].at + pieces[npieces - 1].size) {
        pieces[npieces - 1].size = end - pieces[npieces - 1].at;
        nx2_big_size = end - (u64)nx2_big_at;
    }
    __atomic_store_n(&pieces_lock, 0, __ATOMIC_RELEASE);
}
// what can still be had of it: all that is free, and the largest piece
static void big_left(u64 *all, u64 *largest) {
    *all = *largest = 0;
    while (__atomic_exchange_n(&pieces_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    if (!npieces && nx2_big_size) *all = *largest = nx2_big_size;
    for (u32 k = 0; k < npieces; k++)
        if (!pieces[k].used) {
            *all += pieces[k].size;
            if (pieces[k].size > *largest) *largest = pieces[k].size;
        }
    __atomic_store_n(&pieces_lock, 0, __ATOMIC_RELEASE);
}

// One module, from its file in the NUP: its image in memory (the bss not there), and the row of each table a port has for it.
static int module_load(unsigned k, const char *name, u64 file) {
    const NupFile f = nup_file(file);
    u8 *nso = nx2_big_alloc(f.size, 0);
    int read = -1;
    if (nso && f.codec == 1) {  // (kept in the NUP file as one Zstandard frame)
        u8 *packed = nx2_big_alloc(f.kept, 0);
        if (packed && !nup_read(f.at, packed, f.kept)) read = ZSTD_decompress(nso, f.size, packed, f.kept) == f.size ? 0 : -1;
        nx2_big_free(packed);
    } else if (nso && !f.codec) {
        read = nup_read(f.at, nso, f.size) ? -1 : 0;
    }
    if (read || f.size < 0x100 || memcmp(nso, "NSO0", 4)) {
        nx2_log("[game] %s: not a module's file (%u KB)\n", name, f.size >> 10);
        nx2_big_free(nso);
        return -1;
    }
    const u32 flags = r32(nso + 0x0C);
    u64 mem[3], size[3];
    for (int s = 0; s < 3; s++) mem[s] = r32(nso + 0x10 + s * 0x10 + 4), size[s] = r32(nso + 0x10 + s * 0x10 + 8);
    const u64 image_size = mem[2] + size[2];
    // (a page before it: the loader service is handed the module from there, with a header of the port's in that page; whole.c)
    u8 *block = nx2_big_alloc(0x1000 + ((image_size + 0xFFF) & ~0xFFFull), 1), *img = block ? block + 0x1000 : NULL;
    if (!img || mem[0] || mem[1] < size[0] || mem[2] < mem[1] + size[1]) {
        nx2_log("[game] %s: its parts aren't where a module's are, or there is no memory for its %u KB\n", name, image_size >> 10);
        nx2_big_free(nso);
        return -1;
    }
    memset(img, 0, image_size);
    for (int s = 0; s < 3; s++) {
        const u64 file_off = r32(nso + 0x10 + s * 0x10), packed = r32(nso + 0x60 + s * 4);
        const u8 *raw = nso + file_off;
        int bad = file_off > f.size;
        if (bad) {
        } else if (!(flags >> s & 1)) {
            bad = file_off + size[s] > f.size;
            if (!bad) memcpy(img + mem[s], raw, size[s]);
        } else if (file_off + packed > f.size) {
            bad = 1;
        } else if ((flags & 0x80) && packed >= 4 && !memcmp(raw, "ZBIC", 4)) {
            // (a later system's way: the frame written again as Zstandard's, zbic.c, and unpacked by that)
            const u64 room = packed + packed / 4 + (1u << 16);
            u8 *frame = nx2_big_alloc(room, 0);
            const s64 len = frame ? nx2_zbic_to_zstd(raw, packed, frame, room) : -1;
            bad = len < 0 || ZSTD_decompress(img + mem[s], size[s], frame, (u64)len) != size[s];
            nx2_big_free(frame);
        } else {
            bad = lz4_block(raw, packed, img + mem[s], size[s]);
        }
        if (bad) {
            if (bad != 2) nx2_log("[game] %s: its part %d can't be unpacked: the file is damaged\n", name, (s64)s);
            nx2_big_free(nso), nx2_big_free(block);
            return -1;
        }
    }
    const u64 bss_size = r32(nso + 0x3C), dynstr_off = r32(nso + 0x90), dynstr_size = r32(nso + 0x94), dynsym_off = r32(nso + 0x98),
              dynsym_size = r32(nso + 0x9C);
    nx2_big_free(nso);
    const u64 mod0 = r32(img + 4);
    if (mod0 + 0x1C > image_size || memcmp(img + mod0, "MOD0", 4)) {
        nx2_log("[game] %s: no module header where its code says (%x)\n", name, mod0);
        nx2_big_free(block);
        return -1;
    }
    const u64 dynamic = mod0 + (s64)(s32)r32(img + mod0 + 4), bss_end = mod0 + (s64)(s32)r32(img + mod0 + 12),
              module_object = mod0 + (s64)(s32)r32(img + mod0 + 24);
    u64 end = image_size + bss_size;
    if (bss_end > end) end = bss_end;
    u64 symtab = mem[1] + dynsym_off, nsyms = dynsym_size / 24, strtab = mem[1] + dynstr_off, strsz = dynstr_size;
    if (!nsyms) {  // (a header that doesn't say: they are in the dynamic section, and their count in the hash table)
        u64 hash = 0;
        symtab = strtab = strsz = 0;
        for (u64 a = dynamic; a + 16 <= image_size; a += 16) {
            const u64 tag = r64(img + a), v = r64(img + a + 8);
            if (!tag) break;
            if (tag == 6 && !symtab) symtab = v;
            if (tag == 5 && !strtab) strtab = v;
            if (tag == 10 && !strsz) strsz = v;
            if (tag == 4 && !hash) hash = v;
        }
        if (hash && hash + 8 <= image_size) nsyms = r32(img + hash + 4);
        else if (strtab > symtab) nsyms = (strtab - symtab) / 24;
    }
    // where its system calls are: each "svc #n" of its code (pairs: the place, the number), for whole.c to turn into branches
    u32 count = 0;
    for (u64 a = 0; a + 4 <= size[0]; a += 4)
        if ((r32(img + a) & 0xFFE0001Fu) == 0xD4000001u) count++;
    u32 *svcs = malloc((count ? count : 1) * 8);
    if (!svcs) {
        nx2_big_free(block);
        return -1;
    }
    count = 0;
    for (u64 a = 0; a + 4 <= size[0]; a += 4) {
        const u32 w = r32(img + a);
        if ((w & 0xFFE0001Fu) == 0xD4000001u) svcs[2 * count] = (u32)a, svcs[2 * count + 1] = w >> 5 & 0xFFFF, count++;
    }
    strncpy(module_names[k], name, sizeof module_names[k] - 1);
    nx2_modules[k] = (Nx2Module){module_names[k], NULL, NULL, mem[2], (end + 0xFFF) & ~0xFFFull, mod0, dynamic, symtab, nsyms, strtab, strsz, module_object};
    nx2_images[k] = (Nx2Image){img, image_size, size[0], mem[1], size[1], svcs, count};
    nx2_log("[game] %s: %u KB (code %x, read-only data from %x, data from %x, ends at %x); %u system calls\n", name, image_size >> 10, size[0], mem[1], mem[2],
            nx2_modules[k].end, (u64)count);
    return 0;
}

// ---------------------------------------------------------------- the files: an image of them, made as it is read
// What tools/nup.py's RomFS writes to a card, made here in memory and never written: the header, then the files' bytes (each at
// a multiple of 16), then the four tables (folders and files by hash, and their entries). The tables are built; the files' bytes
// stay where they are, in the NUP file or in a folder of the card, and a read of the image goes to them.
typedef struct {
    const u8 *name;
    u32 name_len, parent, next, first_dir, first_file, meta;  // (next: its next sibling; meta: where its entry is in the table)
    u32 number;                                               // its place in the image's order of folders
} Dir;
typedef struct {
    const u8 *name;
    u32 name_len, dir, next, meta;
    u8 kind;        // 0: bytes of the NUP file as they are; 1: a zstd frame there; 2: a file of the card (a mod)
    u64 size, at;   // (at: where in the NUP file, or which of the card's files, in loose[])
    u64 at2;        // (a packed file: how many bytes it is kept as)
    u64 data;       // where its bytes are in the image's data
} Ent;
#define NONE 0xFFFFFFFFu
static struct {
    Dir *dirs;
    Ent *files;
    u32 ndirs, nfiles, dirs_room, files_room;
    u32 *slots, nslots;  // (a folder's children by name: an open table of numbers, a folder's with the top bit set)
    u32 *order;          // the files in the image's order
    u8 head[0x200], *tables;
    u64 tables_at, tables_size, size, data_size;
    char *loose;         // the card's files laid over the game's: their paths, one after another
    u64 loose_size, loose_room;
    u32 replaced, added;
} rom;

static u32 child_hash(u32 parent, const u8 *name, u32 len) {
    u32 h = parent * 2654435761u + len;
    for (u32 k = 0; k < len; k++) h = (h ^ name[k]) * 16777619u;
    return h;
}
static int slots_grow(void) {
    const u32 n = rom.nslots ? rom.nslots * 2 : 1u << 16;
    u32 *s = nx2_big_alloc(n * 4ull, 1);
    if (!s) return -1;
    memset(s, 0xFF, n * 4ull);
    for (u32 k = 0; k < rom.nslots; k++) {
        const u32 v = rom.slots[k];
        if (v == NONE) continue;
        const int is_dir = v >> 31;
        const u32 i = v & 0x7FFFFFFF;
        u32 at = (is_dir ? child_hash(rom.dirs[i].parent, rom.dirs[i].name, rom.dirs[i].name_len) : child_hash(rom.files[i].dir, rom.files[i].name, rom.files[i].name_len)) &
                 (n - 1);
        while (s[at] != NONE) at = (at + 1) & (n - 1);
        s[at] = v;
    }
    nx2_big_free(rom.slots);
    rom.slots = s, rom.nslots = n;
    return 0;
}
// a folder's child of that name (a folder if is_dir, else a file): its number, or NONE; *slot: where it is, or would go
static u32 child_find(u32 parent, const u8 *name, u32 len, int is_dir, u32 *slot) {
    u32 at = child_hash(parent, name, len) & (rom.nslots - 1);
    for (;; at = (at + 1) & (rom.nslots - 1)) {
        const u32 v = rom.slots[at];
        if (v == NONE) break;
        if ((int)(v >> 31) != is_dir) continue;
        const u32 i = v & 0x7FFFFFFF;
        if (is_dir ? rom.dirs[i].parent == parent && rom.dirs[i].name_len == len && !memcmp(rom.dirs[i].name, name, len)
                   : rom.files[i].dir == parent && rom.files[i].name_len == len && !memcmp(rom.files[i].name, name, len)) {
            *slot = at;
            return i;
        }
    }
    *slot = at;
    return NONE;
}
static u32 dir_under(u32 parent, const u8 *name, u32 len) {
    u32 slot, i = child_find(parent, name, len, 1, &slot);
    if (i != NONE) return i;
    if ((u64)(rom.ndirs + rom.nfiles + 1) * 2 > rom.nslots) {
        if (slots_grow()) return NONE;
        child_find(parent, name, len, 1, &slot);
    }
    if (rom.ndirs == rom.dirs_room) {
        const u32 room = rom.dirs_room ? rom.dirs_room * 2 : 4096;
        Dir *d = big_grown(rom.dirs, rom.dirs_room * sizeof(Dir), room * sizeof(Dir));
        if (!d) return NONE;
        rom.dirs = d, rom.dirs_room = room;
    }
    i = rom.ndirs++;
    rom.dirs[i] = (Dir){name, len, parent, rom.dirs[parent].first_dir, NONE, NONE, 0, 0};
    rom.dirs[parent].first_dir = i;
    rom.slots[slot] = i | 0x80000000u;
    return i;
}
// a file put in a folder: a new one, or in place of the one of that name that an earlier package had there
static int file_put(u32 dir, const u8 *name, u32 len, u8 kind, u64 size, u64 at, u64 at2) {
    u32 slot, i = child_find(dir, name, len, 0, &slot);
    if (i != NONE) {
        rom.files[i].kind = kind, rom.files[i].size = size, rom.files[i].at = at, rom.files[i].at2 = at2;
        rom.replaced++;
        return 0;
    }
    if ((u64)(rom.ndirs + rom.nfiles + 1) * 2 > rom.nslots) {
        if (slots_grow()) return -1;
        child_find(dir, name, len, 0, &slot);
    }
    if (rom.nfiles == rom.files_room) {
        const u32 room = rom.files_room ? rom.files_room * 2 : 16384;
        Ent *e = big_grown(rom.files, rom.files_room * sizeof(Ent), room * sizeof(Ent));
        if (!e) return -1;
        rom.files = e, rom.files_room = room;
    }
    i = rom.nfiles++;
    rom.files[i] = (Ent){name, len, dir, rom.dirs[dir].first_file, 0, kind, size, at, at2, 0};
    rom.dirs[dir].first_file = i;
    rom.slots[slot] = i;
    rom.added++;
    return 0;
}
// a folder of the NUP file, and all under it, laid over a folder of the image
static int lay_nup(u32 from, u32 to, int depth) {
    if (from >= nup.ndirs || depth > 64) return -1;
    const u8 *d = nup.dirs + 0x18 * from;
    const u32 first = r32(d + 8), count = r32(d + 12);
    for (u32 i = 0; i < count; i++) {
        const u32 node = nup_child(first, i);
        if (node < nup.ndirs) {
            const u8 *sub = nup.dirs + 0x18 * node;
            const u32 mine = dir_under(to, nup.pool + r32(sub), r32(sub + 4));
            if (mine == NONE || lay_nup(node, mine, depth + 1)) return -1;
        } else {
            const NupFile f = nup_file(node - nup.ndirs);
            if (f.codec > 1) {
                static u32 said;
                if (said++ < 4) nx2_log("[game] a file of the NUP file is kept in a way this doesn't read (%u): left out\n", (u64)f.codec);
                continue;
            }
            if (file_put(to, f.name, f.name_len, f.codec, f.size, f.at, f.kept)) return -1;
        }
    }
    return 0;
}
// a folder of the card, and all under it, laid over a folder of the image: the user's own files (mods). Their names and paths
// are kept in one block (rom.loose); a file's "at" is where its path begins there.
static char *loose_add(const char *text, u64 len) {
    if (rom.loose_size + len + 1 > rom.loose_room) {
        const u64 room = (rom.loose_room ? rom.loose_room * 2 : 1u << 16) + len + 1;
        char *l = big_grown(rom.loose, rom.loose_size, room);
        if (!l) return NULL;
        // (names already taken from the block are pointed at by entries: they are moved with it)
        if (l != rom.loose) {
            for (u32 k = 0; k < rom.ndirs; k++)
                if ((const char *)rom.dirs[k].name >= rom.loose && (const char *)rom.dirs[k].name < rom.loose + rom.loose_size)
                    rom.dirs[k].name = (const u8 *)(l + ((const char *)rom.dirs[k].name - rom.loose));
            for (u32 k = 0; k < rom.nfiles; k++)
                if ((const char *)rom.files[k].name >= rom.loose && (const char *)rom.files[k].name < rom.loose + rom.loose_size)
                    rom.files[k].name = (const u8 *)(l + ((const char *)rom.files[k].name - rom.loose));
        }
        rom.loose = l, rom.loose_room = room;
    }
    char *at = rom.loose + rom.loose_size;
    memcpy(at, text, len), at[len] = 0;
    rom.loose_size += len + 1;
    return at;
}
static int lay_card(FsFileSystem *sd, const char *path, u32 to, int depth) {
    static char open_path[FS_MAX_PATH];
    if (depth > 24) return 0;
    memset(open_path, 0, sizeof open_path);
    strncpy(open_path, path, sizeof open_path - 1);
    FsDir dir;
    if (fsFsOpenDirectory(sd, open_path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir)) return depth ? -1 : 1;  // (1: no such folder)
    static FsDirectoryEntry entries[16];
    for (;;) {
        s64 got = 0;
        if (fsDirRead(&dir, &got, 16, entries) || got <= 0) break;
        for (s64 e = 0; e < got; e++) {
            const u64 len = strlen(entries[e].name), path_len = strlen(path);
            if (path_len + 1 + len >= FS_MAX_PATH) continue;
            char full[FS_MAX_PATH];
            memcpy(full, path, path_len), full[path_len] = '/', memcpy(full + path_len + 1, entries[e].name, len + 1);
            char *kept = loose_add(full, path_len + 1 + len);  // (the whole path; its last part is the name)
            if (!kept) continue;
            // (the block may have moved: names are offsets into it until the end, see lay_card's caller)
            const u64 kept_at = (u64)(kept - rom.loose);
            const u8 *name = (const u8 *)rom.loose + kept_at + path_len + 1;
            if (entries[e].type == FsDirEntryType_Dir) {
                const u32 mine = dir_under(to, name, (u32)len);
                if (mine != NONE) {
                    // (this folder's own listing is done before going down: the entries' place is one, shared)
                    FsDirectoryEntry held[16];
                    memcpy(held, entries, sizeof held);
                    lay_card(sd, full, mine, depth + 1);
                    memcpy(entries, held, sizeof held);
                }
            } else {
                file_put(to, name, (u32)len, 2, (u64)entries[e].file_size, kept_at, 0);
            }
        }
    }
    fsDirClose(&dir);
    return 0;
}

// (the names in order, as the image has a folder's children: by their bytes, the shorter first where one begins the other)
static int by_name(const u8 *a, u32 an, const u8 *b, u32 bn) {
    const int c = memcmp(a, b, an < bn ? an : bn);
    return c ? c : (an > bn) - (an < bn);
}
static int dirs_by_name(const void *x, const void *y) {
    const Dir *a = &rom.dirs[*(const u32 *)x], *b = &rom.dirs[*(const u32 *)y];
    return by_name(a->name, a->name_len, b->name, b->name_len);
}
static int files_by_name(const void *x, const void *y) {
    const Ent *a = &rom.files[*(const u32 *)x], *b = &rom.files[*(const u32 *)y];
    return by_name(a->name, a->name_len, b->name, b->name_len);
}
static u32 name_hash(u32 parent, const u8 *name, u32 len) {
    u32 h = parent ^ 123456789u;
    for (u32 k = 0; k < len; k++) h = ((h >> 5) | (h << 27)) ^ name[k];
    return h;
}
static u32 buckets(u32 n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (!(n % 2) || !(n % 3) || !(n % 5) || !(n % 7) || !(n % 11) || !(n % 13) || !(n % 17)) n++;
    return n;
}
static u32 pad4(u32 n) { return (n + 3) & ~3u; }

static int rom_build(void) {
    // every folder's children put in order, and the folders numbered as the image has them: the root, then level by level
    u32 *sorted = nx2_big_alloc(((u64)rom.ndirs + rom.nfiles + 1) * 4, 0), *level = nx2_big_alloc(rom.ndirs * 4ull, 0);
    rom.order = nx2_big_alloc((rom.nfiles ? rom.nfiles : 1) * 4ull, 1);
    if (!sorted || !level || !rom.order) return -1;
    u32 queued = 0;
    level[queued++] = 0;
    for (u32 q = 0; q < queued; q++) {
        Dir *d = &rom.dirs[level[q]];
        d->number = q;
        u32 n = 0;
        for (u32 c = d->first_dir; c != NONE; c = rom.dirs[c].next) sorted[n++] = c;
        qsort(sorted, n, 4, dirs_by_name);
        d->first_dir = n ? sorted[0] : NONE;
        for (u32 k = 0; k < n; k++) rom.dirs[sorted[k]].next = k + 1 < n ? sorted[k + 1] : NONE, level[queued++] = sorted[k];
        d = &rom.dirs[level[q]];
        n = 0;
        for (u32 c = d->first_file; c != NONE; c = rom.files[c].next) sorted[n++] = c;
        qsort(sorted, n, 4, files_by_name);
        d->first_file = n ? sorted[0] : NONE;
        for (u32 k = 0; k < n; k++) rom.files[sorted[k]].next = k + 1 < n ? sorted[k + 1] : NONE;
    }
    nx2_big_free(sorted);
    if (queued != rom.ndirs) return -1;
    // where each entry is in its table, and each file's bytes in the data
    u32 dirs_size = 0, files_size = 0, in_order = 0;
    u64 data = 0;
    for (u32 q = 0; q < rom.ndirs; q++) rom.dirs[level[q]].meta = dirs_size, dirs_size += 0x18 + pad4(rom.dirs[level[q]].name_len);
    for (u32 q = 0; q < rom.ndirs; q++)
        for (u32 f = rom.dirs[level[q]].first_file; f != NONE; f = rom.files[f].next) {
            Ent *e = &rom.files[f];
            data = (data + 15) & ~15ull;
            e->meta = files_size, e->data = data;
            files_size += 0x20 + pad4(e->name_len), data += e->size;
            rom.order[in_order++] = f;
        }
    const u32 dir_buckets = buckets(rom.ndirs), file_buckets = buckets(rom.nfiles);
    rom.data_size = data;
    rom.tables_at = (0x200 + data + 15) & ~15ull;
    rom.tables_size = 4ull * dir_buckets + dirs_size + 4ull * file_buckets + files_size;
    rom.tables = nx2_big_alloc(rom.tables_size, 1);
    if (!rom.tables) return -1;
    u8 *dir_hash = rom.tables, *dir_table = dir_hash + 4ull * dir_buckets, *file_hash = dir_table + dirs_size, *file_table = file_hash + 4ull * file_buckets;
    memset(rom.tables, 0, rom.tables_size);
    memset(dir_hash, 0xFF, 4ull * dir_buckets), memset(file_hash, 0xFF, 4ull * file_buckets);
    for (u32 q = 0; q < rom.ndirs; q++) {
        const Dir *d = &rom.dirs[level[q]];
        const u32 parent = rom.dirs[d->parent].meta;
        u8 *t = dir_table + d->meta, *bucket = dir_hash + 4ull * (name_hash(parent, d->name, d->name_len) % dir_buckets);
        w32(t, parent), w32(t + 4, q && d->next != NONE ? rom.dirs[d->next].meta : NONE);
        w32(t + 8, d->first_dir != NONE ? rom.dirs[d->first_dir].meta : NONE), w32(t + 12, d->first_file != NONE ? rom.files[d->first_file].meta : NONE);
        w32(t + 16, r32(bucket)), w32(t + 20, d->name_len);
        memcpy(t + 0x18, d->name, d->name_len);
        w32(bucket, d->meta);
        for (u32 f = d->first_file; f != NONE; f = rom.files[f].next) {
            const Ent *e = &rom.files[f];
            u8 *u = file_table + e->meta, *b = file_hash + 4ull * (name_hash(d->meta, e->name, e->name_len) % file_buckets);
            w32(u, d->meta), w32(u + 4, e->next != NONE ? rom.files[e->next].meta : NONE);
            w64(u + 8, e->data), w64(u + 16, e->size);
            w32(u + 24, r32(b)), w32(u + 28, e->name_len);
            memcpy(u + 0x20, e->name, e->name_len);
            w32(b, e->meta);
        }
    }
    nx2_big_free(level);
    // the header: its own length, then where each table is and how long, then where the files' bytes begin
    u64 at = rom.tables_at;
    const u64 sizes[4] = {4ull * dir_buckets, dirs_size, 4ull * file_buckets, files_size};
    memset(rom.head, 0, sizeof rom.head);
    w64(rom.head, 0x50);
    for (int k = 0; k < 4; k++) w64(rom.head + 8 + 16 * k, at), w64(rom.head + 16 + 16 * k, sizes[k]), at += sizes[k];
    w64(rom.head + 0x48, 0x200);
    rom.size = at;
    return 0;
}

u64 nx2_game_rom_size(void) { return rom.size; }

// The card's files that are laid over the game's, a few kept open: the one read longest ago is closed for the next.
#define OPEN 6
static struct {
    FsFile file;
    u64 at, age;
    int open;
} kept[OPEN];
static u64 kept_clock;
static u32 kept_lock;
static u32 loose_read(u64 which, u64 offset, u8 *to, u64 size) {
    while (__atomic_exchange_n(&kept_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
    int slot = -1, oldest = -1;
    for (int k = 0; k < OPEN; k++)
        if (kept[k].open && kept[k].at == which) slot = k;
    for (int k = 0; k < OPEN && slot < 0; k++)
        if (!kept[k].open) {
            oldest = k;
            break;
        } else if (oldest < 0 || kept[k].age < kept[oldest].age) {
            oldest = k;
        }
    u32 rc = 0;
    if (slot < 0) {
        slot = oldest;
        if (kept[slot].open) fsFileClose(&kept[slot].file), kept[slot].open = 0;
        static char path[FS_MAX_PATH];
        memset(path, 0, sizeof path);
        strncpy(path, rom.loose + which, sizeof path - 1);
        rc = fsFsOpenFile(fsdevGetDeviceFileSystem("sdmc"), path, FsOpenMode_Read, &kept[slot].file);
        if (!rc) kept[slot].open = 1, kept[slot].at = which;
    }
    u64 got = 0;
    if (!rc) rc = fsFileRead(&kept[slot].file, (s64)offset, to, size, FsReadOption_None, &got);
    if (!rc && got != size) rc = 0x2EE202;
    if (!rc) kept[slot].age = ++kept_clock;
    __atomic_store_n(&kept_lock, 0, __ATOMIC_RELEASE);
    return rc;
}

// A file kept packed in the NUP file (one Zstandard frame each): unpacked whole the first time any of it is read, and kept while
// there is room, the one read longest ago let go first. The reads come on the game's threads, where nothing of the C library is
// to be used; so the unpacking, which asks it for memory, is a thread of the port's own, and the game's thread waits for it.
#define UNPACKED 64
static struct {
    u32 file;     // which of the image's files (NONE: a free place)
    u8 *bytes;
    u64 size, age;
    u32 held;     // threads copying out of it now: not let go while any is
} unpacked[UNPACKED];
static u64 unpacked_room = 192ull << 20, unpacked_total, unpacked_clock;
static u32 unpacked_lock, asking_lock;
static volatile u32 asked_file = NONE, asked_state, asked_rc;  // (state: 0 nothing asked, 1 asked, 2 done)
static Thread unpacker;
static int unpacker_up;
static void take(u32 *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
}
static void give(u32 *lock) { __atomic_store_n(lock, 0, __ATOMIC_RELEASE); }
// How much of the loader's own memory can still be had, found by asking: the largest piece, and how much in pieces of 8 MB.
// (Only where the C library may be used: the program's start, the thread that unpacks.)
static void room_said(const char *when) {
    extern u64 heapGetTotalSize(void), heapGetFreeSize(void);  // (libounce's: its allocator's own count)
    u64 all, largest;
    big_left(&all, &largest);
    nx2_log("[game] the loader's own memory, %s: of the %u MB kept for large pieces %u MB are free, %u MB in one piece; the allocator has %u MB free of %u\n", when,
            nx2_big_size >> 20, all >> 20, largest >> 20, heapGetFreeSize() >> 20, heapGetTotalSize() >> 20);
}

static void unpack_one(u32 file) {
    const Ent *e = &rom.files[file];
    const u64 kept_size = e->at2;
    u8 *out = NULL;
    u32 rc = 0, let_go = 0;
    const char *why = "";
    // room for it: the ones read longest ago go, down to none (a file larger than all the room there is still has its turn)
    for (int tries = 0; !out && tries <= UNPACKED; tries++) {
        take(&unpacked_lock);
        while (unpacked_total + e->size > unpacked_room || tries) {
            int oldest = -1;
            for (int k = 0; k < UNPACKED; k++)
                if (unpacked[k].file != NONE && !unpacked[k].held && (oldest < 0 || unpacked[k].age < unpacked[oldest].age)) oldest = k;
            if (oldest < 0) break;
            nx2_big_free(unpacked[oldest].bytes);
            unpacked_total -= unpacked[oldest].size, unpacked[oldest].file = NONE, unpacked[oldest].bytes = NULL, let_go++;
            if (tries) break;  // (no memory for it even so: one more goes at each try)
        }
        give(&unpacked_lock);
        out = nx2_big_alloc(e->size, 0);
    }
    if (!out) rc = 0x2EE202, why = "no memory for it";
    // The packed bytes are read a piece at a time and unpacked straight into the file's own memory: a film of 65 MB, read whole
    // and then unpacked, asked for twice its size at once, and a game was ended for want of it (log, 2026-11-25).
    enum { PIECE = 1 << 20 };
    static ZSTD_DCtx *z;
    static u8 *piece;
    if (!z) z = ZSTD_createDCtx();
    if (!piece) piece = malloc(PIECE);
    if (!rc && (!z || !piece)) rc = 0x2EE202, why = "no memory for the decoder";
    if (!rc) {
        ZSTD_DCtx_reset(z, ZSTD_reset_session_only);
        ZSTD_DCtx_setParameter(z, ZSTD_d_stableOutBuffer, 1);
        ZSTD_outBuffer to = {out, e->size, 0};
        size_t left = 1;
        for (u64 done = 0; !rc && done < kept_size && left;) {
            const u64 n = kept_size - done < PIECE ? kept_size - done : PIECE;
            if (nup_read(e->at + done, piece, n)) {
                rc = 0x2EE202, why = "the NUP file couldn't be read";
                break;
            }
            ZSTD_inBuffer from = {piece, n, 0};
            while (from.pos < from.size && left) {
                left = ZSTD_decompressStream(z, &to, &from);
                if (ZSTD_isError(left)) {
                    rc = 0x2EE202, why = "its bytes are not what was packed (damaged)";
                    break;
                }
            }
            done += n;
        }
        if (!rc && (left || to.pos != e->size)) rc = 0x2EE202, why = "its bytes end early (damaged)";
    }
    if (!rc) {
        take(&unpacked_lock);
        int place = -1;
        for (int k = 0; k < UNPACKED && place < 0; k++)
            if (unpacked[k].file == NONE) place = k;
        if (place < 0) {  // (every place taken: the one read longest ago gives its own)
            for (int k = 0; k < UNPACKED; k++)
                if (!unpacked[k].held && (place < 0 || unpacked[k].age < unpacked[place].age)) place = k;
            if (place >= 0) nx2_big_free(unpacked[place].bytes), unpacked_total -= unpacked[place].size;
        }
        if (place >= 0) unpacked[place].file = file, unpacked[place].bytes = out, unpacked[place].size = e->size, unpacked[place].age = ++unpacked_clock, unpacked[place].held = 0, unpacked_total += e->size;
        else rc = 0x2EE202, why = "every unpacked file is being read";
        give(&unpacked_lock);
    }
    if (rc) {
        nx2_big_free(out);
        static u32 said;
        if (said < 2) room_said("as a file couldn't be unpacked");
        if (said++ < 8) nx2_log("[game] a file kept packed in the NUP file couldn't be unpacked (%u KB to %u KB): %s; %u others were let go for it, %u MB of them are kept\n",
                                kept_size >> 10, e->size >> 10, why, (u64)let_go, unpacked_total >> 20);
    }
    asked_rc = rc;
}
static void unpacker_main(void *arg) {
    (void)arg;
    for (;;) {
        if (asked_state == 1) unpack_one(asked_file), asked_state = 2;
        else svcSleepThread(300000);
    }
}
static u32 packed_read(u32 file, u64 offset, u8 *to, u64 size) {
    for (int round = 0; round < 3; round++) {
        take(&unpacked_lock);
        int at = -1;
        for (int k = 0; k < UNPACKED && at < 0; k++)
            if (unpacked[k].file == file) at = k;
        if (at >= 0) unpacked[at].held++, unpacked[at].age = ++unpacked_clock;
        give(&unpacked_lock);
        if (at >= 0) {
            const int inside = offset + size <= unpacked[at].size;
            if (inside) memcpy(to, unpacked[at].bytes + offset, size);
            take(&unpacked_lock);
            unpacked[at].held--;
            give(&unpacked_lock);
            return inside ? 0 : 0x2EE202;
        }
        if (!unpacker_up) return 0x2EE202;
        take(&asking_lock);  // (one file asked for at a time; another thread may have asked for this one meanwhile: looked for again above)
        take(&unpacked_lock);
        int there = 0;
        for (int k = 0; k < UNPACKED && !there; k++) there = unpacked[k].file == file;
        give(&unpacked_lock);
        u32 rc = 0;
        if (!there) {
            asked_file = file, asked_state = 1;
            while (asked_state != 2) svcSleepThread(100000);
            rc = asked_rc, asked_state = 0;
        }
        give(&asking_lock);
        if (rc) return rc;
    }
    return 0x2EE202;
}

// A read of the image: from the header, from a file's bytes wherever they are, from the tables; nothing where there is nothing.
u32 nx2_game_rom_read(u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;
    while (size) {
        u64 take = size;
        u32 rc = 0;
        if (offset < 0x200) {
            if (take > 0x200 - offset) take = 0x200 - offset;
            memcpy(to, rom.head + offset, take);
        } else if (offset >= rom.tables_at) {
            if (offset - rom.tables_at >= rom.tables_size) return 0x2EE202;
            if (take > rom.tables_size - (offset - rom.tables_at)) take = rom.tables_size - (offset - rom.tables_at);
            memcpy(to, rom.tables + (offset - rom.tables_at), take);
        } else {
            const u64 in = offset - 0x200;
            // the last file that begins at or before this place: its bytes, or the gap after them
            u32 lo = 0, hi = rom.nfiles;
            while (lo < hi) {
                const u32 mid = lo + (hi - lo) / 2;
                if (rom.files[rom.order[mid]].data <= in) lo = mid + 1;
                else hi = mid;
            }
            const Ent *e = lo ? &rom.files[rom.order[lo - 1]] : NULL;
            const u64 next = lo < rom.nfiles ? rom.files[rom.order[lo]].data : rom.tables_at - 0x200;
            if (e && in < e->data + e->size) {
                if (take > e->data + e->size - in) take = e->data + e->size - in;
                if (e->kind == 0) {
                    // (a read that takes very long is said, a few dozen times at most: a game that stops for a moment as it
                    // reads says here whether the card was what it waited for. Not every slow one: a line said is a wait too.)
                    const u64 began = armGetSystemTick();
                    rc = nup_read(e->at + (in - e->data), to, take);
                    const u64 ms = armTicksToNs(armGetSystemTick() - began) / 1000000;
                    static u32 slow;
                    if (ms >= 100 && slow++ < 48)
                        nx2_log("[rom] a read of %u KB, %u MB into the NUP file, took %u ms\n", take >> 10, (e->at + (in - e->data)) >> 20, ms);
                } else if (e->kind == 2) {
                    rc = loose_read(e->at, in - e->data, to, take);
                } else {
                    rc = packed_read(rom.order[lo - 1], in - e->data, to, take);
                }
            } else {  // (between two files, or before the tables: nothing)
                if (take > next - in) take = next - in;
                memset(to, 0, take);
            }
        }
        if (rc) return rc;
        to += take, offset += take, size -= take, *got += take;
    }
    return 0;
}

// ---------------------------------------------------------------- the game's own table of heaps
// tools/heapdef.py, here: a game that keeps a stretch of address space apart from its heap (the heap "VirtualAddressMemory" of its
// table System/HeapSize/*.HeapDef.byml) sizes itself by the console's whole memory, and is told of that. The table is a BYML
// file, as a rule packed with Zstandard and one of the game's own dictionaries (Pack/ZsDic.pack.zs, an archive of them).
static const Ent *rom_find(const char *path) {
    u32 dir = 0;
    for (;;) {
        const char *slash = strchr(path, '/');
        u32 slot;
        if (!slash) {
            const u32 f = child_find(dir, (const u8 *)path, (u32)strlen(path), 0, &slot);
            return f == NONE ? NULL : &rom.files[f];
        }
        dir = child_find(dir, (const u8 *)path, (u32)(slash - path), 1, &slot);
        if (dir == NONE) return NULL;
        path = slash + 1;
    }
}
// a file of the image, whole, in memory of its own (to be freed); NULL if it can't be had
static u8 *rom_whole(const Ent *e) {
    if (!e || e->size > (64u << 20)) return NULL;
    u8 *out = nx2_big_alloc(e->size, 0);
    u32 rc = out ? 0 : 1;
    if (!rc && e->kind == 0) rc = nup_read(e->at, out, e->size);
    else if (!rc && e->kind == 2) rc = loose_read(e->at, 0, out, e->size);
    else if (!rc) {
        u8 *packed = nx2_big_alloc(e->at2, 0);
        rc = !packed || nup_read(e->at, packed, e->at2) || ZSTD_decompress(out, e->size, packed, e->at2) != e->size;
        nx2_big_free(packed);
    }
    if (rc) nx2_big_free(out), out = NULL;
    return out;
}
// a Zstandard frame unpacked with a dictionary (or none): its bytes, in memory of their own, or NULL
static u8 *unpack_with(const u8 *raw, u64 size, const u8 *dict, u64 dict_size, u64 *out_size) {
    unsigned long long whole = ZSTD_getFrameContentSize(raw, size);
    if (whole == ZSTD_CONTENTSIZE_ERROR) return NULL;
    if (whole == ZSTD_CONTENTSIZE_UNKNOWN || whole > (64u << 20)) whole = 16u << 20;
    u8 *out = nx2_big_alloc(whole, 0);
    ZSTD_DCtx *z = ZSTD_createDCtx();
    const size_t got = out && z ? ZSTD_decompress_usingDict(z, out, whole, raw, size, dict, dict_size) : (size_t)-1;
    ZSTD_freeDCtx(z);
    if (!out || ZSTD_isError(got)) {
        nx2_big_free(out);
        return NULL;
    }
    *out_size = got;
    return out;
}
// (a BYML map's entry of that key: its kind and its value, or kind 0)
static u32 byml_get(const u8 *b, u64 size, u64 map, const char *key, u32 *value) {
    const u64 keys = r32(b + 4);
    if (map + 4 > size || b[map] != 0xC1 || keys + 4 > size || b[keys] != 0xC2) return 0;
    const u32 n = r32(b + map) >> 8, nkeys = r32(b + keys) >> 8;
    for (u32 k = 0; k < n && map + 12 + 8ull * k <= size; k++) {
        const u32 w = r32(b + map + 4 + 8 * k), index = w & 0xFFFFFF;
        if (index >= nkeys || keys + 8 + 4ull * index > size) continue;
        const u64 at = keys + r32(b + keys + 4 + 4 * index);
        if (at < size && !strncmp((const char *)b + at, key, size - at) ) {
            *value = r32(b + map + 8 + 8 * k);
            return w >> 24;
        }
    }
    return 0;
}
static void heaps_read(void) {
    // the table: of those in System/HeapSize, the Switch 2's own ("Ounce" in its name), and of those the game's as sold ("Product")
    u32 slot;
    const u32 system = child_find(0, (const u8 *)"System", 6, 1, &slot);
    const u32 folder = system == NONE ? NONE : child_find(system, (const u8 *)"HeapSize", 8, 1, &slot);
    const Ent *table = NULL;
    int best = -1;
    for (u32 f = folder == NONE ? NONE : rom.dirs[folder].first_file; f != NONE; f = rom.files[f].next) {
        const Ent *e = &rom.files[f];
        char name[160] = {0};
        memcpy(name, e->name, e->name_len < sizeof name - 1 ? e->name_len : sizeof name - 1);
        if (!strstr(name, ".HeapDef.byml")) continue;
        const int score = (strstr(name, "Ounce") ? 2 : 0) + (strstr(name, "Product") ? 1 : 0);
        if (score > best) best = score, table = e;
    }
    if (!table) return;
    u8 *raw = rom_whole(table), *byml = NULL;
    u64 size = table->size;
    if (raw && size >= 4 && r32(raw) == 0xFD2FB528u) {  // (packed: with one of the game's dictionaries, or with none)
        const Ent *pack = rom_find("Pack/ZsDic.pack.zs");
        u8 *packed = rom_whole(pack), *dicts = NULL;
        u64 dicts_size = 0;
        if (packed) dicts = unpack_with(packed, pack->size, NULL, 0, &dicts_size);
        nx2_big_free(packed);
        if (dicts && dicts_size > 0x28 && !memcmp(dicts, "SARC", 4) && dicts[6] == 0xFF && dicts[7] == 0xFE) {
            const u64 data = r32(dicts + 0xC), count = dicts[0x1A] | (u64)dicts[0x1B] << 8;
            for (u64 k = 0; k < count && !byml && 0x30 + 16 * k <= dicts_size; k++) {
                const u64 start = data + r32(dicts + 0x20 + 16 * k + 8), end = data + r32(dicts + 0x20 + 16 * k + 12);
                if (start <= end && end <= dicts_size) byml = unpack_with(raw, table->size, dicts + start, end - start, &size);
            }
        }
        nx2_big_free(dicts);
        if (!byml) byml = unpack_with(raw, table->size, NULL, 0, &size);
        nx2_big_free(raw);
    } else {
        byml = raw;
    }
    u32 v = 0, kind = 0;
    if (byml && size > 16 && byml[0] == 'Y' && byml[1] == 'B') {
        const u64 root = r32(byml + 12);
        if (byml_get(byml, size, root, "Heaps", &v) == 0xC1 && byml_get(byml, size, v, "VirtualAddressMemory", &v) == 0xC1) kind = byml_get(byml, size, v, "Size", &v);
    }
    u64 kb = 0;
    if (kind == 0xD1 || kind == 0xD3) kb = v;
    else if ((kind == 0xD4 || kind == 0xD5) && (u64)v + 8 <= size) kb = r64(byml + v);
    if (!byml) nx2_log("[game] the game's table of heaps (%u KB) couldn't be unpacked: it is told of memory as a game that has none\n", table->size >> 10);
    else if (kb >= 1024 && kb < (16ull << 20)) {
        nx2_game_apart_mb = (u32)(kb >> 10), nx2_game_mb = 8992, nx2_game_room_mb = 64;
        nx2_log("[game] the game's own table of heaps keeps %u MB of address space apart from its heap: it is told of the console's 8992 MB, as on its console\n",
                (u64)nx2_game_apart_mb);
    } else
        nx2_log("[game] the game's own table of heaps keeps no address space apart from its heap\n");
    nx2_big_free(byml);
}

// ---------------------------------------------------------------- the game's folder and its settings
// game.cfg: lines of key=value (docs/layout.md of nx2-flow). The value of a key, or "" when it isn't there.
static void setting(const char *key, char *to, u64 room) {
    to[0] = 0;
    FILE *f = fopen(nx2_file("game.cfg"), "r");
    if (!f) return;
    char line[256];
    const u64 len = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, len) && line[len] == '=') {
            u64 n = strlen(line + len + 1);
            while (n && (line[len + n] == '\n' || line[len + n] == '\r' || line[len + n] == ' ')) n--;
            if (n >= room) n = room - 1;
            memcpy(to, line + len + 1, n), to[n] = 0;
        }
    fclose(f);
}

// <folder>/readtest: how fast the card gives pieces of the NUP file, measured before the game starts, and of one more file beside
// it if the switch file names one (a path of the card, as "/folder/file"): for telling whether a file that was written on the
// console is read slower than one written on a computer. Pieces of three sizes, from places all over the file and from places
// each a little after the last.
static void read_test(void) {
    FILE *t = fopen(nx2_file("readtest"), "rb");
    if (!t) return;
    char other[256] = "";
    if (fgets(other, sizeof other, t)) {
        u64 n = strlen(other);
        while (n && (other[n - 1] == '\n' || other[n - 1] == '\r' || other[n - 1] == ' ')) other[--n] = 0;
    }
    fclose(t);
    static u8 piece[1 << 20] __attribute__((aligned(0x1000)));
    FsFile second;
    const int two = other[0] && R_SUCCEEDED(fsFsOpenFile(fsdevGetDeviceFileSystem("sdmc"), other, FsOpenMode_Read, &second));
    static const u64 sizes[3] = {64u << 10, 256u << 10, 900u << 10};
    for (int f = 0; f < (two ? 2 : 1); f++) {
        FsFile *file = f ? &second : &nup.file;
        s64 size = 0;
        fsFileGetSize(file, &size);
        for (int far = 1; far >= 0; far--)
            for (int s = 0; s < 3; s++) {
                u64 seed = 12345, at = (u64)size / 3, sum = 0, most = 0;
                u32 bad = 0;
                for (int k = 0; k < 48; k++) {
                    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                    if (far) at = (seed >> 20) % ((u64)size - (2u << 20));
                    else at = (at + (4u << 20) + ((seed >> 40) & 0xFFFFF)) % ((u64)size - (2u << 20));
                    u64 got = 0;
                    const u64 began = armGetSystemTick();
                    if (R_FAILED(fsFileRead(file, (s64)at, piece, sizes[s], FsReadOption_None, &got)) || got != sizes[s]) bad++;
                    const u64 us = armTicksToNs(armGetSystemTick() - began) / 1000;
                    sum += us;
                    if (us > most) most = us;
                }
                nx2_log("[readtest] %s (%u MB): 48 pieces of %u KB, %s: %u ms each on average, the slowest %u ms%s\n", f ? other : "the NUP file", (u64)size >> 20,
                        sizes[s] >> 10, far ? "from all over the file" : "each a few MB after the last", sum / 48 / 1000, most / 1000, bad ? " (SOME COULD NOT BE READ)" : "");
            }
    }
    if (two) fsFileClose(&second);
}

// How far the loader's own stretch of memory has to reach for this game, as an address (0: as far as it does now). main.c asks
// once the game is open, and gives the game what is past it.
u64 nx2_game_room_end(void) {
    u64 all = 0, largest = 0;
    u32 packed = 0;
    for (u32 k = 0; k < rom.nfiles; k++)
        if (rom.files[k].kind == 1) {
            packed++, all += rom.files[k].size;
            if (rom.files[k].size > largest) largest = rom.files[k].size;
        }
    // The files unpacked as they are read: the room they are kept in and the largest of them besides, or all of them where
    // that is less; and 16 MB over, for their being whole pages each. A game with none packed: 4 MB, for what isn't foreseen.
    u64 cache = 4u << 20;
    if (packed) {
        cache = unpacked_room + largest;
        if (cache > all) cache = all;
        cache += 16u << 20;
    }
    return big_end_wanted(cache);
}

// 0: the game of this NUP file is ready to be started: its modules unpacked, its files' image made, its folder there.
int nx2_game_open(const char *path) {
    if (nup_open(path)) return -1;
    // the packages that make the game: its own, then its updates by their versions
    u32 layers[8], count = 0;
    u64 title = 0;
    for (u32 k = 0; k < nup.npackages && count < 8; k++) {
        const Package p = nup_package(k);
        if (p.kind > 1) continue;
        u32 at = count++;
        for (; at; at--) {  // (kept in order: the game first, an update after any of an earlier version)
            const Package before = nup_package(layers[at - 1]);
            if (before.kind < p.kind || (before.kind == p.kind && before.version <= p.version)) break;
            layers[at] = layers[at - 1];
        }
        layers[at] = k;
        if (!p.kind && !title) title = p.title;
    }
    if (!count) {
        nx2_log("[game] no game in that NUP file (its %u packages are of other kinds)\n", (u64)nup.npackages);
        return -1;
    }
    if (!title) title = nup_package(layers[0]).title;
    static const char hex[] = "0123456789abcdef";
    for (int k = 0; k < 16; k++) nx2_game_id[k] = hex[title >> (60 - 4 * k) & 15];
    nx2_game_id[16] = 0;
    strcpy(home, "/nx2-flow/"), strcat(home, nx2_game_id);
    mkdir("sdmc:/nx2-flow", 0777);
    {
        char dir[96];
        snprintf(dir, sizeof dir, "sdmc:%s", home);
        mkdir(dir, 0777);
    }
    nx2_log("[game] %s: the game %s, of %u package(s); its folder is sdmc:%s\n", path, nx2_game_id, (u64)count, home);

    // the program: the newest package's that has one
    s64 code = -1;
    for (u32 k = 0; k < count; k++) {
        const Package p = nup_package(layers[k]);
        if (p.exefs != NONE && nup_find(p.exefs, "main") >= 0) code = k;
    }
    if (code < 0) {
        nx2_log("[game] no program in that NUP file (a package with an exefs folder that has \"main\")\n");
        return -1;
    }
    const Package with_code = nup_package(layers[code]);
    static const char *const order[] = {"main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
    for (u32 k = 0; k < sizeof order / sizeof *order; k++) {
        const s64 file = nup_find(with_code.exefs, order[k]);
        if (file < 0) continue;
        if (nx2_module_count >= MODULES || module_load(nx2_module_count, order[k], (u64)file)) return -1;
        nx2_module_count++;
    }
    nx2_whole = (int)nx2_module_count;
    {  // (the loader service maps eight images for a program, this one's own among them: the modules that needn't be whole are
        // made into one image when the game needs the room; merge.c. game.cfg's "modules": "whole" or "merge", to say which)
        char wish[16];
        setting("modules", wish, sizeof wish);
        if (nx2_merge_plan(wish)) return -1;
    }
    {  // what its own description asks of the console, which a program has from the title it is started over (main.c says both)
        const s64 npdm = nup_find(with_code.exefs, "main.npdm");
        u8 head[0x18];
        if (npdm >= 0 && !nup_file((u64)npdm).codec && nup_file((u64)npdm).size >= sizeof head && !nup_read(nup_file((u64)npdm).at, head, sizeof head) &&
            !memcmp(head, "META", 4)) {
            nx2_game_remote_mb = r32(head + 0x10) >> 20, nx2_game_sysres_kb = r32(head + 0x14) >> 10;
            if (nx2_game_remote_mb > 4096) nx2_game_remote_mb = 0;
        }
    }

    // the files: each package's over the ones before, then the user's own folder
    rom.dirs = nx2_big_alloc(4096 * sizeof(Dir), 1), rom.dirs_room = 4096;
    if (!rom.dirs || slots_grow()) return -1;
    rom.dirs[0] = (Dir){(const u8 *)"", 0, 0, NONE, NONE, NONE, 0, 0}, rom.ndirs = 1;
    for (u32 k = 0; k < count; k++) {
        const Package p = nup_package(layers[k]);
        if (p.romfs == NONE) continue;
        rom.replaced = rom.added = 0;
        if (lay_nup(p.romfs, 0, 0)) {
            nx2_log("[game] the files of package %u couldn't be listed (no memory, or a table that doesn't hold together)\n", (u64)layers[k]);
            return -1;
        }
        nx2_log("[game] files: %u of %s (version %u)%s\n", (u64)(rom.replaced + rom.added), p.kind ? "an update" : "the game", (u64)p.version,
                rom.replaced ? ", some in place of earlier ones" : "");
    }
    setting("modset", modset, sizeof modset);
    {
        char folder[FS_MAX_PATH];
        if (modset[0]) snprintf(folder, sizeof folder, "%s/modsets/%s", home, modset);
        else snprintf(folder, sizeof folder, "%s/mods", home);
        rom.replaced = rom.added = 0;
        const u64 names_before = rom.loose_size;
        (void)names_before;
        if (!lay_card(fsdevGetDeviceFileSystem("sdmc"), folder, 0, 0))
            nx2_log("[mods] sdmc:%s: %u files laid over the game's own, %u more that the game hasn't\n", folder, (u64)rom.replaced, (u64)rom.added);
    }
    if (rom_build()) {
        nx2_log("[game] the image of the game's %u files couldn't be made (no memory for its tables)\n", (u64)rom.nfiles);
        return -1;
    }
    nx2_log("[game] the game's files: %u in %u folders, %u MB as one image (its tables: %u KB, made here)\n", (u64)rom.nfiles, (u64)rom.ndirs, rom.size >> 20,
            rom.tables_size >> 10);
    {  // (files kept packed: the thread that unpacks them as they are read)
        u32 packed = 0;
        for (u32 k = 0; k < rom.nfiles; k++) packed += rom.files[k].kind == 1;
        for (int k = 0; k < UNPACKED; k++) unpacked[k].file = NONE;
        if (packed && R_SUCCEEDED(threadCreate(&unpacker, unpacker_main, NULL, NULL, 0x40000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&unpacker))) unpacker_up = 1;
        if (packed) nx2_log("[game] %u of them are kept packed in the NUP file: each is unpacked whole when it is first read%s\n", (u64)packed,
                            unpacker_up ? "" : " (the thread for it couldn't be started: they can't be read)");
    }
    heaps_read();
    {  // game.cfg's "save": which of the game's slots of saved data is in use (each a folder of its own; "save" when none is named)
        char slot[64];
        setting("save", slot, sizeof slot);
        int plain = slot[0] && strcmp(slot, "default") && !strstr(slot, "..");
        for (const char *c = slot; *c; c++) plain = plain && *c != '/' && *c != '\\' && *c != ':';
        if (plain) {
            char dir[160];
            snprintf(dir, sizeof dir, "sdmc:%s/saves", home);
            mkdir(dir, 0777);
            snprintf(save, sizeof save, "saves/%s", slot);
            nx2_log("[game] game.cfg: the saved data in use is the slot \"%s\" (sdmc:%s/%s)\n", slot, home, save);
        }
    }
    {  // game.cfg's "memory": what the game is told of, in place of what the loader would decide
        char memory[16];
        setting("memory", memory, sizeof memory);
        const u32 mb = (u32)strtoul(memory, NULL, 10);
        if (mb >= 2048 && mb <= 8992) nx2_game_mb = mb, nx2_game_room_mb = 64, nx2_log("[game] game.cfg: the game is told of %u MB\n", (u64)mb);
    }
    room_said("with the game's modules and the table of its files in it");
    read_test();
    return 0;
}
#endif
