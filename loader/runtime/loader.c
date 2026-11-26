// The loader: what rtld, the console's first module, does for a game, done for modules that are part of this program.
//   1. each module's relocations (its dynamic section's RELA, JMPREL and RELR tables), a module address becoming a pointer through
//      nx2_host, since a module's data is no longer after its code;
//   2. symbols between modules (main's imports from sdk), first definition in load order;
//   3. rtld's record of each module (ModuleObject), linked in a list the SDK walks (nn::ro, the unwinder);
//   4. the start: the functions rtld calls by name, in its order, ending in nnMain.
// Written from the open reimplementation of rtld's behaviour (oss-rtld); the ModuleObject's layout is the part most likely to differ
// between SDK versions.
#include <switch.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "nx2.h"

typedef struct {
    s64 tag;
    u64 val;
} Dyn;
typedef struct {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
} Sym;
typedef struct {
    u64 offset, info;
    s64 addend;
} Rela;

// (libounce's headers have these names as macros since it loads shared libraries itself, 20261123: this file's own are kept)
#undef DT_PLTRELSZ
#undef DT_HASH
#undef DT_STRTAB
#undef DT_SYMTAB
#undef DT_RELA
#undef DT_RELASZ
#undef DT_STRSZ
#undef DT_INIT
#undef DT_FINI
#undef DT_REL
#undef DT_RELSZ
#undef DT_JMPREL
#undef DT_INIT_ARRAY
#undef DT_INIT_ARRAYSZ
#undef DT_RELRSZ
#undef DT_RELR
#undef DT_PLTGOT
#undef DT_RELACOUNT
enum { DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_STRSZ = 10, DT_INIT = 12, DT_FINI = 13,
       DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36, DT_PLTGOT = 3,
       DT_RELACOUNT = 0x6ffffff9 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

// rtld's record of a module (oss-rtld's ModuleObject), which the module reserves in its bss
typedef struct ModuleObject {
    struct ModuleObject *next, *prev;
    void *rela_or_rel_plt, *rela_or_rel;
    u8 *module_base;
    Dyn *dynamic;
    u64 is_rela;
    u64 rela_or_rel_plt_size;
    void (*dt_init)(void);
    void (*dt_fini)(void);
    u32 *hash_bucket, *hash_chain;
    char *dynstr;
    Sym *dynsym;
    u64 dynstr_size;
    void **got;
    u64 rela_dyn_size, rel_dyn_size, rel_count, rela_count, hash_nchain_value, hash_nbucket_value;
    void *got_stub_ptr;
} ModuleObject;
static struct {
    ModuleObject *next, *prev;
} module_list;  // (a list head, as rtld's g_pAutoLoadList points at)

// The same record as a Switch 2 game's SDK has it (SDK 20: the module's first word is 1, tools/game2.py). Read from that SDK's own
// nn::ro::LoadModule, which fills one for each module it loads (A game's sdk+1567ac on; 2026-11-25): the
// module's base and its dynamic section are 8 bytes further on than in the record above, with a pointer more before them; whether
// the relocations are RELA is a bit of a byte of flags near the end, not a word of its own; the fields from the PLT's size to the
// GOT's stub are where they were; and after them are the module's name's place, a size, the SDK's number and the flags. With the
// older record, that SDK took the dynamic section for the base when it went over the game's modules as one of the game's own was
// unloaded (nn::ro::UnloadModule), and the game ended with a data abort.
typedef struct {
    ModuleObject *next, *prev;                    // 0x00
    void *rela_plt, *rela, *rel;                  // 0x10
    u8 *module_base;                              // 0x28
    Dyn *dynamic;                                 // 0x30
    u64 rela_plt_size;                            // 0x38
    void (*dt_init)(void);                        // 0x40
    void (*dt_fini)(void);                        // 0x48
    void *hash_bucket;                            // 0x50  (SysV: the buckets; GNU: the bloom filter)
    u64 hash_chain;                               // 0x58  (SysV: the chains' address; GNU: the bloom filter's size, 32 bits)
    char *dynstr;                                 // 0x60
    Sym *dynsym;                                  // 0x68
    u64 dynstr_size;                              // 0x70
    void **got;                                   // 0x78
    u64 rela_dyn_size, rel_dyn_size, rel_count, rela_count;  // 0x80
    u64 hash_nchain;                              // 0xA0  (GNU: the bloom filter's shift)
    u64 hash_nbucket;                             // 0xA8  (GNU: the hash table's address)
    u64 soname;                                   // 0xB0
    u64 size;                                     // 0xB8
    u8 sdk, pad[2], flags;                        // 0xC0: 20; 0xC3: 1 bound as called, 2 RELA, 4 bound at load, 16 a GNU hash table
    u32 pad2;
    u64 stub;                                     // 0xC8
} ModuleObject20;

// ---------------------------------------------------------------- symbols
typedef struct {
    const char *name;
    void *addr;
} Entry;
static Entry *table;
static u32 table_mask;

static u32 hash_of(const char *s) {
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}
static const Sym *sym_at(const Nx2Module *m, u64 i) { return (const Sym *)nx2_host(m, m->dynsym) + i; }
static const char *name_of(const Nx2Module *m, const Sym *s) { return (const char *)nx2_host(m, m->dynstr) + s->name; }

static void build_table(void) {
    u64 total = 0;
    for (unsigned k = 0; k < nx2_module_count; k++) total += nx2_modules[k].nsyms;
    u32 size = 1024;
    while (size < total + total / 4) size <<= 1;  // (no larger than it must be: the memory is the game's; main.c)
    table = calloc(size, sizeof *table);
    table_mask = size - 1;
    u64 defined = 0;
    for (unsigned k = 0; k < nx2_module_count; k++) {
        const Nx2Module *m = &nx2_modules[k];
        for (u64 i = 1; i < m->nsyms; i++) {
            const Sym *s = sym_at(m, i);
            if (!s->shndx || (s->info >> 4) == 0) continue;  // undefined here, or local
            if ((s->other & 3) == 1 || (s->other & 3) == 2) continue;  // (kept to its own module: not for the others to find)
            const char *name = name_of(m, s);
            u32 h = hash_of(name) & table_mask;
            while (table[h].name && strcmp(table[h].name, name)) h = (h + 1) & table_mask;
            if (table[h].name) continue;  // an earlier module has it
            table[h].name = name;
            table[h].addr = nx2_host(m, s->value);
            defined++;
        }
    }
    nx2_log("[load] %u symbols defined in %u modules\n", defined, (u64)nx2_module_count);
}

void *nx2_symbol(const char *name) {
    for (u32 h = hash_of(name) & table_mask; table[h].name; h = (h + 1) & table_mask)
        if (!strcmp(table[h].name, name)) return table[h].addr;
    return NULL;
}

int nx2_where(u64 host, const char **module, u64 *offset) {
#ifdef __SWITCH2__
    host &= (1ull << 39) - 1;  // (a Switch 2 game signs its return addresses: the high bits are the signature)
#endif
    for (unsigned k = 0; k < nx2_module_count; k++) {
        const Nx2Module *m = &nx2_modules[k];
        if (host >= (u64)m->text && host < (u64)m->text + m->data_off) {
            *module = m->name, *offset = host - (u64)m->text;
            return 1;
        }
        if (host >= (u64)m->data && host < (u64)m->data + (m->end - m->data_off)) {
            *module = m->name, *offset = host - (u64)m->data + m->data_off;
            return 1;
        }
    }
    return 0;
}

// (the SDK's own way into the modules a game loads as it runs: nn::ro leaves its function for it where rtld would look, below)
static void *manual_lookup;
// a symbol no module has, called: said, and the game ends
static void unresolved(void) {
    nx2_log("[load] the game called a function no module defines (returning to %p)\n", (u64)__builtin_return_address(0));
    nx2_crash("the game called a function that none of its modules has (the log's lines \"no module defines\" name what was missing)");
    nx2_exit();
}

// ---------------------------------------------------------------- calls between modules, in the log
// Each PLT slot of the game's own modules (not the SDK's: its calls to itself are too many) goes through a small piece of code
// (svc_entry.s) that says here which slot it was. From nnMain on, a slot's first call is logged, with where it was called from: when
// the game ends without a word (a fault ends the program at once on a Switch 2), the last of these lines says how far it got.
#define PLT_MAX 8192
extern const u32 nx2_plt_pool[];
static struct {
    void *target;
    const char *name;
    u32 calls;
    u64 *slot;  // the module's own slot that was pointed here, to be given the function itself once its first call is said
} plt[PLT_MAX];
static u32 plt_count;
static int plt_on;
static int plt_budget = 4000;

// <folder>/peek: "module offset [offset ...]", all hex: the 8 bytes at that place of the module, and then, each taken as an
// address, the 8 bytes that many further on; said at every look of the watcher's, and at every call <folder>/tracein logs. For a
// game that ends reading through an object of its own: which link of the chain is the empty one, and when it is filled.
static int peek_count = -1;
static char peek_module[16];
static unsigned long long peek_at[6];
static void peek_values(u64 *v, int *bad) {
    u64 at = 0;
    *bad = -1;
    for (unsigned q = 0; q < nx2_module_count; q++) {  // (the module's place: its code, or its data past data_off)
        const Nx2Module *pm = &nx2_modules[q];
        if (strcmp(pm->name, peek_module)) continue;
        at = peek_at[0] < pm->data_off ? (u64)pm->text : (u64)pm->data - pm->data_off;
    }
    for (int k = 0; k < peek_count; k++) {
        v[k] = 0;
        if (!at || *bad >= 0) continue;
        at += peek_at[k];
        MemoryInfo info;
        u32 page;
        if (R_FAILED(svcQueryMemory(&info, &page, at)) || !(info.perm & Perm_R)) {
            *bad = k;
            continue;
        }
        v[k] = *(const volatile u64 *)at;
        at = v[k];
    }
}
static void nx2_peek_say(int look) {  // (the watcher's thread: the file is read here, with the C library)
    if (peek_count < 0) {
        peek_count = 0;
        FILE *f = fopen(NX2_F("peek"), "rb");
        if (f) {
            int n = 0;
            if (fscanf(f, "%15s", peek_module) == 1)
                while (n < 6 && fscanf(f, "%llx", &peek_at[n]) == 1) n++;
            fclose(f);
            peek_count = n;
        }
    }
    if (peek_count <= 0) return;
    u64 v[6] = {0};
    int bad;
    peek_values(v, &bad);
    nx2_log("[peek] look %d: %x %x %x %x (no memory at link %d)" "\n", (s64)look, v[0], v[1], v[2], v[3], (s64)bad);
}
static void nx2_peek_at_call(void) {  // (a thread of the game's: system calls and the log only)
    if (peek_count <= 0) return;
    u64 v[6] = {0};
    int bad;
    peek_values(v, &bad);
    nx2_log("[peek] at that call: %x %x %x %x (no memory at link %d)" "\n", v[0], v[1], v[2], v[3], (s64)bad);
}

char nx2_trace[96];
int nx2_trace_on;  // 0: off; 1: waiting for the function named; 2: every call is logged
char nx2_tracein_module[16];
u64 nx2_tracein[16];
int nx2_tracein_count;
u64 nx2_tracebt;
u32 nx2_tracebt_thread;
static int tracebt_budget = 600;
u64 nx2_traceat;  // (<folder>/traceat: a call from this offset of the traced module turns the log of every call on)
// <folder>/slow: the calls of functions whose name has the file's text in it each wait a little first, one thread at a time (with
// "!" first: the calls of every other function). For a game that ends at no fixed place when it runs at its own speed and doesn't
// when every call is logged: which calls have to be kept apart.
char nx2_slow[64];
static u32 slow_lock;

static int trace_budget = 30000;
int nx2_net_on;  // (netwatch.c: a computer is being told; every call between modules is put in the ring it sends)
void *nx2_plt_called(u32 i, u64 from) {
#ifdef __SWITCH2__  // (the ring is the Switch 2 port's: nvnwatch.c)
    if (nx2_net_on) {
        const u32 at = __atomic_fetch_add(&nx2_nvn_ring_in, 1, __ATOMIC_RELAXED) % NX2_NVN_RING;
        u64 tls;
        __asm__("mrs %0, tpidrro_el0" : "=r"(tls));
        nx2_nvn_ring[at] = (struct nx2_nvn_mark){.thread = tls, .a = from, .b = (u64)plt[i].target, .r = 0, .call = i, .what = 0xFFFF, .phase = 2};
    }
#endif
    // A function no module had when the program was put together: looked for again now that it is called. A game whose own code
    // is a module it loads as it runs has its first module call into that one, and on its console the call is bound at its first
    // use, among the modules loaded by then (rtld's lazy binding, which asks nn::ro for those it loaded). Bound to the end of the
    // program from the start, as this was, such a game ended at that call (report, 2026-11-25).
    if (plt[i].target == (void *)unresolved) {
        void *v = nx2_symbol(plt[i].name);
        if (!v && manual_lookup) v = (void *)((u64 (*)(const char *))manual_lookup)(plt[i].name);
        if (v) {
            nx2_log("[load] %s, which no module had at the start, is in a module loaded since: bound now" "\n", plt[i].name);
            plt[i].target = v;
        } else {
            nx2_log("[load] the game called %s, which no module defines" "\n", plt[i].name);
            nx2_crash("the game called a function that none of its modules has (the log's last \"[load]\" line names it)");
            nx2_exit();
        }
    }
    if (nx2_tracein_count && plt_on) {  // (<folder>/tracein: the calls made from chosen stretches of a module's code, every one)
        const char *module = "?";
        u64 off = from;
        if (nx2_where(from, &module, &off) && !strcmp(module, nx2_tracein_module))
            for (int k = 0; k < nx2_tracein_count; k++)
                if (off >= nx2_tracein[2 * k] && off < nx2_tracein[2 * k + 1]) {
                    u64 tls;
                    __asm__("mrs %0, tpidrro_el0" : "=r"(tls));
                    nx2_log("[in] %s, from %s+%x, on thread %x" "\n", plt[i].name, module, off - 4, (u64) * (const u32 *)(tls + 0x110));
                    if (!*(const volatile u16 *)(tls + 0x100)) nx2_peek_at_call();
                    if (nx2_traceat && off - 4 == nx2_traceat) nx2_trace_on = 2, nx2_log("[in] every call between modules is logged from here (<folder>/traceat)" "\n");
                    break;
                }
    }
    if (nx2_tracebt && nx2_trace_on == 2 && plt_on) {  // (<folder>/tracebt: who called, by the frames, at one call site, on one thread)
        const char *module = "?";
        u64 off = from, tp;
        __asm__("mrs %0, tpidrro_el0" : "=r"(tp));
        if (nx2_where(from, &module, &off) && off - 4 == nx2_tracebt && (!nx2_tracebt_thread || *(const u32 *)(tp + 0x110) == nx2_tracebt_thread) &&
            __atomic_fetch_sub(&tracebt_budget, 1, __ATOMIC_RELAXED) > 0) {
            const u64 *frame = __builtin_frame_address(0);
            char line[220];
            u32 len = 0;
            static const char hex[] = "0123456789abcdef";
            frame = frame ? (const u64 *)frame[0] : NULL;  // (the game's own frames begin one up)
            for (int k = 0; k < 9 && frame && !((u64)frame & 7) && (u64)frame > 0x10000; k++) {
                const char *m = "?";
                u64 o = frame[1] & ((1ull << 39) - 1);
                nx2_where(o, &m, &o);
                o -= 4;
                line[len++] = ' ';
                for (const char *c = m; *c && len < sizeof line - 20; c++) line[len++] = *c;
                line[len++] = '+';
                int digits = 1;
                while (digits < 16 && o >> (4 * digits)) digits++;
                for (int d = digits - 1; d >= 0; d--) line[len++] = hex[o >> (4 * d) & 15];
                const u64 *up = (const u64 *)frame[0];
                if (up <= frame || (u64)up - (u64)frame > 0x100000) break;
                frame = up;
            }
            line[len] = 0;
            nx2_log("[bt]%s" "\n", line);
        }
    }
    if (nx2_trace_on && plt_on) {
        if (nx2_trace_on == 1 && strstr(plt[i].name, nx2_trace)) nx2_trace_on = 2;
        if (nx2_trace_on == 2 && __atomic_fetch_sub(&trace_budget, 1, __ATOMIC_RELAXED) > 0) {
            const char *module = "?";
            u64 off = from;
            nx2_where(from, &module, &off);
            u64 tp;
            __asm__("mrs %0, tpidrro_el0" : "=r"(tp));
            nx2_log("[t] %s, from %s+%x, thread %x\n", plt[i].name, module, off - 4, (u64) * (const u32 *)(tp + 0x110));
        }
    }
    if (nx2_slow[0] && plt_on) {
        const int not = nx2_slow[0] == '!';
        if ((strstr(plt[i].name, nx2_slow + not) != NULL) != not) {
            while (__atomic_exchange_n(&slow_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(20000);
            svcSleepThread(100000);
            __atomic_store_n(&slow_lock, 0, __ATOMIC_RELEASE);
        }
    }
    if (plt_on && __atomic_fetch_add(&plt[i].calls, 1, __ATOMIC_RELAXED) < 1 && __atomic_fetch_sub(&plt_budget, 1, __ATOMIC_RELAXED) > 0) {
        const char *module = "?";
        u64 off = from;
        nx2_where(from, &module, &off);
        nx2_log("[call] %s, from %s+%x\n", plt[i].name, module, off - 4);
    }
    // Once its first call is said, a slot is given the function itself, and the game's later calls go straight to it, as on its
    // console. Left as they were, every call from the game's code to its SDK or its C library came through here for the whole
    // run: its registers kept, a stack taken under a lock all the threads share, a count they all write. A game in play makes
    // such calls by the hundred thousand for each picture, and one ran at a fraction of its speed where its films, which make few,
    // played as they should (seen on a console, 2026-11-25). A slot stays routed only while one of the folder's switches asks to
    // see every call.
    if (plt_on && plt[i].slot && plt[i].target != (void *)unresolved && !nx2_tracein_count && !nx2_tracebt && !nx2_trace_on && !nx2_slow[0]
#ifdef __SWITCH2__
        && !nx2_net_on
#endif
    )
        __atomic_store_n(plt[i].slot, (u64)plt[i].target, __ATOMIC_RELEASE);
    return plt[i].target;
}
static u64 late_slot(const char *name, u64 *slot) {  // (whatever the module and the switches: this one is needed to run, not to log)
    plt[plt_count].target = (void *)unresolved, plt[plt_count].name = name, plt[plt_count].slot = slot;
    return (u64)(nx2_plt_pool + 2 * plt_count++);
}
static u64 plt_slot(const Nx2Module *m, const char *name, void *target, u64 *slot) {
    if (plt_count >= PLT_MAX || nx2_noplt || !strcmp(m->name, "sdk")) return (u64)target;
    plt[plt_count].target = target, plt[plt_count].name = name, plt[plt_count].slot = slot;
    return (u64)(nx2_plt_pool + 2 * plt_count++);
}

// a function reached through a slot of its own, so that its first call is said in the log by name (nvn2.c's)
void *nx2_traced(const char *name, void *target) {
    if (plt_count >= PLT_MAX) return target;
    plt[plt_count].target = target, plt[plt_count].name = name;
    return (void *)(nx2_plt_pool + 2 * plt_count++);
}

// ---------------------------------------------------------------- relocations
static u32 bad_writes, missing;
static u32 overridden __attribute__((unused));

static u64 *slot(const Nx2Module *m, u64 off) {
    if (off < m->data_off || off + 8 > m->end) {  // in the code or the read-only data, which this program can't write
        if (bad_writes++ < 8) nx2_log("[load] %s: a relocation at %x is outside the data: left as it was\n", m->name, off);
        return NULL;
    }
    return (u64 *)nx2_host(m, off);
}

static void apply_rela(const Nx2Module *m, const Rela *r, u64 count) {
    for (u64 i = 0; i < count; i++, r++) {
        u64 *p = slot(m, r->offset);
        if (!p) continue;
        const u32 type = (u32)r->info, symi = (u32)(r->info >> 32);
        if (type == R_RELATIVE) {
            *p = (u64)nx2_host(m, (u64)r->addend);
        } else if (type == R_ABS64 || type == R_GLOB_DAT || type == R_JUMP_SLOT) {
            const Sym *s = sym_at(m, symi);
            const char *name = name_of(m, s);
            // Whose is a symbol that this module defines itself? Its console's loader looks every symbol of the usual visibility up
            // among all the modules, in their order (the game's own first, its SDK last), and the first to define it is everyone's:
            // so a game's own `operator new` serves its SDK too, and a thing defined once in a header and built into two modules
            // (a function's static object, a type's description) is one thing, not two. Given each module's own copy, as this did,
            // the video player's two halves (one in the SDK, one in a module beside it) each had their own "heap manager": one
            // was set up and the other asked, and a game ended as it started a film's sound, another as it opened a film
            // (reports, 2026-11-25). A local symbol, or one its module keeps to itself (hidden, protected), is its module's own.
            const int kept = s->shndx && ((s->info >> 4) == 0 || (s->other & 3));
            void *v = kept ? (void *)nx2_host(m, s->value) : nx2_symbol(name);
            if (!v && s->shndx) v = (void *)nx2_host(m, s->value);
#ifdef __SWITCH2__
            if (!s->shndx) {  // an import the port answers itself (gl2.c)
                void *mine = nx2_override(name);
                if (mine) v = mine, overridden++;
            } else if (!strncmp(name, "_ZN5movie", 9) || !strncmp(name, "_ZN2nn4diag6detail21GetProcessName", 34)) {
                // (and a few a module calls of its own: the video player's start, the process's name)
                void *mine = nx2_override(name);
                if (mine) v = mine, overridden++;
            }
#endif
            if (!v && s->shndx == 0) v = nx2_symbol(name);
            if (!v) {
                const int weak = (s->info >> 4) == 2;
                if (!weak && missing++ < 40) nx2_log("[load] %s: no module defines %s\n", m->name, name);
                // (a function that is called through a slot: the slot is one of the log's own, with the name, so that the call can
                // look for it again; loader's nx2_plt_called)
                *p = weak ? 0 : type != R_JUMP_SLOT ? 0 : plt_count < PLT_MAX ? late_slot(name, (u64 *)p) : (u64)(void *)unresolved;
                continue;
            }
            *p = type == R_JUMP_SLOT ? plt_slot(m, name, v, (u64 *)p) : (u64)v + (u64)r->addend;
        } else if (type >= 1028 && type <= 1031) {
            // (a module's thread-local storage: left alone here, as the console's own loader leaves it. The game's SDK fills it in
            // when the module's start hands the module to its C library; seen done on a console, 2026-11-25.)
            static u32 said;
            if (said++ < 4) nx2_log("[load] %s: thread-local storage of its own (relocation type %u): left to the game's SDK, as on its console\n", m->name, (u64)type);
        } else if (type) {
            if (bad_writes++ < 8) nx2_log("[load] %s: relocation type %u isn't handled\n", m->name, (u64)type);
        }
    }
}

// DT_RELR: packed relative relocations. An even word is an address to relocate; an odd one is a bitmap for the 63 words after the last.
static void apply_relr(const Nx2Module *m, const u64 *e, u64 count) {
    u64 where = 0;
    for (u64 i = 0; i < count; i++) {
        const u64 v = e[i];
        if (!(v & 1)) {
            u64 *p = slot(m, v);
            if (p) *p = (u64)nx2_host(m, *p);
            where = v + 8;
        } else {
            for (u64 bits = v >> 1, k = 0; bits; bits >>= 1, k++)
                if (bits & 1) {
                    u64 *p = slot(m, where + k * 8);
                    if (p) *p = (u64)nx2_host(m, *p);
                }
            where += 63 * 8;
        }
    }
}

typedef void (*Fn)(void);
static struct {
    Fn *array;
    u64 count;
    Fn init;
} inits[16];

static void relocate(unsigned k) {
    const Nx2Module *m = &nx2_modules[k];
    if (memcmp(nx2_host(m, m->mod0), "MOD0", 4)) {
        nx2_log("[load] %s: no MOD0 header where the tool said\n", m->name);
        return;
    }
    u64 tag[40] = {0}, relacount = 0;
    for (const Dyn *d = (const Dyn *)nx2_host(m, m->dynamic); d->tag; d++) {
        if (d->tag >= 0 && d->tag < 40) tag[d->tag] = d->val;
        if (d->tag == DT_RELACOUNT) relacount = d->val;
    }
    if (tag[DT_RELR]) apply_relr(m, (const u64 *)nx2_host(m, tag[DT_RELR]), tag[DT_RELRSZ] / 8);
    if (tag[DT_RELA]) apply_rela(m, (const Rela *)nx2_host(m, tag[DT_RELA]), tag[DT_RELASZ] / sizeof(Rela));
    if (tag[DT_JMPREL]) apply_rela(m, (const Rela *)nx2_host(m, tag[DT_JMPREL]), tag[DT_PLTRELSZ] / sizeof(Rela));
    if (tag[DT_REL]) nx2_log("[load] %s: has REL relocations (%x bytes), which aren't handled\n", m->name, tag[DT_RELSZ]);
    if (k < 16) {
        inits[k].init = tag[DT_INIT] ? (Fn)nx2_host(m, tag[DT_INIT]) : NULL;
        inits[k].array = tag[DT_INIT_ARRAY] ? (Fn *)nx2_host(m, tag[DT_INIT_ARRAY]) : NULL;
        inits[k].count = tag[DT_INIT_ARRAYSZ] / 8;
    }
    // rtld's record, in the module's bss, on the list
    if (*(const u32 *)nx2_host(m, 0) == 1 && m->module_object >= m->data_off && m->module_object + sizeof(ModuleObject20) <= m->end) {
        // (a Switch 2 game's module: the record as its SDK has it)
        ModuleObject20 *o = (ModuleObject20 *)nx2_host(m, m->module_object);
        memset(o, 0, sizeof *o);
        o->module_base = m->text;
        o->dynamic = (Dyn *)nx2_host(m, m->dynamic);
        // The relocations are said to be none for a module that was relinked: the SDK goes over them with the base and each one's
        // offset (to take back what a module being unloaded had given), and a relinked module's data is not at that distance from
        // its code. They were all seen to here, at load, and none is ever bound to a module loaded later.
        if (nx2_images[k].image) {
            o->rela = tag[DT_RELA] ? nx2_host(m, tag[DT_RELA]) : NULL;
            o->rela_dyn_size = tag[DT_RELASZ];
            o->rela_count = relacount;
            o->rela_plt = tag[DT_JMPREL] ? nx2_host(m, tag[DT_JMPREL]) : NULL;
            o->rela_plt_size = tag[DT_PLTRELSZ];
        }
        o->dt_init = inits[k < 16 ? k : 0].init;
        o->dt_fini = tag[DT_FINI] ? (Fn)nx2_host(m, tag[DT_FINI]) : NULL;
        o->sdk = 20, o->flags = 2 | 4;
        u64 gnu = 0;
        for (const Dyn *d = (const Dyn *)nx2_host(m, m->dynamic); d->tag; d++)
            if (d->tag == 0x6ffffef5) gnu = d->val;  // DT_GNU_HASH
        if (gnu) {
            const u32 *h = (const u32 *)nx2_host(m, gnu);
            o->hash_bucket = (void *)(h + 4), o->hash_chain = h[2], o->hash_nchain = h[3], o->hash_nbucket = (u64)h;
            o->flags |= 16;
        } else if (tag[DT_HASH]) {
            u32 *h = (u32 *)nx2_host(m, tag[DT_HASH]);
            o->hash_nbucket = h[0], o->hash_nchain = h[1];
            o->hash_bucket = h + 2, o->hash_chain = (u64)(h + 2 + h[0]);
        }
        o->dynstr = (char *)nx2_host(m, m->dynstr);
        o->dynstr_size = m->dynstr_size;
        o->dynsym = (Sym *)nx2_host(m, m->dynsym);
        o->got = tag[DT_PLTGOT] ? (void **)nx2_host(m, tag[DT_PLTGOT]) : NULL;
        o->prev = module_list.prev;
        o->next = (ModuleObject *)&module_list;
        module_list.prev->next = (ModuleObject *)o;
        module_list.prev = (ModuleObject *)o;
    } else if (m->module_object >= m->data_off && m->module_object + sizeof(ModuleObject) <= m->end) {
        ModuleObject *o = (ModuleObject *)nx2_host(m, m->module_object);
        memset(o, 0, sizeof *o);
        o->module_base = m->text;
        o->dynamic = (Dyn *)nx2_host(m, m->dynamic);
        o->is_rela = 1;
        o->rela_or_rel = tag[DT_RELA] ? nx2_host(m, tag[DT_RELA]) : NULL;
        o->rela_dyn_size = tag[DT_RELASZ];
        o->rela_count = relacount;
        o->rela_or_rel_plt = tag[DT_JMPREL] ? nx2_host(m, tag[DT_JMPREL]) : NULL;
        o->rela_or_rel_plt_size = tag[DT_PLTRELSZ];
        o->dt_init = inits[k < 16 ? k : 0].init;
        o->dt_fini = tag[DT_FINI] ? (Fn)nx2_host(m, tag[DT_FINI]) : NULL;
        if (tag[DT_HASH]) {
            u32 *h = (u32 *)nx2_host(m, tag[DT_HASH]);
            o->hash_nbucket_value = h[0], o->hash_nchain_value = h[1];
            o->hash_bucket = h + 2, o->hash_chain = h + 2 + h[0];
        }
        o->dynstr = (char *)nx2_host(m, m->dynstr);
        o->dynstr_size = m->dynstr_size;
        o->dynsym = (Sym *)nx2_host(m, m->dynsym);
        o->got = tag[DT_PLTGOT] ? (void **)nx2_host(m, tag[DT_PLTGOT]) : NULL;
        o->prev = module_list.prev;
        o->next = (ModuleObject *)&module_list;
        module_list.prev->next = o;
        module_list.prev = o;
    }
    nx2_log("[load] %s: code at %p, data at %p; RELA %x, JMPREL %x, RELR %x bytes; %u initializers\n", m->name, (u64)m->text, (u64)m->data,
            tag[DT_RELASZ], tag[DT_PLTRELSZ], tag[DT_RELRSZ], (u64)(k < 16 ? inits[k].count : 0));
}

int nx2_map_modules(void);  // whole.c: the modules kept whole, mapped by the console's loader service

int nx2_load(void) {
#ifdef __SWITCH2__
    nx2_ro_start();  // (memory for the modules a game loads as it runs, asked for now, on a thread of the host's; ro.c)
#endif
    if (nx2_whole && nx2_map_modules()) {
        nx2_log("[load] the modules couldn't be put in memory: the game isn't started\n");
        nx2_log_flush();
        return -1;
    }
    module_list.next = module_list.prev = (ModuleObject *)&module_list;
    build_table();
    for (unsigned k = 0; k < nx2_module_count; k++) relocate(k);
    // what rtld sets in the SDK when the SDK has it
    static const char *k_lists[] = {"_ZN2nn2ro6detail15g_pAutoLoadListE", "_ZN2nn2ro6detail17g_pManualLoadListE"};
    static struct {
        void *next, *prev;
    } manual;
    manual.next = manual.prev = &manual;
    void **p = nx2_symbol(k_lists[0]);
    if (p) *p = &module_list;
    p = nx2_symbol(k_lists[1]);
    if (p) *p = &manual;
    // The SDK's nn::ro, when a game starts it (to load modules of its own as it runs), puts its function for looking a name up in
    // those modules where rtld keeps one: at the place this pointer of the SDK's names, which rtld sets to a variable of its own.
    // Left null, the SDK wrote to address 0 in nn::ro::Initialize: a game ended there (data abort,
    // 2168-0002 in the console's report; sdk+156034, 2026-11-25).
    p = nx2_symbol("_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE");
    if (p) *p = &manual_lookup;
    // (and a flag of rtld's that the SDK reads through a pointer of the same kind as it puts a module in its list: left null, the
    // SDK read address 0 when the game's first module was in memory, sdk+156d28; the same game, the same day. The flag: nothing
    // said by the SDK about the names it binds.)
    static u8 ro_debug;
    p = nx2_symbol("_ZN2nn2ro6detail14g_pRoDebugFlagE");
    if (p) *p = &ro_debug;
    nx2_log("[load] done: %u relocations skipped, %u symbols missing\n", (u64)bad_writes, (u64)missing);
    return 0;
}

// rtld's call_initializator: every module's DT_INIT and its array, the last module first (the SDK before the game)
static void call_initializers(void) {
    for (int k = (int)(nx2_module_count < 16 ? nx2_module_count : 16) - 1; k >= 0; k--) {
        nx2_log("[init] %s: its init function, then %u initializers\n", nx2_modules[k].name, inits[k].count);
        // A module's init function (the SDK's crt's) sees to its own initializers: it hands them to the C library
        // (__nnmusl_init_dso) or, failing that, calls each. Called here as well they ran twice, which two games bore and one didn't:
        // an object of a game's put itself on a list a second time, and the walk to the list's end never
        // ended (main's initializer 11; log, 2026-11-22). So the list is this loader's only for a module with no init function.
        if (inits[k].init) {
            nx2_log("[init] %s: its init function sees to them\n", nx2_modules[k].name);
            inits[k].init();
            continue;
        }
        for (u64 i = 0; i < inits[k].count; i++)
            if (inits[k].array[i] && (u64)inits[k].array[i] != ~0ull) {
                nx2_log("[init] %s %u\n", nx2_modules[k].name, i);  // (one that doesn't return is the last named)
                inits[k].array[i]();
            }
    }
}

// A fault in the game (or in the port), written to the log before the system ends the program: where it was, what it touched, who
// called. libnx hands a fault here when the loader passes it on, on a stack of its own. Without this a crash leaves nothing in the
// log (A game on a Switch 2, ended somewhere after its first textures; 2026-11-23).
__attribute__((aligned(16))) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof __nx_exception_stack;
u32 __nx_exception_ignoredebug = 1;  // (told here even with a debugger attached, as the kernel may say of a Switch 2's loader)
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    const char *m1 = "?", *m2 = "?";
    u64 o1 = ctx->pc.x, o2 = ctx->lr.x;
    nx2_where(ctx->pc.x, &m1, &o1), nx2_where(ctx->lr.x, &m2, &o2);
    nx2_log("[fault] kind %x at %s+%x, called from %s+%x; the address touched %x (esr %x)\n", (u64)ctx->error_desc, m1, o1, m2, o2, ctx->far.x, (u64)ctx->esr);
    nx2_log("[fault] x0 %x, x1 %x, x2 %x, x3 %x, x4 %x, x8 %x, x9 %x, x19 %x, x20 %x, sp %x\n", ctx->cpu_gprs[0].x, ctx->cpu_gprs[1].x, ctx->cpu_gprs[2].x,
            ctx->cpu_gprs[3].x, ctx->cpu_gprs[4].x, ctx->cpu_gprs[8].x, ctx->cpu_gprs[9].x, ctx->cpu_gprs[19].x, ctx->cpu_gprs[20].x, ctx->sp.x);
    // (who called, a few frames up: the frame pointer's chain, as far as it stays near the stack pointer)
    u64 fp = ctx->fp.x;
    for (int k = 0; k < 8 && fp >= ctx->sp.x && fp - ctx->sp.x < 0x100000 && !(fp & 7); k++) {
        const u64 *frame = (const u64 *)fp;
        const char *m = "?";
        u64 o = frame[1];
        nx2_where(frame[1], &m, &o);
        nx2_log("[fault]   called from %s+%x\n", m, o);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    nx2_crash_now("a fault in the game's code, which the console handed to the program (the log's lines that begin \"[fault]\" say where)");
}

// Where the game's main thread is, said every few seconds for its first minutes: a game that shows nothing and makes no system call
// leaves no other trace (A game, a black screen and a log that ends at its initializers; 2026-11-22). The main
// thread is stopped for the look, as the kernel asks.
static Thread watcher;
static u32 watched;
static void watch(void *arg) {
    (void)arg;
#ifdef NX2_GAME_MB
    enum { LOOKS = 1200, RUNNING_ONLY = 1 };  // (a Switch 2 game's own: a look a second, and of the other threads only those in the game's code)
#else
    enum { LOOKS = 36, RUNNING_ONLY = 0 };
#endif
    for (int n = 0; n < LOOKS; n++) {
        svcSleepThread(RUNNING_ONLY ? (n < 11 ? 1000000000ull : 100000000ull) : n < 6 ? 1000000000ull : n < 12 ? 5000000000ull : 20000000000ull);  // (the first few looks a second apart: a start that stops at once)
#ifdef __SWITCH2__
        fflush(stderr), fsync(fileno(stderr));  // (the host's own messages, to the card: gl2.c)
        // (For a game the system closes with no word: <folder>/pause holds a look's number and, after it, which of the game's
        // threads are stopped there for good, "all" or a range "from-to" (0 the main thread). Closed all the same, the cause isn't
        // in those threads.)
        {
            static int pause_at = -1, from, to = 95;
            if (pause_at < 0) {
                pause_at = 0;
                FILE *f = fopen(NX2_F("pause"), "rb");
                if (f) {
                    if (fscanf(f, "%d %d-%d", &pause_at, &from, &to) < 1) pause_at = 0;
                    fclose(f);
                }
            }
            if (RUNNING_ONLY && n >= 10 && n % 5 == 0) nx2_log("[watch] look %d\n", (s64)n);
            nx2_peek_say(n);
            if (pause_at && n == pause_at) {
                extern u32 nx2_threads[96];
                u32 stopped = 0;
                if (from == 0) stopped += R_SUCCEEDED(svcSetThreadActivity(watched, ThreadActivity_Paused));
                for (int k = from > 1 ? from - 1 : 0; k < 96 && k < to; k++)
                    if (nx2_threads[k]) stopped += R_SUCCEEDED(svcSetThreadActivity(nx2_threads[k], ThreadActivity_Paused));
                nx2_log("[watch] look %d: the game's threads %d to %d are stopped for good (%u of them; <folder>/pause)\n", (s64)n, (s64)from, (s64)to, (u64)stopped);
                for (int k = 0; k < 60; k++) {
                    svcSleepThread(1000000000ull);
                    nx2_log("[watch] %d s after they were stopped: the program is still here\n", (s64)(k + 1));
                }
            }
        }
#endif
        ThreadContext c;
        if (R_FAILED(svcSetThreadActivity(watched, ThreadActivity_Paused))) continue;
        const Result rc = svcGetThreadContext3(&c, watched);
        svcSetThreadActivity(watched, ThreadActivity_Runnable);
        if (R_FAILED(rc)) continue;
        const char *m1 = "?", *m2 = "?";
        u64 o1 = c.pc.x, o2 = c.lr;
        nx2_where(c.pc.x, &m1, &o1), nx2_where(c.lr, &m2, &o2);
        if (!RUNNING_ONLY || n < 11 || nx2_where(c.pc.x, &m1, &o1))
        nx2_log("[watch] the main thread is at %s+%x, called from %s+%x; x0 %x, x1 %x, x8 %x, sp %x\n", m1, o1, m2, o2, c.cpu_gprs[0].x,
                c.cpu_gprs[1].x, c.cpu_gprs[8].x, c.sp);
        // (and, from the fourth look on, where each of the game's other threads is and who called it there, by its frames: a game
        // that shows nothing and ends nothing is waiting somewhere; a game, a Switch 2, 2026-11-23)
        extern u32 nx2_threads[96];
        for (u32 k = 0; n >= 3 && nx2_debug && k < 96; k++) {
            const u32 h = nx2_threads[k];
            if (!h || R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
            ThreadContext t;
            const Result got = svcGetThreadContext3(&t, h);
            char line[400];
            int at = 0;
            const char *in = "?";
            u64 in_off = 0;
            if (R_SUCCEEDED(got) && (!RUNNING_ONLY || nx2_where(t.pc.x, &in, &in_off))) {
                u64 pcs[7] = {t.pc.x, t.lr}, count = 2;
                const u64 *frame = (const u64 *)t.fp;
                while (count < 7 && frame && !((u64)frame & 15) && (u64)frame >= t.sp && (u64)frame - t.sp < 0x80000) {
                    pcs[count++] = frame[1];
                    const u64 *up = (const u64 *)frame[0];
                    if (up <= frame) break;
                    frame = up;
                }
                for (u64 i = 0; i < count && at < 340; i++) {
                    const char *m = "?";
                    u64 o = pcs[i] & ((1ull << 39) - 1);
                    nx2_where(pcs[i], &m, &o);
                    at += snprintf(line + at, sizeof line - (size_t)at, "%s%s+%llx", i ? ", " : "", m, (unsigned long long)o);
                }
            }
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            if (at) nx2_log("[watch]   thread %u: %s\n", (u64)(k + 1), line);
        }
    }
}

static void step(const char *name) {
    Fn f = (Fn)nx2_symbol(name);
    nx2_log("[start] %s%s\n", name, f ? "" : ": not in this game");
    if (f) f();
}

// The SDK's own start (nn::init::Start, in the SDKs that have it) calls this when the exception handler may be used: rtld's concern.
static void handler_ready(void) {}
// ... and this for the modules' initializers, just before nnMain: from here on the game's calls into the SDK are logged
static void initializers_then_watch(void) {
    nx2_log("[start] the modules' initializers\n");
    call_initializers();
    nx2_log("[start] nnMain next\n");
    plt_on = 1;
}
// (asked by the SDK's nn::ro for each name a module loaded as the game runs takes from the game's own modules; and by lazy binding)
static u64 lookup(const char *name) {
    void *v = nx2_symbol(name);
    static u32 asked, missing_said;
    const u32 n = __atomic_add_fetch(&asked, 1, __ATOMIC_RELAXED);
    if (!v && __atomic_add_fetch(&missing_said, 1, __ATOMIC_RELAXED) <= 40) nx2_log("[ro] a name asked for at run time that no module has: %s\n", name);
    else if (n <= 4 || (n & (n - 1)) == 0) nx2_log("[ro] name %u asked for at run time: %s, at %p\n", (u64)n, name, (u64)v);
    return (u64)v;
}

// rtld's __rtld_start_app
void nx2_start(u32 main_thread) {
    nx2_log_wait(1);
    // what a module asks rtld for by name at run time (nn::ro::LookupSymbol, lazy binding)
    u64 (**look)(const char *) = nx2_symbol("_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE");
    if (look) *look = lookup;
    // A newer SDK starts itself: rtld only calls nn::init::Start, which does the steps below in its own order and, between them,
    // nninitInitializeSdkModule (nn::oe, nn::fs and others made ready: without it the game's first nn::oe call faulted; log,
    // 2026-11-22).
    // (A later SDK's takes a third function, called after nnMain returns, for the modules' finalizers: a game's,
    // 11.4. Without this name the steps below were done by hand, and the game stayed in its initializers: log, 2026-11-22.)
    void (*sdk_start)(u64, u64, Fn, Fn, Fn) = (void (*)(u64, u64, Fn, Fn, Fn))nx2_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!sdk_start) sdk_start = (void (*)(u64, u64, Fn, Fn, Fn))nx2_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    watched = main_thread;
#ifdef __SWITCH2__
    {
        extern u32 nx2_main_handle;  // (gl2.c)
        nx2_main_handle = main_thread;
        // (Will a fault be told? The loader hands one to this program's start, and the host library's code there gives up, with
        // no word, unless the kernel answers whether a debugger is attached: said here, since a Switch 2 has never logged one.)
        u64 attached = 0;
        const Result asked = svcGetInfo(&attached, InfoType_DebuggerAttached, INVALID_HANDLE, 0);
        nx2_log("[fault] is a debugger attached: the kernel says %x (answer %x); a fault is %s\n", (u64)asked, attached,
                asked ? "NOT told to this port" : "told to this port");
        extern void nx2_fault_loader(void);  // (fault_raw.c)
        nx2_fault_loader();
        extern void nx2_pac_test(void);  // (pactest.c)
        nx2_pac_test();
        extern void nx2_net_watch(void);  // (netwatch.c)
        nx2_net_watch();
        FILE *test = fopen(NX2_F("faulttest"), "rb");  // (a fault made on purpose, to see whether one reaches the log at all)
        if (test) {
            fclose(test);
            nx2_log("[fault] <folder>/faulttest is there: a fault is made now\n");
            *(volatile u32 *)0x10 = 1;
        }
    }
#endif
    {  // (<folder>/nowatch: no looks at all, for a game the looking itself may end)
        FILE *off = fopen(NX2_F("nowatch"), "rb");
        if (off) fclose(off);
        else if (R_SUCCEEDED(threadCreate(&watcher, watch, NULL, NULL, 0x8000, 0x2B, -2))) threadStart(&watcher);
    }
    // The main thread's local region is the game's from here, and a game's starts as zeros. The host library's record of the thread
    // was still in its last 32 bytes, where a later SDK keeps a pointer of its C library's: a game
    // (SDK 16.2) followed the host's record as that pointer, in pthread_self, from its initializers (crash report, 2026-11-22).
    // (On a Switch 2 the host's half was saved by nx2_host_capture, and comes back for each call into the host.)
    // Not its first 32 bytes, which are the kernel's own and stay as the kernel has them: its count of the thread's pinning (0x100),
    // and, on a later system, the thread's own handle at 0x110, which the kernel writes and an SDK from 22 on reads to know which
    // thread it is on (23 places of one game's sdk read it, none writes it). Zeroed with the rest, as this was, the first lock
    // that SDK took looked held by "thread 0", itself, and the SDK ended the program in its own initializers (crash report,
    // 2026-11-25). An SDK before 22 keeps the handle in its own record of the thread, and never looked there.
    {
        u8 *local = armGetTls();
        nx2_log("[start] the main thread's local region, the kernel's part: %x %x %x %x" "\n",
                *(const u64 *)(local + 0x100), *(const u64 *)(local + 0x108), *(const u64 *)(local + 0x110), *(const u64 *)(local + 0x118));
        for (u32 k = 0x120; k < 0x200; k++) local[k] = 0;
    }
    if (sdk_start) {
        nx2_log("[start] nn::init::Start\n");
        nx2_svc_verbose(1);
        sdk_start(main_thread, 0, handler_ready, initializers_then_watch, handler_ready);
        nx2_log("[start] nn::init::Start returned\n");
        nx2_exit();
    }
    step("__nnDetailInitLibc0");
    void (*os_init)(u32, void *) = (void (*)(u32, void *))nx2_symbol("nnosInitialize");
    nx2_log("[start] nnosInitialize%s\n", os_init ? "" : ": not in this game");
    if (os_init) os_init(main_thread, NULL);
    step("__nnDetailInitLibc1");
    step("nndiagStartup");
    step("nninitInitializeSdkModule");
    step("nninitStartup");
    step("__nnDetailInitLibc2");
    initializers_then_watch();
    nx2_svc_verbose(1);
    step("nnMain");
    nx2_log("[start] nnMain returned\n");
    nx2_exit();
}
