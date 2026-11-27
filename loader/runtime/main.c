// nx2-recomp: the port's own start. The game's modules are in this program already (tools/relink.py); this sets the log up, gives the
// game a heap, does the loader's work and hands over to the game, for good: the main thread is the game's from then on.
#include <switch.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "nx2.h"

// The host library's heap is the memory the homebrew loader hands over, nearly all a game may have. The game's own heap is taken off
// its top before the C library has any of it: asked of malloc afterwards, a block of 2 GB or more was refused on a Switch with
// 2.9 GB free (1792 MB was the most given; log, 2026-11-22), and a game needs 1.92 GB in one piece.
static u8 *early_block;
static u64 early_size, early_all;
// (a Switch 2 game's: the part of the heap given back to the kernel, below)
static u64 heap_was, heap_given, heap_at, heap_host;
static u32 heap_given_rc;
#ifdef NX2_LOADER
static int vam_auto;  // the host has no system resource and the game was made for one: the pool of vam.c, unasked
static u64 loader_room;  // (the game loader's room for a game's modules and unpacked files, below)
#endif
void __libnx_initheap(void) {
    extern char *fake_heap_start, *fake_heap_end;
    u8 *addr = NULL;
    u64 size = 0;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr(), size = envGetHeapOverrideSize();
    } else {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (total > used + 0x200000) size = (total - used - 0x200000) & ~0x1FFFFFull;
        if (!size) size = 0x2000000 * 16;
        if (R_FAILED(svcSetHeapSize((void **)&addr, size))) addr = NULL, size = 0;
    }
    early_all = size;
#ifdef NX2_GAME_MB
    // A Switch 2 game's SDK takes memory of its own from the kernel as it runs (MapPhysicalMemory, which gives what the process
    // has not yet taken), and the loader this is started by has taken all there is for the heap it hands over: 4 MB were left,
    // A game asked for 5 at its start, was refused (10801) and ended itself; with 256 MB given back to the
    // kernel it went on to ask for all the memory it had been told of that same way, 7748 MB in one piece, where a game asks for its own as a heap (SetHeapSize) and for 4 KB that way (logs, 2026-11-25). A game's memory is its SDK's to
    // take either way, so none is set aside here: the heap is made as small as the host's own part, 256 MB, before anything is
    // put in it, and the rest is the kernel's again, to give the game as it asks (svc.c: its SetHeapSize makes the kernel's heap
    // larger, after the host's part; its MapPhysicalMemory is the kernel's own).
    {
        // (128 MB, where it was 256: what the port takes of its own heap while a game runs is a few MB, and its table of a mods
        // folder's files; the other half is the game's now, which a game that maps memory as it runs was short of)
        u64 host = 128ull << 20;
        // (with the modules kept whole, whole.c, each one's image and bss are put together in the host's heap before the loader
        // service maps them: that much more is the host's. A game's seven are 190 MB, and with 256 MB in all the sixth had
        // none; log, 2026-11-25.)
        for (unsigned k = 0; nx2_whole && k < nx2_module_count; k++)
            if (nx2_images[k].image) host += (nx2_modules[k].end + 0x2FFFFF) & ~0x1FFFFFull;
#ifdef NX2_LOADER
        // (The game loader doesn't know its game yet, nor how large the game's modules are: they are read from the NUP file
        // once the program is up, game.c. Room is kept for them, for the table of the game's files, and for the files that are
        // unpacked as they are read: 384 MB, or what "room=<MB>" says where the program was started with that.)
        {
            u64 room = 384;
            const char *said = envGetArgv();
            for (const char *c = said; c && *c; c++)
                if (c[0] == 'r' && c[1] == 'o' && c[2] == 'o' && c[3] == 'm' && c[4] == '=' && (c == said || c[-1] == ' ')) {
                    u64 mb = 0;
                    for (const char *d = c + 5; *d >= '0' && *d <= '9'; d++) mb = mb * 10 + (u64)(*d - '0');
                    if (mb >= 128 && mb <= 2048) room = mb;
                }
            host += room << 20;
            loader_room = room << 20;
        }
#endif
        u64 base = 0, end = 0;
        if (addr && size > 4 * host && R_SUCCEEDED(svcGetInfo(&base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0))) {
            for (end = base;;) {  // where the heap ends now
                MemoryInfo mi;
                u32 pi;
                if (R_FAILED(svcQueryMemory(&mi, &pi, end)) || (mi.type & 0xFF) != MemType_Heap) break;
                end = mi.addr + mi.size;
            }
            const u64 new_size = ((u64)addr + host - base + 0x1FFFFF) & ~0x1FFFFFull, new_end = base + new_size;
            if ((u64)addr >= base && end > new_end + host && (u64)addr + size <= end) {
                void *at = NULL;
                heap_was = end - base;
                heap_given_rc = svcSetHeapSize(&at, new_size);
                if (R_SUCCEEDED(heap_given_rc)) {
                    heap_given = end - new_end, heap_at = base, heap_host = new_size;
                    size = new_end - (u64)addr;
                }
            }
        }
    }
#endif
    // The host's own (the log, the table of symbols, two threads' stacks) is a few MB, and it keeps 12: a game
    // stops at its start, in a loop of one instruction, unless its heap is over 3136 MB (main+5499f4; log, 2026-11-22), and the
    // loader hands over 3153.
    // (12 was still too much: with 3140 MB the last of the game's four heaps, which are 3136 MB together, had a null; crash report,
    // 2026-11-22. The table of symbols is 1 MB of the 4.)
#ifdef __SWITCH2__
    // A Switch 2 hands over some 9 GB. The game has what a Switch would give it, 3200 MB, and no more (its sums were made for that
    // much); the rest is the host's, whose OpenGL the game draws with.
    // (A Switch 2 game's own asks for far more: a game's heaps are some 7.5 GB together, its HeapDef file says, and it
    // ended itself at its start with 3200 MB; log, 2026-11-24. Such a port is built with the size, tools/build.py --heap, and the
    // host keeps less: the game's graphics are its own SDK's.)
#ifdef NX2_GAME_MB
    // (with the heap made the host's part alone, above, all that is left here is the host's: no block for the game)
    const u64 keep = heap_given ? size : 512ull << 20, most = (u64)NX2_GAME_MB << 20;
#else
    const u64 keep = 1024ull << 20, most = 3200ull << 20;
#endif
    u64 game = size > keep ? (size - keep) & ~0x1FFFFFull : 0;
    if (game > most) game = most;
#else
    const u64 keep = 4ull << 20;
    u64 game = size > keep ? (size - keep) & ~0x1FFFFFull : 0;
#endif
    u8 *top = (u8 *)(((u64)addr + size) & ~0x1FFFFFull);
    if (game >= (256ull << 20) && top - game >= addr + keep / 2) {
        early_block = top - game, early_size = game;
        size = (u64)(early_block - addr);
    }
#ifdef NX2_LOADER
    // (The game loader's room is not the allocator's to give: a game's modules and its unpacked files are large pieces, which
    // the library's allocator serves badly. The top of the host's part, as much as the room is, is game.c's own to give out.)
    if (loader_room && size > loader_room + (96ull << 20)) {
        nx2_big_size = loader_room & ~0xFFFull;
        nx2_big_at = (u8 *)(((u64)addr + size - nx2_big_size) & ~0xFFFull);
        size = (u64)(nx2_big_at - addr);
    }
#endif
    fake_heap_start = (char *)addr, fake_heap_end = (char *)addr + size;
}

// Before this program goes back to the loader that started it (not when the system closes it): the loader hands the next program
// the heap as it was, so the part given back to the kernel is asked for again. If it can't be had, the process is ended here: a
// program started on a heap with its top missing would end worse.
static void heap_back(void) {
    if (!heap_given) return;
    void *at = NULL;
    const Result rc = svcSetHeapSize(&at, heap_was);
    nx2_log("[nx2] the %u MB of the heap given back to the kernel are taken again: %x\n", heap_given >> 20, (u64)rc);
    nx2_log_flush();
    if (R_FAILED(rc)) nx2_exit();
    heap_given = 0;
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
#ifdef NX2_LOADER
    // The game: the NUP file named as the program was started (nx2-flow's menu does that: docs/layout.md), read before
    // anything else, since the log's own folder is the game's. "menu=<program>" is what to start again when the game is closed.
    const char *menu = NULL;
    for (int k = 2; k < argc; k++)
        if (!strncmp(argv[k], "menu=", 5)) menu = argv[k] + 5;
    // (started with no file named, as from the homebrew menu itself: the one sdmc:/ns2LOAD/next.txt names, if that is there)
    static char named[512];
    if (argc < 2) {
        FILE *next = fopen("sdmc:/ns2LOAD/next.txt", "r");
        if (next) {
            if (fgets(named, sizeof named, next)) {
                size_t n = strlen(named);
                while (n && (named[n - 1] == 10 || named[n - 1] == 13 || named[n - 1] == ' ')) named[--n] = 0;
            }
            fclose(next);
        }
    }
    const char *which = argc >= 2 ? argv[1] : named[0] ? named : NULL;
    const int opened = which ? nx2_game_open(which) : -2;
    // (The room kept for the game's modules, tables and unpacked files was a guess made before the game was known. Now that it
    // is, what the loader won't use of it is the game's: the program's heap is made that much smaller, as it was made the
    // host's part alone before. game.c says how far it needs to reach.)
    u64 room_back = 0;
    if (!opened && heap_given) {
        const u64 end = nx2_game_room_end(), host = end > heap_at ? (end - heap_at + 0x1FFFFF) & ~0x1FFFFFull : 0;
        void *at = NULL;
        if (host && host < heap_host && R_SUCCEEDED(svcSetHeapSize(&at, host))) {
            room_back = heap_host - host;
            heap_given += room_back, heap_host = host;
            nx2_game_room_trimmed(heap_at + host);
        }
    }
    nx2_log_start();
    if (room_back) nx2_log("[nx2] %u MB of the room kept for the game's modules and files are not needed for this game: they are the game's to take\n", room_back >> 20);
    if (menu && envHasNextLoad()) envSetNextLoad(menu, menu);
    if (opened) {
        nx2_log("[nx2] %s: the game can't be started (the lines above say why)\n", which ? which : "no NUP file was named: this program is started by ns2LOAD's menu");
        nx2_crash("the game's NUP file could not be read, or holds a game this loader can't run (the log's lines that begin \"[game]\" say which and why)");
        nx2_log_end();
        heap_back();
        return 1;
    }
#else
    nx2_log_start();
#endif
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const u32 hos = hosversionGet();
    nx2_log("[nx2] nx2-recomp, built for %s; Horizon %u.%u.%u; applet type %u; memory %u MB, %u MB used\n",
#ifdef __SWITCH2__
            "Switch 2",
#else
            "Switch",
#endif
            (u64)HOSVER_MAJOR(hos), (u64)HOSVER_MINOR(hos), (u64)HOSVER_MICRO(hos), (u64)appletGetAppletType(), total >> 20, used >> 20);

    // the game's heap: half of what's free, 1 GB at most, in 2 MB steps as the kernel gives it
    // (asked of malloc, not worked out from the kernel's count of free memory: the host library has taken all of it for its own heap
    // by now, and on a Switch 2 that count said 4 MB free of 9120, which left the game none; log, 2026-11-22)
    // Each step said and written to the card before the next: with 1 GB asked of memalign in one go, a Switch 2 closed the program
    // before the log's first write.
    nx2_log_flush();
    // 2.5 GB first, then less by 256 MB at a time: a game asks its allocator for 1.92 GB in one piece at its start, and with
    // 1 GB had a null for it, which it used (crash report, 2026-11-22). A Switch has 3.2 GB for a game in all.
    u64 heap = 2560ull << 20;
    u8 *block = NULL;
    nx2_log("[nx2] the memory handed to this program: %u MB; %u MB of it set aside for the game's heap\n", early_all >> 20, early_size >> 20);
    int live = 0;  // (a Switch 2 game's: its memory is the kernel's to give as the game asks, none set aside)
    if (heap_was) {
        nx2_log("[nx2] of the loader's heap of %u MB, %u MB given back to the kernel for the game's SDK to take as it asks (%x); the host keeps %u MB\n",
                heap_was >> 20, heap_given >> 20, (u64)heap_given_rc, heap_host >> 20);
#ifdef NX2_GAME_MB
        if (heap_given) {
            // What the game is told it has: what it was built to be given, and not quite all that the kernel has (64 MB are left
            // for what its SDK takes besides: its threads' stacks). Not more than that: with 512 MB left, a game was
            // told of 7554 MB where it had had 7748, and ended as it loaded its world (its heaps are some 7.5 GB together; log,
            // 2026-11-25). A game, which asks the kernel for everything it is told of in one piece, only tries that: it gives
            // it back at once and takes what it needs as it goes.
            // A game that keeps a stretch of address space apart from its heap (NX2_GAME_APART_MB, read from its own table of
            // heaps when the port is made: tools/heapdef.py) is told of that much more than there is: it takes as its heap what
            // it is told of less that stretch, into which it maps as it goes, and its fixed heaps only fit in what the console's
            // own figure leaves. One such game, told of 7800 MB, gave its graphics a heap of no size and ended at its start; told
            // of the console's 8992 it runs (2026-11-25). Its heap is still no more than what there is.
#ifndef NX2_GAME_APART_MB
#define NX2_GAME_APART_MB 0
#endif
#ifdef NX2_GAME_ROOM_MB  // (no figure was given for the game: this much of what there is stays outside what it is told of)
            const u64 slack = (u64)NX2_GAME_ROOM_MB << 20, apart = (u64)NX2_GAME_APART_MB << 20;
#else
            const u64 slack = 64ull << 20, apart = (u64)NX2_GAME_APART_MB << 20;
#endif
            u64 most = (u64)NX2_GAME_MB << 20;
            if (heap_given + apart < most + slack) most = heap_given + apart > slack ? (heap_given + apart - slack) & ~0x1FFFFFull : 0;
            {  // (<folder>/told: the game is told of this many MB, whatever there is. A try, for a game that sizes itself by it.)
                FILE *t = fopen(NX2_F("told"), "rb");
                if (t) {
                    unsigned long long mb = 0;
                    if (fscanf(t, "%llu", &mb) == 1 && mb >= 512) most = mb << 20;
                    fclose(t);
                }
            }
            nx2_heap_live(heap_at, heap_host, most);
            nx2_log("[nx2] the game is told of %u MB (%u MB of them address space it keeps apart from its heap): its heap, when it asks for one, is the kernel's own, after the host's part\n",
                    most >> 20, apart >> 20);
            live = 1;
#ifdef NX2_GAME_REMOTE_MB
            // The memory the graphics server keeps in a program's name, "remote memory": how much there may be of it is set for a
            // title by its own description (main.npdm, the number at 0x10), so a port has what the title it is started over has,
            // not what its game was made for. A game that needs more has a mapping of its graphics refused and its SDK ends it
            // (result 0x82f3, then nn::os::GetRemoteMemoryUsage and an abort; report, 2026-11-25). Nothing here can give more:
            // it is said, in the log and so in a crash report, for the one who reads it.
            {
                u64 there = 0, held = 0;
                if (R_SUCCEEDED(svcGetInfo(&there, 31, CUR_PROCESS_HANDLE, 0))) {
                    svcGetInfo(&held, 32, CUR_PROCESS_HANDLE, 0);
                    const u64 made_for = (u64)NX2_GAME_REMOTE_MB << 20;
                    nx2_log("[nx2] memory for graphics in this program's name (\"remote memory\"): the title this was started over has %u MB (%u in use); the game was made for %u MB%s" "\n",
                            there >> 20, held >> 20, made_for >> 20,
                            there < made_for ? ". THAT IS LESS: the game may end for lack of it. Started over a title that has as much, it would have it" : "");
                }
            }
#endif
#ifdef NX2_GAME_SYSRES_KB
            // The "system resource" of a program: memory the kernel keeps for the program's own bookkeeping, set by its description
            // too (main.npdm, the number at 0x14). A program that has none can't map memory into its address space as it runs
            // (MapPhysicalMemory is refused), which the SDK asks before it tries: a game that takes its memory so ended at its
            // start under a title that has none, where it went on under one that has (the same game, two reports, 2026-11-25).
            {
                u64 there = 0;
                if (R_SUCCEEDED(svcGetInfo(&there, 16, CUR_PROCESS_HANDLE, 0))) {
                    const u64 made_for = (u64)NX2_GAME_SYSRES_KB << 10;
                    if (!there && made_for) vam_auto = 1;
                    { extern u64 nx2_sysres_made_for; nx2_sysres_made_for = made_for; }  // (the pool is taken for it, below, unless <folder>/vam says otherwise)
                    nx2_log("[nx2] memory the system keeps for a program's own mappings (\"system resource\"): the title this was started over has %u MB; the game was made for %u MB%s\n",
                            there >> 20, made_for >> 20,
                            !there && made_for ? ". THERE IS NONE: a game that maps memory as it runs can't here, and may end at its start. Started over a title that has some, it would have it"
                            : "");
                }
            }
#endif
        }
#endif
    }
    if (early_block) block = early_block, heap = early_size;
    for (; !live && !block && heap >= (64u << 20); heap = heap > (1ull << 30) ? heap - (256u << 20) : heap >> 1) {
        nx2_log("[nx2] asking for %u MB for the game's heap\n", heap >> 20);
        nx2_log_flush();
        u8 *raw = malloc(heap + 0x200000);
        if (raw) {
            block = (u8 *)(((u64)raw + 0x1FFFFF) & ~0x1FFFFFull);
            break;
        }
    }
    if (!block) heap = 0;
    if (!live) {
        nx2_log("[nx2] %u MB at %p; clearing it\n", heap >> 20, (u64)block);
        nx2_log_flush();
        for (u64 at = 0; at < heap; at += 64u << 20) memset(block + at, 0, heap - at < (64u << 20) ? heap - at : 64u << 20);
        nx2_heap_set(block, heap);
    }
    nx2_log("[nx2] the game's heap is ready\n");
    nx2_log_flush();

    {
        FILE *f = fopen(NX2_F("debug"), "rb");
        if (f) fclose(f), nx2_debug = 1;
        nx2_log("[nx2] the long log is %s (<folder>/debug %s)\n", nx2_debug ? "on" : "off", nx2_debug ? "is there" : "isn't there");
        {  // (<folder>/onecore: the game's threads are left on the core the system gives them, not spread over three; svc.c)
            FILE *one = fopen(NX2_F("onecore"), "rb");
            nx2_onecore = one != NULL;
            if (one) fclose(one);
            FILE *ipc = fopen(NX2_F("ipclog"), "rb");
            nx2_ipclog = ipc != NULL;
            if (ipc) fclose(ipc);
            FILE *pin = fopen(NX2_F("pin"), "rb");
            nx2_pin = pin != NULL;
            if (pin) fclose(pin);
            FILE *nl = fopen(NX2_F("nvnlock"), "rb");
            nx2_nvnlock = nl != NULL;
            if (nl) fclose(nl);
            FILE *bo = fopen(NX2_F("bounce"), "rb");
            nx2_bounce = bo != NULL;
            if (bo) fclose(bo);
            FILE *sw = fopen(NX2_F("stackwatch"), "rb");
            nx2_stackwatch = sw != NULL;
            if (sw) fclose(sw);
            {  // (<folder>/hold: the game's threads of these numbers, as the log counts them, are made and never started: "54", or
               // "50-54". For a game the system closes with no word: closed all the same, the cause isn't in those threads; svc.c)
                FILE *hold = fopen(NX2_F("hold"), "rb");
                if (hold) {
                    if (fscanf(hold, "%d-%d", &nx2_hold_from, &nx2_hold_to) == 1) nx2_hold_to = nx2_hold_from;
                    fclose(hold);
                    nx2_log("[nx2] the game's threads %d to %d will be made and not started (<folder>/hold)" "\n", (s64)nx2_hold_from, (s64)nx2_hold_to);
                }
            }
            {  // (<folder>/vam: the game's address regions from a pool of the heap, for a title with no system resource; vam.c.
               // The file's number is the pool's size in MB; empty, 1024.)
               // (Taken on its own when the host has no system resource and the game was made for one, as under Pokopia;
               // a file with 0 in it turns that off.)
                FILE *vam = fopen(NX2_F("vam"), "rb");
                unsigned mb = 0;
                int asked = vam != NULL;
                if (vam) {
                    if (fscanf(vam, "%u", &mb) != 1) mb = 0;
                    else if (!mb) asked = -1;  // ("0": no pool)
                    fclose(vam);
                }
                if (asked > 0 || (!asked && vam_auto)) {
                    if (!asked) nx2_log("[vam] the host has no system resource: the pool is taken on its own (<folder>/vam with 0 in it: not)\n");
                    nx2_heap_pool(mb);
                }
            }
            FILE *noplt = fopen(NX2_F("noplt"), "rb");
            nx2_noplt = noplt != NULL;
            if (noplt) fclose(noplt);
            FILE *own = fopen(NX2_F("ownapplet"), "rb");
            nx2_ownapplet = own != NULL;
            if (own) fclose(own);
            // (<folder>/erpt: the console's error reports are looked at and copied to <folder>/reports/, and that is all this run
            // does; erpt.c)
            // (<folder>/control: the game's name, version and icon are asked of the console and written to the card, and that is
            // all this run does; control.c, tools/control.py)
            FILE *co = fopen(NX2_F("control"), "rb");
            if (co) {
                void nx2_control_look(void);
                fclose(co);
                nx2_control_look();
                nx2_log_end();
                heap_back();
                return 0;
            }
            FILE *er = fopen(NX2_F("erpt"), "rb");
            if (er) {
                void nx2_erpt_look(void);
                fclose(er);
                nx2_erpt_look();
                nx2_log_flush();
                nx2_log_end();
                heap_back();
                return 0;
            }
        }
        // <folder>/trace: every call between modules is logged (not just each one's first), from the first call of a function
        // whose name has the file's text in it (an empty file: from the start). For finding where a game ends without a word.
        extern char nx2_trace[96];
        extern int nx2_trace_on;
        {
            {  // (<folder>/ipcbytes: the bytes of what is asked of one service, and answered)
                extern char nx2_ipcbytes[12];
                FILE *b = fopen(NX2_F("ipcbytes"), "rb");
                if (b) {
                    size_t n = fread(nx2_ipcbytes, 1, sizeof nx2_ipcbytes - 1, b);
                    while (n && (nx2_ipcbytes[n - 1] == 10 || nx2_ipcbytes[n - 1] == 13 || nx2_ipcbytes[n - 1] == ' ')) n--;
                    nx2_ipcbytes[n] = 0;
                    fclose(b);
                }
            }
            {  // (<folder>/novamsdk: the game's SDK isn't told of a system resource, nor of the pool as its region; vam.c)
                FILE *f = fopen(NX2_F("novamsdk"), "rb");
                if (f) nx2_novamsdk = 1, fclose(f), nx2_log("[nx2] <folder>/novamsdk is there: the pool answers the game's calls, and the SDK keeps none of its own\n");
            }
            {  // (<folder>/nonvdrv: no stand-in for the graphics driver's service, nvdrv)
                FILE *nv = fopen(NX2_F("nonvdrv"), "rb");
                if (nv) {
                    nx2_nonvdrv = 1;
                    fclose(nv);
                    nx2_log("[nx2] <folder>/nonvdrv is there: nvdrv isn't stood in for\n");
                }
            }
            {  // (<folder>/refuse: services treated as refused, to see the port's stand-in for one at work)
                extern char nx2_refuse[200];
                FILE *refuse = fopen(NX2_F("refuse"), "rb");
                if (refuse) {
                    const size_t n = fread(nx2_refuse, 1, sizeof nx2_refuse - 1, refuse);
                    nx2_refuse[n] = 0;
                    fclose(refuse);
                    nx2_log("[nx2] <folder>/refuse is there: these services are treated as refused: %s\n", nx2_refuse);
                }
            }
            extern char nx2_slow[64];
            FILE *slow = fopen(NX2_F("slow"), "rb");
            if (slow) {
                size_t n = fread(nx2_slow, 1, sizeof nx2_slow - 1, slow);
                while (n && (nx2_slow[n - 1] == 10 || nx2_slow[n - 1] == 13 || nx2_slow[n - 1] == ' ')) n--;
                nx2_slow[n] = 0;
                fclose(slow);
            }
        }
        {  // (<folder>/tracein: "module from to [from to ...]", hex offsets: loader.c)
            extern char nx2_tracein_module[16];
            extern u64 nx2_tracein[16];
            extern int nx2_tracein_count;
            FILE *in = fopen(NX2_F("tracein"), "rb");
            if (in) {
                unsigned long long a, b;
                if (fscanf(in, "%15s", nx2_tracein_module) == 1)
                    while (nx2_tracein_count < 8 && fscanf(in, "%llx %llx", &a, &b) == 2)
                        nx2_tracein[2 * nx2_tracein_count] = a, nx2_tracein[2 * nx2_tracein_count + 1] = b, nx2_tracein_count++;
                fclose(in);
                nx2_log("[nx2] the calls made from %d stretches of %s are logged (<folder>/tracein)" "\n", (s64)nx2_tracein_count, nx2_tracein_module);
                {
                    extern u64 nx2_tracebt;
                    extern u32 nx2_tracebt_thread;
                    FILE *bt = fopen(NX2_F("tracebt"), "rb");
                    if (bt) {
                        unsigned long long v = 0, t = 0;
                        const int got = fscanf(bt, "%llx %llx", &v, &t);
                        if (got >= 1) nx2_tracebt = v;
                        if (got >= 2) nx2_tracebt_thread = (u32)t;
                        fclose(bt);
                    }
                }
                extern u64 nx2_traceat;
                FILE *at = fopen(NX2_F("traceat"), "rb");
                if (at) {
                    unsigned long long v = 0;
                    if (fscanf(at, "%llx", &v) == 1) nx2_traceat = v;
                    fclose(at);
                }
            }
        }
        f = fopen(NX2_F("trace"), "rb");
        if (f) {
            size_t n = fread(nx2_trace, 1, sizeof nx2_trace - 1, f);
            while (n && (nx2_trace[n - 1] == '\n' || nx2_trace[n - 1] == '\r' || nx2_trace[n - 1] == ' ')) n--;
            nx2_trace[n] = 0;
            fclose(f);
            nx2_trace_on = n ? 1 : 2;
            nx2_log("[nx2] every call between modules is logged, from %s%s\n", n ? "the first call of a function named like " : "the start", nx2_trace);
        }
    }
    nx2_rom_open();
    nx2_save_open();
#ifdef __SWITCH2__
    {  // the game's sound (audio2.c), and the picture's size (gl2.c): both decided before the game's imports are bound
#ifndef NX2_GAME_MB  // (a Switch 2 game's SDK opens aud:u itself: the port takes no session of it)
        static Service audio;
        const Result rc = auduOpenAudioOutManager(&audio);
        if (R_SUCCEEDED(rc) && !audio.object_id) nx2_audio_master = audio.session;
        nx2_log("[audio] the Switch 2's audio output manager (aud:u): %x, session %x%s\n", (u64)rc, (u64)audio.session,
                nx2_audio_master ? ": the game's sound goes to it" : ": not usable, the game's sound will be silent");
        static Service renderer;
        const Result rc2 = auduOpenAudioRendererManager(&renderer);
        if (R_SUCCEEDED(rc2) && !renderer.object_id) nx2_audren_master = renderer.session;
        nx2_log("[audio] the Switch 2's audio renderer manager (aud:u): %x, session %x%s\n", (u64)rc2, (u64)renderer.session,
                nx2_audren_master ? ": a game's audio renderer goes to it" : ": not usable");
#endif
        nx2_gl_config();
    }
#endif
    if (nx2_load()) {  // (the modules couldn't be put in memory: said in the log, and back to where this was started from)
#ifdef NX2_LOADER
        nx2_log("[nx2] the game's modules couldn't be put in memory\n");
#endif
        nx2_log_flush();
        nx2_crash("the game's modules could not be put in memory (the log's lines that begin \"[load]\" say which and why)");
        nx2_log_end();
        heap_back();
        return 1;
    }
    // The system gives a program one applet session (appletOE), and the host library opened it at start; the SDK asks for its own
    // (nn::oe::Initialize) and was refused: result 3e880, then Break (log, 2026-11-22). So the host's is closed first, as one
    // homebrew program does before the next is loaded into the same process.
    // Not on a Switch 2: there the SDK's request after this close ended in a fatal error of the system's applet manager (2128-0007,
    // the console had to be forced off; log, 2026-11-22). Left open there, the request is refused and the game stops cleanly.
#ifdef __SWITCH2__
    // (<folder>/ownapplet: a try of the Switch's way for a Switch 2 game's own SDK, which registers by a command of its own kind.)
    if (nx2_ownapplet) {
        nx2_log("[nx2] the host library's applet session is closed (<folder>/ownapplet): the SDK opens and registers its own\n");
        nx2_log_flush();
        appletExit();
        nx2_log("[nx2] closed\n");
        nx2_log_flush();
    }
#endif
#ifndef __SWITCH2__
    appletExit();
    nx2_log("[nx2] the host library's applet session is closed: the SDK opens its own\n");
#endif
    nx2_log_flush();
#ifdef __SWITCH2__
    // The controllers: the host library has its own use of the hid service from its start, and the SDK's was then refused
    // (CreateAppletResource: 82cca; log, 2026-11-22). The host has no use for its own, so it is given up before the game asks.
    nx2_log("[nx2] the host library's hid session: %s\n", serviceIsActive(hidGetServiceSession()) ? "open; closed now" : "not open");
    if (serviceIsActive(hidGetServiceSession())) hidExit();
    nx2_log_flush();
    {
        // A Switch 2 game that draws through its own SDK (<folder>/native): the host library made a window of its own at
        // its start, on the layer the system gives the program, and the SDK then opens that layer too. On a Switch the host's
        // side of all this is closed before the game starts (appletExit, above); here the window and the host's display session
        // are, so that the layer is the SDK's alone, as it is the game's on its console.
        FILE *native = fopen(NX2_F("native"), "rb");
        int is_native = native != NULL;
        if (native) fclose(native);
#ifdef NX2_GAME_MB
        is_native = 1;  // (built as a Switch 2 game, tools/build.py: the file is for a try with any other)
#endif
        if (is_native) {
            extern void __nx_win_exit(void);
            __nx_win_exit();
            nx2_log("[nx2] the host library's window and display session are closed: the game's SDK opens its own\n");
            nx2_log_flush();
            // (The memory for graphics that is in use before the game starts, some 19 MB of what the started-over title has, is not
            // the host library's to give back: closing its window, and its session with the graphics driver, left it as it was;
            // measured, 2026-11-25.)
        }
    }
    nx2_host_capture();
#endif
    nx2_start(envGetMainThreadHandle());
    nx2_log_end();
    heap_back();
    return 0;
}
