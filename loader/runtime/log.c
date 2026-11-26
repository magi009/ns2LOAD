// The log: a ring any thread appends to without the C library (the game's threads have the console SDK's data where libnx keeps a
// thread's own, so nothing of newlib's that looks there may run on them), and a thread of the port's that writes it to the card.
#include <malloc.h>
#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nx2.h"

#define RING (1u << 18)
static char ring[RING];
static u64 ring_in;  // bytes ever put (atomic)
static u64 ring_out;
int nx2_debug;
int nx2_onecore;
int nx2_ipclog;
int nx2_pin;
int nx2_hold_from, nx2_hold_to;
int nx2_nvnlock;
int nx2_bounce;
int nx2_stackwatch;
int nx2_noplt;
int nx2_ownapplet;
static volatile int quit, written;
static volatile u64 flushed;   // bytes on the card (the writer's)
static volatile int waiting;   // the game has started: a line's caller waits until the card has it
static Thread writer;

static void put(char *line, int *n, char c) {
    if (*n < 510) line[(*n)++] = c;
}
static void put_num(char *line, int *n, u64 v, unsigned base, int min, int upper) {
    char t[24];
    int k = 0;
    do {
        const unsigned d = (unsigned)(v % base);
        t[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    while (k < min) t[k++] = '0';
    while (k) put(line, n, t[--k]);
}

void nx2_log(const char *fmt, ...) {
    char line[512];
    int n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            put(line, &n, *f);
            continue;
        }
        f++;
        int min = 0;
        while (*f >= '0' && *f <= '9') min = min * 10 + (*f++ - '0');
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (int k = 0; s && s[k] && k < 300; k++) put(line, &n, s[k]);
            break;
        }
        case 'c': put(line, &n, (char)va_arg(ap, int)); break;
        case 'x': put_num(line, &n, va_arg(ap, u64), 16, min, 0); break;
        case 'X': put_num(line, &n, va_arg(ap, u64), 16, min, 1); break;
        case 'p': put(line, &n, '0'), put(line, &n, 'x'), put_num(line, &n, va_arg(ap, u64), 16, 0, 0); break;
        case 'u': put_num(line, &n, va_arg(ap, u64), 10, min, 0); break;
        case 'd': {
            const s64 v = va_arg(ap, s64);
            if (v < 0) put(line, &n, '-');
            put_num(line, &n, (u64)(v < 0 ? -v : v), 10, min, 0);
            break;
        }
        default: put(line, &n, *f); break;
        }
    }
    va_end(ap);
    const u64 at = __atomic_fetch_add(&ring_in, (u64)n, __ATOMIC_SEQ_CST);
    for (int k = 0; k < n; k++) ring[(at + (u64)k) & (RING - 1)] = line[k];
    // A thread of the game's in a stretch where its SDK has said it isn't to be taken off its core (the count in the thread's own
    // area, at 0x100; tools/relink.py, NOT_ASKED) may make no system call: the kernel ends the program for one. The line is in the
    // ring, and the writer thread has it on the card in its own time.
    if (*(const volatile u16 *)((const u8 *)armGetTls() + 0x100)) return;
    svcOutputDebugString(line, (u64)n);
    // With the game running, the line is on the card before its caller goes on: a fault ends the program at once on a Switch 2, and
    // the lines of the 50 ms before it were lost with it. (Only system calls here: the writer thread does the writing.)
    if (waiting)
        for (int k = 0; k < 1000 && flushed < at + (u64)n; k++) svcSleepThread(500000);
}
void nx2_log_wait(int on) { waiting = on; }

static Mutex flush_lock;
static void flush(void) {
    mutexLock(&flush_lock);
    const u64 in = __atomic_load_n(&ring_in, __ATOMIC_SEQ_CST);
    if (in - ring_out > RING) ring_out = in - RING;
    FILE *f = in == ring_out ? NULL : fopen(NX2_F("log.txt"), "a");
    if (!f) {
        mutexUnlock(&flush_lock);
        return;
    }
    while (ring_out < in) {
        const u32 at = (u32)(ring_out & (RING - 1));
        const u64 len = in - ring_out < RING - at ? in - ring_out : RING - at;
        fwrite(ring + at, 1, len, f);
        ring_out += len;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    flushed = ring_out;
    mutexUnlock(&flush_lock);
}
void nx2_log_flush(void) { flush(); }
// The same for a thread that may be the game's, where the C library isn't to be used: it waits, half a second at most, until the
// writer thread has on the card all that was logged so far. (hostend.c called nx2_log_flush for this, from 0.1.8 on: on a thread of
// the game's the C library has no state of its own and the host library ends the program for it (345-0012), which came back here
// through the same wrapper, round and round. A game whose SDK ended it stayed on a black screen in place of closing; reports,
// 2026-11-26.)
void nx2_log_settle(void) {
    const u64 upto = __atomic_load_n(&ring_in, __ATOMIC_SEQ_CST);
    for (int k = 0; k < 500 && flushed < upto; k++) svcSleepThread(1000000);
}

// ---------------------------------------------------------------- a run that ended badly: <folder>/crash.txt
// One file, always of the same name and in the port's own folder, to send with a report: what ended the run in words, what the port
// and the console are, and the run's whole log. It is written:
//   - as the program ends, when the port knows why: the game's SDK ended it (an abort, svc.c), the game called something no module
//     has (loader.c), a fault the console handed the program (a Switch's);
//   - at the next start, when the run before left no word and the console says it ended in a fault: on a Switch 2 a fault ends
//     the program at once and nothing of it reaches the program. A file, "running", is in the folder from a run's start to its
//     end, with the time the run began. Found there at a start, the run before ended with no word: it was closed from the HOME
//     menu (the system ends a program so, and tells it nothing), the console was turned off, or it faulted. The console's own
//     reports tell which (erpt.c): one of a program's fault, written between that run's start and its log's last line, makes the
//     run's log the crash file, with the kind of fault. With none, the run was closed, and no crash file is written: a file
//     there after every game closed would be sent as a fault's.
#ifndef NX2_VERSION
#define NX2_VERSION "dev"
#endif
static const char *volatile crash_why;
static volatile int closing, crash_now;
void nx2_crash(const char *why) { crash_why = why; }
// (the same, and the file written before this returns: for a fault's handler, after which the system ends the program its own way)
void nx2_crash_now(const char *why) {
    crash_why = why, crash_now = 1;
    for (int k = 0; k < 300 && crash_now; k++) svcSleepThread(10000000);
}
void nx2_closing(void) { closing = 1; }

static void crash_file(const char *why, const char *detail, const char *log_path) {
    FILE *out = fopen(NX2_F("crash.txt"), "w");
    if (!out) return;
    const u32 hos = hosversionGet();
    const time_t now = time(NULL);
    char when[32] = "";
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", gmtime(&now));
    fprintf(out, "%s %s crash report\n", NX2_TOOL, NX2_VERSION);
    fprintf(out, "port:     %s\n", NX2_NAME);
#ifdef __SWITCH2__
    fprintf(out, "console:  Switch 2, system %u.%u.%u\n", (unsigned)HOSVER_MAJOR(hos), (unsigned)HOSVER_MINOR(hos), (unsigned)HOSVER_MICRO(hos));
#else
    fprintf(out, "console:  Switch, system %u.%u.%u\n", (unsigned)HOSVER_MAJOR(hos), (unsigned)HOSVER_MINOR(hos), (unsigned)HOSVER_MICRO(hos));
#endif
    fprintf(out, "written:  %s (UTC, the console's clock)\n", when);
    fprintf(out, "ended:    %s\n", why);
    if (detail && *detail) fprintf(out, "console's record: %s\n", detail);
    fprintf(out, "\nTo report this, send this file as it is. It has no personal data: the port's own log follows.\n");
    fprintf(out, "--------------------------------------------------------------------------------\n");
    FILE *in = fopen(log_path, "rb");
    if (in) {
        static char block[0x4000];
        size_t n;
        while ((n = fread(block, 1, sizeof block, in)) > 0) fwrite(block, 1, n, out);
        fclose(in);
    }
    fclose(out);
}

#ifdef __SWITCH2__
// How the console is set, said when it changes: docked or handheld, and the "performance configuration" the system has the
// program on (its processor's and its graphics processor's speeds). A game that is slow in one mode and not in the other, or
// slow after its loading is over (a game has the processor sped up and the graphics processor slowed while it loads, and asks
// for the usual again after), shows here. Asked from this thread, the port's own: every two seconds.
static void mode_said(void) {
    static int ready = -1, said;
    static u32 mode_then = ~0u, now_then = ~0u;
    if (ready < 0) ready = R_SUCCEEDED(apmInitialize());
    if (!ready || said > 200) return;
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    u32 now = 0, normal = 0, boost = 0;
    if (R_FAILED(apmGetPerformanceMode(&mode))) return;
    if (R_FAILED(appletGetCurrentPerformanceConfiguration(&now))) now = 0;
    if ((u32)mode == mode_then && now == now_then) return;
    mode_then = (u32)mode, now_then = now, said++;
    apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &normal);
    apmGetPerformanceConfiguration(ApmPerformanceMode_Boost, &boost);
    nx2_log("[nx2] the console is %s; the program's performance configuration is %x now (set for handheld: %x, for docked: %x)\n",
            mode == ApmPerformanceMode_Boost ? "docked" : mode == ApmPerformanceMode_Normal ? "handheld" : "in a mode this doesn't know", (u64)now, (u64)normal,
            (u64)boost);
}
#endif

static void writer_main(void *arg) {
    (void)arg;
    int closed_said = 0;
    for (;;) {
        svcSleepThread(waiting ? 500000 : 50000000);
        const int q = quit;
#ifdef __SWITCH2__
        // (every two seconds by the clock: the rounds of this loop are of no fixed length)
        static u64 looked;
        const u64 tick = armGetSystemTick();
        const int due = !q && (!looked || armTicksToNs(tick - looked) >= 2000000000ull);
        if (due) looked = tick, mode_said();
#endif
        flush();
        if (crash_now) {
            crash_file(crash_why, NULL, NX2_F("log.txt"));
            remove(NX2_F("running"));
            crash_now = 0;
        }
        if (closing && !closed_said) {  // (the user closes the game: the system may end the program at any moment from here)
            closed_said = 1;
            remove(NX2_F("running"));
        }
        if (q) {
            if (crash_why) crash_file(crash_why, NULL, NX2_F("log.txt"));
            remove(NX2_F("running"));
            written = 1;
            for (;;) svcSleepThread(1000000000);
        }
    }
}

void nx2_log_start(void) {
    mkdir("sdmc:/" NX2_TOOL, 0777);
#ifdef NX2_LOADER
    {
        char home[96];
        snprintf(home, sizeof home, "sdmc:%s", nx2_home());
        mkdir(home, 0777);
    }
#else
    mkdir("sdmc:" NX2_HOME, 0777);
#endif
    FILE *left = fopen(NX2_F("running"), "rb");
    const char *before = NULL;  // (what is said of the run before, in this run's log)
    if (left) {  // the run before ended with no word
        long long began = 0;
        if (fscanf(left, "%lld", &began) != 1) began = 0;
        fclose(left);
#ifdef __SWITCH2__
        char record[160] = "";
        struct stat st;
        const s64 ended = began && !stat(NX2_F("log.txt"), &st) && st.st_mtime > began ? (s64)st.st_mtime : 0;
        int nx2_erpt_last(char *to, size_t size, s64 from, s64 to_time);
        const int fault = began ? nx2_erpt_last(record, sizeof record, began, ended) : -1;
        if (fault > 0) {
            crash_file("the console closed the program for a fault", record, NX2_F("log.txt"));
            before = "[nx2] the run before this one ended in a fault, by the console's own record: <folder>/crash.txt has its log" "\n";
        } else if (fault < 0) {
            crash_file("the run ended with no word to the program, and the console's records couldn't be asked: it was closed from the HOME menu, "
                       "or the console was turned off, or it ended in a fault", NULL, NX2_F("log.txt"));
            before = "[nx2] the run before this one ended with no word; the console's records couldn't be asked: <folder>/crash.txt has its log" "\n";
        } else
            before = "[nx2] the run before this one was closed from outside (the HOME menu, or the console turned off): the console recorded no fault" "\n";
#else
        // (a Switch hands a fault to the program, which writes the file then: a run that left no word was closed from outside)
        (void)began;
        before = "[nx2] the run before this one was closed from outside (the HOME menu, or the console turned off)" "\n";
#endif
    }
    remove(NX2_F("log.prev.txt"));
    rename(NX2_F("log.txt"), NX2_F("log.prev.txt"));
    FILE *mark = fopen(NX2_F("running"), "wb");
    if (mark) {
        fprintf(mark, "%lld" "\n", (long long)time(NULL));
        fclose(mark);
    }
    if (R_SUCCEEDED(threadCreate(&writer, writer_main, NULL, NULL, 0x10000, 0x2C, -2))) threadStart(&writer);
    if (before) nx2_log("%s", before);
}

// The program goes back to where it was started from (main's return): its run ended well.
void nx2_log_end(void) {
    flush();
    remove(NX2_F("running"));
}

// (callable from a thread of the game's: only system calls here)
void nx2_exit(void) {
    quit = 1;
    for (int k = 0; k < 100 && !written; k++) svcSleepThread(20000000);
    svcExitProcess();
    for (;;) {
    }
}
