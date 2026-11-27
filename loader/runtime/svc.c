// The game's system calls. Both consoles run Horizon, so most of them are the host's own and go through as they are: threads, locks,
// events, time, and the messages to the system's services. Answered here are the ones where this program stands between the game and
// the system: the heap (the host's is this program's own), the ends (exit, break), and the debug string. Every number is counted, and
// its first uses logged with where in the game they came from, which is the map of what a game needs.
// Runs on the game's threads: no C library, no thread-local storage (see log.c).
#include <switch.h>

#include "nx2.h"

static u8 *heap_base;
static u64 heap_max, heap_now;
void nx2_heap_set(void *base, u64 size) { heap_base = base, heap_max = size; }
// A Switch 2 game's (main.c): no block is set aside. The kernel's heap is the host's part alone, `host` bytes from `base`, and the
// game's SetHeapSize makes it larger by what the game asks: the game's heap is what comes after the host's part. `most` is what
// the game is told it has, and the most it may ask as a heap.
static u64 live_base, live_host;
void nx2_heap_live(u64 base, u64 host, u64 most) { live_base = base, live_host = host, heap_max = most; }
// (<folder>/vam: the pool for the game's address regions comes after the host's part, before the game's heap, and is the host's
// from then on: the game's SetHeapSize makes the kernel's heap the host's part, the pool and what it asks; vam.c)
void nx2_heap_pool(u64 mb) {
    if (!live_base) {
        nx2_log("[vam] <folder>/vam is there, but this game's heap is not the kernel's own: no pool\n");
        return;
    }
    live_host += nx2_vam_init(live_base, live_host, mb);
}

static u32 counts[0xC0];

static const char *name_of(u32 n) {
    switch (n) {
    case 0x01: return "SetHeapSize";
    case 0x02: return "SetMemoryPermission";
    case 0x03: return "SetMemoryAttribute";
    case 0x04: return "MapMemory";
    case 0x05: return "UnmapMemory";
    case 0x06: return "QueryMemory";
    case 0x07: return "ExitProcess";
    case 0x08: return "CreateThread";
    case 0x09: return "StartThread";
    case 0x0A: return "ExitThread";
    case 0x0B: return "SleepThread";
    case 0x0C: return "GetThreadPriority";
    case 0x0D: return "SetThreadPriority";
    case 0x0E: return "GetThreadCoreMask";
    case 0x0F: return "SetThreadCoreMask";
    case 0x10: return "GetCurrentProcessorNumber";
    case 0x11: return "SignalEvent";
    case 0x12: return "ClearEvent";
    case 0x13: return "MapSharedMemory";
    case 0x14: return "UnmapSharedMemory";
    case 0x15: return "CreateTransferMemory";
    case 0x16: return "CloseHandle";
    case 0x17: return "ResetSignal";
    case 0x18: return "WaitSynchronization";
    case 0x19: return "CancelSynchronization";
    case 0x1A: return "ArbitrateLock";
    case 0x1B: return "ArbitrateUnlock";
    case 0x1C: return "WaitProcessWideKeyAtomic";
    case 0x1D: return "SignalProcessWideKey";
    case 0x1E: return "GetSystemTick";
    case 0x1F: return "ConnectToNamedPort";
    case 0x21: return "SendSyncRequest";
    case 0x22: return "SendSyncRequestWithUserBuffer";
    case 0x24: return "GetProcessId";
    case 0x25: return "GetThreadId";
    case 0x26: return "Break";
    case 0x27: return "OutputDebugString";
    case 0x28: return "ReturnFromException";
    case 0x29: return "GetInfo";
    case 0x2C: return "MapPhysicalMemory";
    case 0x2D: return "UnmapPhysicalMemory";
    case 0x32: return "SetThreadActivity";
    case 0x33: return "GetThreadContext3";
    case 0x34: return "WaitForAddress";
    case 0x35: return "SignalToAddress";
    default: return "?";
    }
}

// where in the game the call is: the thunk that brought it ends in a branch back to the instruction after it (tools/relink.py)
static void site_of(const u64 *regs, const char **module, u64 *offset) {
    *module = "?", *offset = 0;
    const u32 *back = (const u32 *)(regs[19] + 4);  // x30 as nx2_svc_entry was called: in the thunk, before its ldp and its b
    if ((*back & 0xFC000000u) != 0x14000000u) return;
    s64 d = (s64)(*back & 0x03FFFFFFu);
    if (d & 0x02000000) d -= 0x04000000;
    nx2_where((u64)back + (u64)(d * 4) - 4, module, offset);
}

// ---------------------------------------------------------------- the messages to the system's services
// Which service a handle is a session of, as far as the game's own messages say: sm:'s GetService names one, and a session a service
// hands back is named after the one that gave it and the command that asked.
// The table is what the port's own answers go by (the file system's sessions get the game's files and saved data from the card), so a
// name must hold for as long as its handle does. It didn't: with 96 places filled in turn, the 97th session a game opened took the
// place of the oldest, and a game, which opens that many, had its file system session forgotten. Its
// next read of the game's files went to the system as it was, which closed the session, and the SDK ended the game (25a0b; log,
// 2026-11-23). Worse could have followed: a request about saved data sent on without its folder acts on the card's root. So: a
// place is freed when its handle is closed, there are many more of them, and the ones the port answers for are never given up.
#define SESSIONS 1024
static struct {
    u32 handle;
    char name[28];
} sessions[SESSIONS];
static u32 session_next, session_lock;
u32 nx2_threads[96];  // the game's threads' handles, in the order they were made (0: not made, or more than these)
static int verbose;        // every call logged (from nnMain on), while the budget lasts
static int budget = 1500;
void nx2_svc_verbose(int on) { verbose = on; }

static void sessions_take(void) {
    while (__atomic_exchange_n(&session_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
}
static void sessions_give(void) { __atomic_store_n(&session_lock, 0, __ATOMIC_RELEASE); }
static const char *session_name(u32 h) {
    for (u32 k = 0; h && k < SESSIONS; k++)
        if (sessions[k].handle == h) return sessions[k].name;
    return "?";
}
// (the sessions the port answers for: the file system's, the applet's, add-on content's, and sm: itself)
static int session_kept(const char *n) {
    return (n[0] == 'f' && n[1] == 's' && n[2] == 'p') || (n[0] == 'a' && n[1] == 'p' && n[2] == 'p') || (n[0] == 'a' && n[1] == 'o' && n[2] == 'c') ||
           (n[0] == 's' && n[1] == 'm' && n[2] == ':') || n[0] == '~';
}
static void session_forget(u32 h) {  // the handle is being closed: its number may be another session's next
    sessions_take();
    for (u32 k = 0; h && k < SESSIONS; k++)
        if (sessions[k].handle == h) sessions[k].handle = 0, sessions[k].name[0] = 0;
    sessions_give();
}
static void session_set(u32 h, const char *a, const char *b, u32 cmd) {
    char name[28];
    u32 n = 0;
    for (; a && *a && n < 20; a++) name[n++] = *a;
    for (; b && *b && n < 20; b++) name[n++] = *b;
    if (cmd != ~0u) {
        name[n++] = '.';
        if (cmd >= 100) name[n++] = (char)('0' + cmd / 100 % 10);
        if (cmd >= 10) name[n++] = (char)('0' + cmd / 10 % 10);
        name[n++] = (char)('0' + cmd % 10);
    }
    name[n] = 0;
    sessions_take();
    u32 at = SESSIONS;
    for (u32 k = 0; k < SESSIONS && at == SESSIONS; k++)
        if (sessions[k].handle == h) at = k;  // (the same handle again: its new name)
    for (u32 k = 0; k < SESSIONS && at == SESSIONS; k++)
        if (!sessions[k].handle) at = k;
    for (u32 tries = 0; tries < SESSIONS && at == SESSIONS; tries++) {  // full: the oldest that isn't one the port answers for
        const u32 k = session_next++ % SESSIONS;
        if (!session_kept(sessions[k].name)) at = k;
    }
    if (at < SESSIONS) {
        sessions[at].handle = h;
        for (u32 k = 0; k <= n; k++) sessions[at].name[k] = name[k];
    }
    sessions_give();
    if (at == SESSIONS) nx2_log("[ipc] no place left to remember the session %s (handle %x)\n", name, (u64)h);
}
static u8 *tls(void) {
    u8 *p;
    __asm__("mrs %0, tpidrro_el0" : "=r"(p));
    return p;
}
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
// the data of a message: after "SFCI" (a request: the command) or "SFCO" (an answer: the result). Null: none (a control message, a close)
static const u8 *payload(const u8 *m, const char *magic) {
    for (u32 o = 8; o < 0xF0; o += 4)
        if (m[o] == magic[0] && m[o + 1] == magic[1] && m[o + 2] == magic[2] && m[o + 3] == magic[3]) return m + o;
    return NULL;
}
// the first handle an answer carries (a new session), or 0
// (an answer in the system's newer, shorter form has no "SFCO": its result is the first word after the header and its handles)
static u32 short_result(const u8 *m) {
    u32 at = 8;
    if (rd32(m + 4) >> 31) {
        const u32 d = rd32(m + 8);
        at = 12 + ((d & 1) ? 8 : 0) + 4 * ((d >> 1 & 15) + (d >> 5 & 15));
    }
    return at + 4 <= 0x100 ? rd32(m + at) : 0;
}
// A service the console won't give this program. Which services a program may open is set by its own description (main.npdm), and
// a port is, to the console, the title it was started over: a game that opens a service that title may not is refused it ("not
// allowed", 0x1015), and its SDK ends the program on that (a game ended so at its start under one title and runs under another;
// reports, 2026-11-25). Nothing here can give the service: it is said, and kept for the report.
static char refused_service[9];
static u32 answer_handle(const u8 *m) {
    if (!(rd32(m + 4) >> 31)) return 0;
    const u32 d = rd32(m + 8), count = (d >> 1 & 15) + (d >> 5 & 15);
    return count ? rd32(m + 12 + ((d & 1) ? 8 : 0)) : 0;
}

static u32 held[16], held_count;  // the threads made and not to be started (<folder>/hold)
// (the game's own callers at a system call: its frames, from the frame pointer it came in with. The port's stack at a system call
// is its own, so the walk from here, below, never reaches them)
static void game_callers_said(const u64 *r) {
    const u64 *fp = (const u64 *)r[18];
    const char *module = "?";
    u64 off = r[19] & ((1ull << 39) - 1);
    nx2_where(off, &module, &off);
    nx2_log("[from]   the game's return address: %s+%x\n", module, off);
    for (int k = 0; k < 32 && fp && !((u64)fp & 7) && (u64)fp > 0x10000; k++) {
        const u64 from = fp[1] & ((1ull << 39) - 1);
        module = "?", off = from;
        nx2_where(from, &module, &off);
        nx2_log("[from]   the game's caller %u: %s+%x\n", (u64)k, module, off);
        if (fp[0] <= (u64)fp || fp[0] - (u64)fp > 0x400000) break;
        fp = (const u64 *)fp[0];
    }
}
#ifdef __SWITCH2__
// The applet session, on a Switch 2. The system gives a program one registration as an application (appletOE's OpenApplicationProxy),
// and the host library made it at start: the SDK's own was refused (3e880), and after the host's was closed it ended in a fatal error
// of the system's (2128-0007; logs, 2026-11-22). So the SDK shares the host's: its messages to the session it opened go to the
// host's instead, and the two steps that can't be repeated there are answered here: making the session a domain (it is one), and
// the registration (the host's proxy object is handed over). Everything else is the system's own answer.
static u32 applet_sdk, applet_host, applet_proxy;
#define APPLET_ROOT 1u  // the session's own object, as the SDK is told
static u32 applet_receive;  // the message being sent asks for the system's next message to the program
static u32 applet_state, applet_event;  // ICommonStateGetter's object, and the handle of its event (a message is waiting)
static u32 applet_asked;                // the object the request being sent asks command 0 of
static int applet_message = 1;          // the first message is still to be given (below)

// the answer to a request sent on: which object is the state getter, and which handle its event
static void applet_answered(const u8 *m, u32 handle) {
    if (applet_asked == applet_proxy && !applet_state && m[0x10] == 1) {  // GetCommonStateGetter
        applet_state = (u32)m[0x30] | (u32)m[0x31] << 8 | (u32)m[0x32] << 16 | (u32)m[0x33] << 24;
        nx2_log("[applet] the state getter is object %u\n", (u64)applet_state);
    } else if (applet_state && applet_asked == applet_state && handle && !applet_event) {  // GetEventHandle
        applet_event = handle;
    }
    applet_asked = 0;
}

static void applet_named(u32 sdk_handle) {
    if (nx2_ownapplet) return;  // (the SDK's session is its own)
    Service *p = appletGetServiceSession_Proxy();
    if (!serviceIsActive(p) || !p->object_id) return;
    applet_host = p->session, applet_proxy = p->object_id, applet_sdk = sdk_handle;
    nx2_log("[applet] the SDK's applet session (%x) goes to the host library's (%x, proxy object %u)\n", (u64)sdk_handle, (u64)applet_host,
            (u64)applet_proxy);
}
// (for a game whose start ends at the applet session with no word: a message's first bytes, and who sent it, a few frames up)
static void bytes_said(const char *what, const u8 *m) {
    static const char hex[] = "0123456789abcdef";
    char line[3 * 0x50 + 1];
    u32 at = 0;
    for (u32 k = 0; k < 0x50; k++) {
        line[at++] = hex[m[k] >> 4], line[at++] = hex[m[k] & 15];
        if ((k & 3) == 3) line[at++] = ' ';
    }
    line[at] = 0;
    nx2_log("[applet]   %s: %s\n", what, line);
}
static void callers_said(void) {
    const u64 *fp = __builtin_frame_address(0);
    const u64 here = (u64)fp;
    for (int k = 0; k < 24 && fp && !((u64)fp & 7) && (u64)fp >= here && (u64)fp - here < 0x200000; k++) {
        const char *module = "?";
        const u64 from = fp[1] & ((1ull << 39) - 1);  // (a Switch 2 game signs its return addresses: the high bits are the signature)
        u64 off = from;
        nx2_where(from, &module, &off);
        nx2_log("[from]   called from %s+%x\n", module, off);
        if (fp[0] <= (u64)fp) break;
        fp = (const u64 *)fp[0];
    }
}
// 1: answered here. 0: to be sent, to *h
static int applet_ipc(u8 *m, u32 *h, u32 type, u32 cmd) {
    *h = applet_host;
    if (type == 5 || type == 7) {  // a control message
        if (cmd != 0) return 0;
        for (u32 k = 0; k < 0x100; k++) m[k] = 0;  // ConvertCurrentObjectToDomain: done already; the object's id
        m[4] = 8;
        m[0x10] = 'S', m[0x11] = 'F', m[0x12] = 'C', m[0x13] = 'O';
        m[0x20] = APPLET_ROOT;
        return 1;
    }
    u64 buf, size;
    const u8 *d = nx2_ipc_data(m, &buf, &size);
    const u32 kind = d[0], object = (u32)d[4] | (u32)d[5] << 8 | (u32)d[6] << 16 | (u32)d[7] << 24;
    if (kind == 2 && (object == APPLET_ROOT || object == applet_proxy)) {  // closed by the SDK: the host's stay
        nx2_ipc_answer(m, 0, NULL, 0, 0);
        return 1;
    }
    applet_asked = kind == 1 && cmd == 0 ? object : 0;
    applet_receive = kind == 1 && object == applet_state && cmd == 1;  // (ReceiveMessage: the system's messages to the program)
    // The SDK's start waits for the system's message that the program has the screen (15: the focus changed), and the host library
    // took that message at its own start: the SDK waited for good, a black screen (log, 2026-11-22). It is given once more, here.
    if (kind == 1 && object == applet_state && cmd == 1 && applet_message) {  // ICommonStateGetter's ReceiveMessage
        static const u8 focus[4] = {15, 0, 0, 0};
        applet_message = 0;
        nx2_log("[applet] the SDK is given the message that the program has the screen\n");
        nx2_ipc_answer(m, 0, focus, 4, 0);
        return 1;
    }
    // (A Switch 2 game's SDK registers with command 10, the process's number and handle and eight bytes more: sent on to the
    // system it was never answered, and the console stayed black until restarted, 2026-11-24. It is the same registration.)
    if (kind == 1 && object == APPLET_ROOT && (cmd == 0 || cmd == 10)) {  // OpenApplicationProxy
        nx2_log("[applet] the SDK's registration as an application (command %u): given the host library's\n", (u64)cmd);
        nx2_ipc_answer(m, 0, NULL, 0, applet_proxy);
        return 1;
    }
    if (kind == 1 && object == APPLET_ROOT) {  // nothing else of the session's own object is sent to the system
        nx2_log("[applet] command %u of the applet session itself isn't one this port knows: refused\n", (u64)cmd);
        nx2_ipc_answer(m, 0x3E880, NULL, 0, 0);
        return 1;
    }
    return 0;
}
#endif

// SendSyncRequest: made here, so the answer is seen too
// ---------------------------------------------------------------- a service the started-over title may not open: stood in for
// Which services a program may open is its own description's to say, and a port is the title it was started over: a game that
// opens one that title may not is refused (0x1015) and its SDK ends the program, however little the game needs the service (one
// ended at its start for the service that streams the game to another console; report, 2026-11-25). Where the whole family of
// the service is closed to the title (an SDK asks for the fuller forms of a service first and settles for a plainer one: that is
// no refusal to act on), the game is given a session of the port's own, on which every request is answered "done" with nothing
// in the answer: no network, no figures read, no friends. What the game does with the service doesn't work; the game goes on.
// Not for the services a game can't run without (the graphics processor's, the display's, the controllers', its files): there
// the refusal stands, and the report says so.
static int starts(const char *n, const char *with);
char nx2_ipcbytes[12];  // (<folder>/ipcbytes: a service's name, or its beginning: the bytes of what is asked of it and answered are logged)
static u32 bytes_budget = 80;
#ifdef __SWITCH2__
static void bytes_said(const char *what, const u8 *m);
#else
static void bytes_said(const char *what, const u8 *m) { (void)what, (void)m; }  // (a Switch 2 port's: nothing is refused on a Switch)
#endif
static int bytes_wanted(const char *who) {
    if (!nx2_ipcbytes[0]) return 0;
    const char *n = who[0] == '~' ? who + 3 : who;
    return starts(n, nx2_ipcbytes) && __atomic_fetch_sub(&bytes_budget, 1, __ATOMIC_RELAXED) - 1 < 80;
}
char nx2_refuse[200];  // (<folder>/refuse: services to treat as refused, names with spaces between. A try, to see a stand-in at work.)
enum { STOOD = 24 };
static struct {
    char name[9];
    u8 calls[256];  // how often each command was answered (its low byte): the first few answers carry handles, see below
} stood[STOOD];
static u32 stood_count, stood_objects;
static void put32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static int starts(const char *n, const char *with) {
    for (; *with; n++, with++)
        if (*n != *with) return 0;
    return 1;
}
static int listed(const char *list, const char *name) {
    for (const char *c = list; *c;) {
        while (*c == ' ' || *c == '\n' || *c == '\r') c++;
        u32 k = 0;
        while (c[k] && c[k] != ' ' && c[k] != '\n' && c[k] != '\r') k++;
        u32 same = k > 0;
        for (u32 i = 0; i < k && same; i++) same = name[i] == c[i];
        if (same && !name[k]) return 1;
        c += k;
    }
    return 0;
}
// (the ones no stand-in is of use for: with nothing behind them there is no picture, no pad, no file)
static int never_stood(const char *n) {
    if (nx2_nvdrv_stands(n)) return 0;  // (the graphics driver's, of a Switch game: answered by nvdrv.c, nothing drawn)
    static const char *const core[] = {"nv", "vi:", "dispdrv", "gsv", "hid", "fsp", "applet", "aud", "ldr", "sm:", "lm", "apm", "set", "time", "pl:"};
    for (u32 k = 0; k < sizeof core / sizeof *core; k++)
        if (starts(n, core[k])) return 1;
    return 0;
}
// A handle that stands for a session of the port's, or for an event that never comes: a number of the port's own, which the
// kernel never sees (it has a bit set that no handle of the kernel's has). A program of this kind is let make no handle that
// would do: an event's has the kernel end it (2168-0008, "a system call the kernel didn't allow"), and of the stretches of
// memory handed over to nobody ("transfer memory") it may have a dozen at a time; both seen on a console, 2026-11-25. So what the
// game's SDK does with such a handle is answered here: a request on it (above), closing it, waiting on it (nx2_svc, below).
#define STOOD_HANDLE(h) (((u32)(h) & 0xF0000000u) == 0x40000000u)
static u32 stood_handles;
static u32 stood_handle(void) { return 0x40000000u | (1 + (__atomic_fetch_add(&stood_handles, 1, __ATOMIC_RELAXED) & 0x0FFFFFFFu)); }
u32 nx2_stood_handle(void) { return stood_handle(); }
static void stood_set(u32 h, int domain, u32 index, const char *service) {
    char name[16] = {'~', domain == 2 ? 's' : domain ? 'd' : 'n', (char)('A' + index)};  // (2: a session one of these handed back)
    u32 n = 3;
    for (; *service && n < 12; service++) name[n++] = *service;
    name[n] = 0;
    session_set(h, name, NULL, ~0u);
}
// Where "done, with nothing" is the wrong answer, by the service: what its own answer is when it has nothing to give.
//   bcat (data sent to a game from the network): a file or a folder of it asked for by name isn't there, said with the result
//   the system's own service gives on a console that has none (147a; seen, 2026-11-25). Told "done", a game went on to read the
//   file, got nothing, and ended in what it took for the file's contents.
static u32 stood_result(const char *service, int first, u32 in_bytes, int reads) {
    if (starts(service, "bcat") && !first && in_bytes >= 0x40 && !reads) return 0x147a;
    return 0;
}
// The services that hand back numbers and never an object, and what their numbers are when there is nothing behind them:
//   bsd (sockets): its answers are a call's own result and an error number. What needs no network is "done": registering the
//   program and its watch (0, 0: what the system's own service answers; seen, 2026-11-25), an event to wake a waiting thread with
//   (a number of its own, as a file's; written to, it takes the eight bytes; read, it has nothing yet), a wait for something to
//   arrive (nothing does: it comes back after a moment, as one that ran out of time), closing, and the settings of a socket.
//   Everything that would reach the network is "failed, the network is down" (-1 and 50), which a game with no network is made
//   to take. Told "done" with an object's number where the error number goes, a game took its sockets for broken and ended itself
//   (81b); told the network was down when it asked for the event, its own network code ended it (abort).
//   sfdnsres (names looked up): "no such host" (1).   nsd: nothing.
static int stood_plain(const char *service, u32 cmd, u32 *out0, u32 *out1) {
    *out0 = *out1 = 0;
    if (starts(service, "bsd")) {
        static u32 events;
        switch (cmd) {
        case 0: case 1: case 28: case 32: case 33: case 36:  // the program registered, its watch, its statistics
        case 19: case 20: case 21: case 22: case 23: case 26:  // a socket's settings, shut, closed
            break;
        case 31: *out0 = 100 + (__atomic_fetch_add(&events, 1, __ATOMIC_RELAXED) & 0xFFF); break;  // an event's own number
        case 24: *out0 = 8; break;                                                                // written to
        case 25: *out0 = 0xFFFFFFFFu, *out1 = 35; break;                                          // read: nothing there yet
        case 5: case 6: svcSleepThread(20000000); break;                                          // waited on: nothing arrived
        default: *out0 = 0xFFFFFFFFu, *out1 = 50;                                                 // the network is down
        }
        return 1;
    }
    if (starts(service, "sfdnsres")) {
        *out0 = 1;
        return 1;
    }
    return starts(service, "nsd");
}
// a request on such a session, answered here
static void stood_answer(u64 *r, u8 *m, u32 h, const char *who) {
    const u32 type = rd32(m) & 0xFFFF, index = (u32)(who[2] - 'A') % STOOD;
    const int domain = who[1] == 'd', control = type == 5 || type == 7;
    const u8 *q = payload(m, "SFCI");
    const u32 cmd = q ? rd32(q + 8) : ~0u;
    if (type == 2) {  // the session is closed: what the kernel says of one
        r[0] = 0xF601;
        return;
    }
    if (!control && nx2_nvdrv_stands(who + 3) && nx2_nvdrv_answer(r, m, domain)) return;  // (nvdrv: its own answers)
    // What an answer carries is the command's own to say, and here the command is unknown. A command that hands back a session,
    // or an event to wait on, is asked once or a few times; one asked at every picture hands back numbers. So the first four
    // answers to each command carry a handle of either kind (an SDK that expected none leaves them be), and the later ones none:
    // the kernel has only so many handles to give.
    u32 copy = 0, move = 0, word = 0, object = 0, result = 0, out0 = 0, out1 = 0;
    const int plain = !control && stood_plain(who + 3, cmd, &out0, &out1);
    if (control) {  // the session's own commands: become a domain; a copy of it; how large a buffer it takes
        if (cmd == 0) word = 0x100, stood_set(h, 1, index, who + 3);
        else if (cmd == 1 || cmd == 2 || cmd == 4) move = stood_handle();
        // (how large a buffer the session takes in a message's own bytes: the most there can be. With the 128 bytes one of the
        // system's services says, a game's SDK ended the program before sending a larger one: "pointer buffer too small", 11a0b)
        else if (cmd == 3) word = 0xFFFF;
    } else {
        u8 *calls = &stood[index].calls[cmd & 255];
        // (A request that brings a buffer to be filled is one that reads something: it is answered with nothing read, and no
        // more. Given an object's number where it looks for how much was read, a game took its own empty buffer for a file's
        // contents and ended in them; seen on a console, 2026-11-25.)
        const u32 w0 = rd32(m), w1 = rd32(m + 4);
        const int reads = (w0 >> 24 & 15) || (w0 >> 28 & 15) || (w1 >> 10 & 15);
        // (is this the session sm: gave, or the object it became, and how much the request brings: in a domain both are in
        // the eight bytes before "SFCI"'s sixteen)
        const u8 *dom = domain && q && q - m >= 16 ? q - 16 : NULL;
        const int first = dom ? rd32(dom + 4) == 0x100 : who[1] == 'n';
        const u32 in_bytes = dom ? (u32)dom[2] | (u32)dom[3] << 8 : q ? (w1 & 0x3FF) * 4 - (u32)(q - m - 8) : 0;
        result = stood_result(who + 3, first, in_bytes, reads);
        // (In a domain a new object is a number, not a handle, and it comes after whatever else the answer has: right after
        // the answer's header for a command that hands back an object and nothing else, as the ones that open an interface do
        // (seen in the system's own answers). So that is where one is put, every time: a number costs nothing, and a game asks
        // some of these commands once for each file it opens. A command that hands back a number instead reads it as that
        // number: one whose low byte is 0, so that a yes-or-no is "no".)
        if (domain && !reads && !result && !plain) object = 0x100 * (2 + (__atomic_fetch_add(&stood_objects, 1, __ATOMIC_RELAXED) & 0x3FFF));
        if (*calls < 4 && !reads && !result && !plain) {
            (*calls)++;
            copy = stood_handle();
            if (!domain) move = stood_handle();
        }
        static u32 said;
        if (said++ < 60 && result) nx2_log("[ipc] the port's stand-in for \"%s\": command %d answered that there is none (%x)\n", stood[index].name, (s64)(s32)cmd, (u64)result);
        else if (said <= 60) nx2_log("[ipc] the port's stand-in for \"%s\": command %d answered with nothing\n", stood[index].name, (s64)(s32)cmd);
    }
    if (move) stood_set(move, control ? domain : 2, index, who + 3);
    for (u32 k = 0; k < 0x100; k++) m[k] = 0;
    u32 o = 8;
    if (copy || move) {
        put32(m + 8, (copy ? 1u : 0) << 1 | (move ? 1u : 0) << 5);
        o = 12;
        if (copy) put32(m + o, copy), o += 4;
        if (move) put32(m + o, move), o += 4;
    }
    u8 *d = m + ((o + 15) & ~15u);
    if (domain && !control) put32(d, object ? 1 : 0), d += 16;  // (a domain's answer begins with how many objects it hands back)
    put32(d, 0x4F434653), put32(d + 8, result), put32(d + 16, control ? word : plain ? out0 : object);  // "SFCO", the result; then the one number, or the new object's
    if (plain) put32(d + 20, out1);
    put32(m + 4, ((copy || move) ? 0x80000000u : 0) | (0xF0 - o) / 4);
    r[0] = 0;
}
// Is any other form of the service given to the title? Asked of sm: itself with the game's own request, the name changed: a
// session that is given is closed again at once.
static int family_given(u8 *m, const u8 *asked, u32 sm, const char *name) {
    static const char *const ends[] = {"", ":u", ":user", ":a", ":s", ":r", ":m", ":su", ":sc", ":c", ":e", ":app", ":u0", ":u1", ":sys", ":t", ":d"};
    const u8 *q = payload(asked, "SFCI");
    const u32 at = q ? (u32)(q - asked) + 16 : 8;
    u32 stem = 0;
    while (name[stem] && name[stem] != ':') stem++;
    int given = 0;
    for (u32 e = 0; e < sizeof ends / sizeof *ends && !given; e++) {
        char other[9] = {0};
        u32 n = 0;
        for (; n < stem; n++) other[n] = name[n];
        const char *end = ends[e];
        for (; *end && n < 9; end++) other[n++] = *end;
        if (n > 8) continue;
        u32 same = 1;
        for (u32 k = 0; k < 9 && same; k++) same = other[k] == name[k];
        if (same) continue;
        for (u32 k = 0; k < 0x100; k++) m[k] = asked[k];
        for (u32 k = 0; k < 8; k++) m[at + k] = (u8)other[k];
        if (R_FAILED(svcSendSyncRequest(sm))) continue;
        const u32 got = answer_handle(m);
        if (got) {
            svcCloseHandle(got);
            given = 1;
            nx2_log("[ipc] sm: of that service's family, \"%s\" is given: the game's SDK is left to ask for it\n", other);
        }
    }
    return given;
}

static void send_sync(u64 *r, const char *module, u64 off, int log) {
    u8 *m = tls();
    u32 h = (u32)r[0];
    const u8 *q = payload(m, "SFCI");
    const u32 type = rd32(m) & 0xFFFF;
    u32 cmd = q ? rd32(q + 8) : ~0u;
    char text[9] = {0};
    for (u32 k = 0; q && k < 8; k++) text[k] = (q[16 + k] >= 0x20 && q[16 + k] < 0x7F) ? (char)q[16 + k] : 0;
    const char *who = session_name(h);
    if (who[0] == '~') {  // (a session of the port's own, in place of a service the started-over title may not open)
        const int said = bytes_wanted(who);
        if (said) nx2_log("[ipc] %s (handle %x), the port's own:\n", who, (u64)h), bytes_said("asked", m);
        stood_answer(r, m, h, who);
        if (said) bytes_said("answered here", m);
        return;
    }
    const int is_sm = who[0] == 's' && who[1] == 'm' && who[2] == ':';
    // A later SDK speaks to sm: in the system's newer, shorter form (TIPC: the command is in the message's type, 16 and up, and the
    // data follows the header at once, with no "SFCI"). Its request for a service is type 17, the name at the eighth byte. Not seen
    // as one, every service a game (SDK 16.2) opened was taken for sm: itself, the file system
    // service among them: its files and saved data were then the system's, of the program this was started over (log, 2026-11-22).
    const int tipc = is_sm && !q && type == 17;
#ifdef NX2_GAME_MB
    // (A Switch 2 SDK's messages in the shorter form to anything but sm:, its graphics driver's by the hundred for each picture:
    // none is this port's to answer or to learn a session from. Sent as they are.)
    if (!is_sm && !q && type >= 16 && !log) {
        r[0] = svcSendSyncRequest(h);
        return;
    }
#endif
    if (tipc) {
        cmd = 1;
        for (u32 k = 0; k < 8; k++) text[k] = (m[8 + k] >= 0x20 && m[8 + k] < 0x7F) ? (char)m[8 + k] : 0;
    }
#ifdef __SWITCH2__
    if (q && (type == 4 || type == 6) && who[0] == 'l' && who[1] == 'd' && who[2] == 'r' && who[3] == ':' && who[4] == 'r' && who[5] == 'o' && !who[6] &&
        nx2_ro_ipc(m, q, cmd, module, off)) {  // a module the game loads as it runs (ro.c)
        r[0] = nx2_ro_kernel;
        return;
    }
#endif
    if (who[0] == 'f' && who[1] == 's' && who[2] == 'p' && nx2_rom_ipc(m)) {  // the game's files, from the card (rom.c)
        r[0] = 0;
        return;
    }
    if (who[0] == 'a' && who[1] == 'o' && who[2] == 'c' && nx2_aoc_ipc(m)) {  // its add-on content, likewise
        r[0] = 0;
        return;
    }
    // Before it aborts, the SDK tells the system why the application ends (the applet session's SetTerminateResult, command 22). The
    // application, to the system, is the installed game this was started over: when the reason was one of the file system's "data is
    // corrupted" results (217202, from a RomFS image this project's own tool had made wrongly), the system marked that game as
    // corrupted, twice, two games (2026-11-22). The report is the port's to keep: it is answered here and never sent.
    if (who[0] == 'a' && who[1] == 'p' && who[2] == 'p' && who[3] == 'l' && who[4] == 'e' && who[5] == 't' && q && cmd == 22 &&
        (type == 4 || type == 6)) {
        u64 b, bs;
        const u8 *d = nx2_ipc_data(m, &b, &bs);
        const u32 reason = d + 36 <= m + 0x100 ? rd32(d + 32) : 0;
        nx2_log("[applet] the SDK reports why the game ends (result %x): kept from the system\n", (u64)reason);
        nx2_ipc_answer(m, 0, NULL, 0, 0);
        r[0] = 0;
        return;
    }
    // How long the game has been played, asked of the system by application id (the applet session's QueryApplicationPlayStatistics,
    // commands 110 and 111). The system answers only an application allowed to ask about that id, and the application is the one
    // this was started over: under one installed game a game was answered, under another it was refused
    // (3e880) and the SDK ended it (log, 2026-11-23). So the answer is the port's: none played, whatever it was started over.
    // (Only a request with a buffer to be filled: another of the session's objects has a command 110 that sets something.)
    if (who[0] == 'a' && who[1] == 'p' && who[2] == 'p' && who[3] == 'l' && who[4] == 'e' && who[5] == 't' && q && (cmd == 110 || cmd == 111) &&
        (type == 4 || type == 6) && (rd32(m) >> 24 & 15)) {
        static const u8 none[4];
        nx2_log("[applet] the game asks how long it has been played (command %u): told of none\n", (u64)cmd);
        nx2_ipc_answer(m, 0, none, 4, 0);
        r[0] = 0;
        return;
    }
    char save_paths[2][0x301];
    u32 save_handle = 0;
    const int save = who[0] == 'f' && who[1] == 's' && who[2] == 'p' ? nx2_save_before(m, save_paths, &save_handle) : 0;  // saved data, on the card (rom.c)
    if (save == 5) h = save_handle;  // (through the host's own session of the file system service)
    if (save == 3) {
        r[0] = 0;
        return;
    }
#ifdef __SWITCH2__
    // The SDK asks sm: for audout:u, which a Switch 2 refuses this program: it is given a new session of the audio output manager
    // main.c opened through aud:u instead (the same object). The request sent is "clone this session", to that manager; its answer
    // has the shape of sm:'s (a result, and the session's handle).
    // (The same for audren:u, the audio renderer's manager, which most games use: a game, a game.)
    const int wants_out = text[0] == 'a' && text[1] == 'u' && text[2] == 'd' && text[3] == 'o' && text[4] == 'u' && text[5] == 't' &&
                          text[6] == ':' && text[7] == 'u';
    const int wants_ren = text[0] == 'a' && text[1] == 'u' && text[2] == 'd' && text[3] == 'r' && text[4] == 'e' && text[5] == 'n' &&
                          text[6] == ':' && text[7] == 'u';
    if (is_sm && cmd == 1 && ((wants_out && nx2_audio_master) || (wants_ren && nx2_audren_master))) {
        for (u32 k = 0; k < 0x40; k++) m[k] = 0;
        m[0] = 5, m[4] = 8;  // a control message, 8 words of data
        m[0x10] = 'S', m[0x11] = 'F', m[0x12] = 'C', m[0x13] = 'I';
        m[0x18] = 2;  // CloneCurrentObject
        h = wants_out ? nx2_audio_master : nx2_audren_master;
    }
    static u32 applet_said;
    const int applet_watch = applet_sdk && h == applet_sdk && applet_said < 16;
    if (applet_watch) {
        applet_said++;
        nx2_log("[applet] message %u to the applet session: type %u, command %d\n", (u64)applet_said, (u64)type, (s64)(s32)cmd);
        bytes_said("asked", m);
        callers_said();
    }
    if (applet_sdk && h == applet_sdk && applet_ipc(m, &h, type, cmd)) {
        if (log) nx2_log("[ipc] %s type %u command %d: answered here, from %s+%x\n", who, (u64)type, (s64)(s32)cmd, module, off);
        if (applet_watch) bytes_said("answered here", m);
        r[0] = 0;
        return;
    }
#endif
    const int bytes = bytes_wanted(who);
    if (bytes) nx2_log("[ipc] %s (handle %x) type %u command %d:\n", who, (u64)h, (u64)type, (s64)(s32)cmd), bytes_said("asked", m);
    u8 asked[0x100];
    const int asks_service = is_sm && cmd == 1, forced = asks_service && nx2_refuse[0] && listed(nx2_refuse, text);
    for (u32 k = 0; asks_service && k < 0x100; k++) asked[k] = m[k];
    const u32 rc = forced ? 0 : svcSendSyncRequest(h);
    if (forced) {  // (<folder>/refuse: as if sm: had said "not allowed")
        for (u32 k = 0; k < 0x100; k++) m[k] = 0;
        if (q) put32(m + 4, 8), put32(m + 16, 0x4F434653), put32(m + 24, 0x1015);
        else put32(m + 4, 1), put32(m + 8, 0x1015);
    }
    if (bytes) bytes_said("the system answered", m);
#ifdef __SWITCH2__
    if (applet_watch) bytes_said("the system answered", m);
    // (the system's message that the program is to close, 4: the user closed the game. A run that ends after it is not one that
    // ended badly; log.c)
    if (applet_sdk && h == applet_sdk && applet_receive && !rc) {
        const u8 *o = payload(m, "SFCO");
        if (o && !rd32(o + 8) && rd32(o + 16) == 4) {
            nx2_log("[applet] the system tells the game to close\n");
            nx2_closing();
        }
    }
#endif
    if ((save == 1 || save == 5) && !rc) nx2_save_after(m, save);
#ifdef __SWITCH2__
    // (the answer to "clone this session", for an SDK that asked in the newer form: the result where that form has it, after the handle)
    if (tipc && h != (u32)r[0] && !rc) {
        const u8 *o = payload(m, "SFCO");
        const u32 res = o ? rd32(o + 8) : 0xE401;
        m[0x10] = (u8)res, m[0x11] = (u8)(res >> 8), m[0x12] = (u8)(res >> 16), m[0x13] = (u8)(res >> 24);
        m[4] = 1, m[5] = 0;  // one word of data (the handle's descriptor is kept)
    }
#endif
    const u8 *a = payload(m, "SFCO");
    u32 result = a ? rd32(a + 8) : 0, got = rc ? 0 : answer_handle(m);
    if (is_sm && cmd == 1 && !rc && !got) {
        const u32 said = q ? result : short_result(m);
        if (said == 0x1015) {
            // (said plainly and no louder: an SDK asks for the fuller forms of a service first and settles for the plain one, so
            // most of these are nothing; the one a game ends on is named in its report, below)
            nx2_log("[ipc] sm: the service \"%s\" isn't given to the title this was started over (result 1015, not allowed)%s\n", text,
                    forced ? ": made so by <folder>/refuse" : "");
            u32 mine = 0;
            u8 refusal[0x100];  // (sm:'s own answer, kept: the looks at the service's family write over it)
            for (u32 k = 0; k < 0x100; k++) refusal[k] = m[k];
            if (never_stood(text))
                nx2_log("[ipc]   and the port can't stand in for that one: a game that needs it needs another game to be started over\n");
            else if (stood_count < STOOD && (forced || !family_given(m, asked, h, text)) && (mine = stood_handle()) != 0) {
                const u32 index = stood_count++;
                for (u32 k = 0; k < 9; k++) stood[index].name[k] = text[k];
                stood_set(mine, 0, index, text);
                nx2_log("[ipc]   THE PORT STANDS IN FOR \"%s\": the game is given a session that answers every request with nothing. What the game "
                        "does with that service won't work; the game itself goes on\n", text);
            }
            if (mine) {  // the answer sm: would have given, in the form the game's SDK asked in: a handle, and "done"
                for (u32 k = 0; k < 0x100; k++) m[k] = 0;
                put32(m + 8, 1u << 5), put32(m + 12, mine);
                if (q) put32(m + 4, 0x80000000u | 8), put32(m + 16, 0x4F434653);
                else put32(m + 4, 0x80000000u | 1);
                result = 0, got = 0;  // (named above already: not to be named again as sm:'s own)
            } else {
                for (u32 k = 0; k < 9; k++) refused_service[k] = text[k];
                // (the answer as sm: gave it, byte for byte. Written anew here, in the form it was thought to have, a later SDK
                // read "done" and a handle of 0 in it, and ended on the handle; seen on a console, 2026-11-25.)
                for (u32 k = 0; k < 0x100; k++) m[k] = refusal[k];
            }
        } else if (said && tipc)
            nx2_log("[ipc] sm: the service \"%s\" isn't given: result %x\n", text, (u64)said);
    }
#ifdef __SWITCH2__
    if (applet_sdk && (u32)r[0] == applet_sdk && !rc && !result) applet_answered(m, got);
#endif
    if (got && !result) {
        if (is_sm && cmd == 1) {
            session_set(got, text, NULL, ~0u);
#ifdef __SWITCH2__
            if (text[0] == 'a' && text[1] == 'p' && text[2] == 'p' && text[3] == 'l' && text[4] == 'e' && text[5] == 't') applet_named(got);
#endif
        }
        else session_set(got, who, NULL, cmd);
    }
    // (a request that failed is always said, a few hundred times at most: it is what a game is most often ended for, and the log's
    // budget of ordinary lines is long spent by then)
    static int failures = 300;
    const int failed = (rc && !(rc == 0xF601 && type == 2)) || result;
    if (log || (failed && __atomic_fetch_sub(&failures, 1, __ATOMIC_RELAXED) > 0))
        nx2_log("[ipc] %s (handle %x) type %u command %d%s%s: kernel %x, result %x%s%s, from %s+%x%s", who, (u64)h, (u64)type, (s64)(s32)cmd,
                is_sm && cmd == 1 ? " " : "", is_sm && cmd == 1 ? text : "", (u64)rc, (u64)result, got ? ", a session: " : "",
                got ? session_name(got) : "", module, off, "\n");
    r[0] = rc;
}

void nx2_vsync_tick(void);  // gl2.c (a Switch 2's)
int nx2_svc(u64 *r, u32 n) {
#ifdef __SWITCH2__
    if (n == 0x0b) nx2_vsync_tick();  // (SleepThread)
#endif
#ifdef NX2_GAME_MB
    // (How much of its stack a thread has left when it makes a system call: the call costs it nothing on its console, and here the
    // registers are kept on its stack, 192 bytes below where it was. A thread of 4 KB may not have them. Each new least is said.)
    if (nx2_stackwatch) {
        static u64 least = ~0ull;
        MemoryInfo mi;
        u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)r))) {
            const u64 left = (u64)r - mi.addr, was = __atomic_load_n(&least, __ATOMIC_RELAXED);
            if (left < was && left < 0x1000) {
                __atomic_store_n(&least, left, __ATOMIC_RELAXED);
                const char *module;
                u64 off;
                site_of(r, &module, &off);
                nx2_log("[stack] system call %x from %s+%x: %u bytes of the thread's stack left below the port's 192 (the stack's memory: %x bytes at %x, kind %u)\n",
                        (u64)n, module, off, left, mi.size, mi.addr, (u64)mi.type);
            }
        }
    }
#endif
    const u32 seen = n < 0xC0 ? __atomic_fetch_add(&counts[n], 1, __ATOMIC_RELAXED) : 0;
#ifdef NX2_GAME_MB
    // (The first use of each system call, always said: one the kernel doesn't allow this process, which is an installed Switch
    // game's, ends the program at once and with no word, as a try of this port's own showed; 2026-11-24.)
    if (!seen) {
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        nx2_log("[svc] the first use of system call %x %s (%x, %x, %x, %x), from %s+%x\n", (u64)n, name_of(n), r[0], r[1], r[2], r[3], module, off);
    }
#endif
    // (from nnMain on, every call but the locks' and the clock's, which come by the thousand)
    const int quiet = n == 0x1A || n == 0x1B || n == 0x1C || n == 0x1D || n == 0x1E || n == 0x10 || n == 0x34 || n == 0x35;
    const int more = verbose && nx2_debug && !quiet && __atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED) > 0;
#ifdef __SWITCH2__
    {  // (who asks what memory an address inside the program is in, or what is at the program's start: a few times)
        static u32 asked;
        if (n == 0x06 && verbose && ((r[2] & 0xFFF) || r[2] == 0x8000000) && r[2] < 0x10000000 && __atomic_fetch_add(&asked, 1, __ATOMIC_RELAXED) < 6) {
            nx2_log("[svc] QueryMemory of %x, asked by:\n", r[2]);
            callers_said();
        }
    }
#endif
#ifdef __SWITCH2__
    if (n == 0x80) {  // (a call a Switch 2's SDK makes on memory its graphics driver uses: made here, so that what the kernel says is seen)
        typedef struct {
            u64 x0, x1;
        } Two;
        extern const u32 nx2_svc_stubs[];  // (svc_entry.s: "svc n; ret", 8 bytes each)
        const u64 a = r[0], b = r[1], c = r[2], d = r[3];
        const Two out = ((Two (*)(u64, u64, u64, u64, u64, u64, u64, u64))(nx2_svc_stubs + 2 * 0x80))(a, b, c, d, r[4], r[5], r[6], r[7]);
        r[0] = out.x0, r[1] = out.x1;
        static u32 said;
        if (out.x0 || __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 12)
            nx2_log("[svc] 80 (%x, %x, %x, %x): the kernel says %x (then %x)\n", a, b, c, d, out.x0, out.x1);
        return 1;
    }
#endif
    if (n == 0x21) {
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        send_sync(r, module, off, seen < 3 || more || nx2_ipclog);
        return 1;
    }
    if (n == 0x1F) {  // ConnectToNamedPort(out handle, name): sm: is where every service is asked for
        const char *name = (const char *)r[1];
        Handle h = 0;
        u32 rc = svcConnectToNamedPort(&h, name);
        nx2_log("[svc] 1f ConnectToNamedPort %s: %x, handle %x\n", name, (u64)rc, (u64)(rc ? 0 : h));
        // Refused on a Switch (10801: the process's limit of sessions reached; log, 2026-11-22), where the host library has one to
        // sm: already: the SDK is given that one.
        if (rc && name[0] == 's' && name[1] == 'm' && name[2] == ':' && !name[3] && serviceIsActive(smGetServiceSession())) {
            h = smGetServiceSession()->session, rc = 0;
            nx2_log("[svc] ... the SDK gets the host library's session to sm: (handle %x)\n", (u64)h);
        }
        if (!rc) session_set(h, name, NULL, ~0u);
        r[0] = rc, r[1] = h;
        return 1;
    }
    if (seen < 3 || more) {
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        nx2_log("[svc] %2x %s (%x, %x, %x, %x) from %s+%x%s\n", (u64)n, name_of(n), r[0], r[1], r[2], r[3], module, off,
                seen == 2 && !verbose ? " (its later uses aren't logged)" : "");
    }
    if (n == 0x16) session_forget((u32)r[0]);  // CloseHandle
    // (a handle of the port's own, standing for a service the started-over title may not open: closed, reset, waited on here)
    if ((n == 0x16 || n == 0x17 || n == 0x12) && STOOD_HANDLE(r[0])) {
        r[0] = 0;
        return 1;
    }
    if (n == 0x18 && r[1] && r[2] >= 1 && r[2] <= 64) {  // WaitSynchronization(out index, handles, count, timeout)
        const u32 *all = (const u32 *)r[1];
        u32 real[64], place[64], count = 0, mine = 0;
        for (u32 k = 0; k < (u32)r[2]; k++) {
            if (STOOD_HANDLE(all[k])) mine++;
            else real[count] = all[k], place[count++] = k;
        }
        if (mine) {  // the wait goes on for the kernel's own handles alone (with none, until its time is up): the port's never signal
            s32 index = -1;
            const u32 rc = svcWaitSynchronization(&index, real, (s32)count, r[3]);
            r[0] = rc, r[1] = !rc && index >= 0 && (u32)index < count ? place[index] : (u64)(s64)index;
            return 1;
        }
    }
    switch (n) {
#ifdef __SWITCH2__
    case 0x18:  // WaitSynchronization(out index, handles, count, timeout): the applet's event, with the message above still to give
        if (applet_message && applet_event && r[2] == 1 && *(const u32 *)r[1] == applet_event) {
            r[0] = 0, r[1] = 0;
            return 1;
        }
        return 0;
#endif
    case 0x29: {  // GetInfo(out, type, handle, sub)
        // Memory in use (7, and 22 without the system's part): the host library has taken nearly all of it for its own heap, so a
        // game that sizes its heap by what is free (total - used) got 2 MB and its first large allocation failed (A game's nninitStartup; log, 2026-11-22). What's free, as the game is told, is the block set aside for its heap.
        const u32 type = (u32)r[1];
        // (The "system resource" (16: its size, 17: how much of it is used), while vam.c's pool stands in for it: the game is
        // told of the one it was made for. Told of none, a game's SDK ended it at its start, the question asked last before
        // the end: Kirby and the Forgotten Land, Switch 2 Edition, over a title that has none; log, 2026-11-27.)
        // (and the region the SDK keeps its own address regions in, the "alias" region (2: where, 3: how large, 28: the
        // part of it more than the kernel's least), as the pool: vam.c, nx2_vam_map_physical)
        if ((type == 2 || type == 3 || type == 28) && nx2_vam_sdk() && (u32)r[2] == 0xFFFF8001u) {
            r[0] = 0, r[1] = type == 2 ? nx2_vam_pool_base() : type == 3 ? nx2_vam_pool_size() : 0;
            static int said;
            if (said++ < 3) nx2_log("[vam] the game asks where its alias region is (%u): told the pool's, %x\n", (u64)type, r[1]);
            return 1;
        }
        if ((type == 16 || type == 17) && nx2_vam_sdk()) {
            const u64 used = nx2_vam_mapped();
            r[0] = 0, r[1] = type == 16 ? nx2_sysres_made_for : (used < nx2_sysres_made_for ? used : nx2_sysres_made_for);
            static int said;
            if (said++ < 4)
                nx2_log("[vam] the game asks of its system resource (%u): told %u KB, as it was made for (the pool stands in for it)\n", (u64)type,
                        r[1] >> 10);
            return 1;
        }
#ifdef NX2_GAME_MB
        // (A Switch 2 game's own: a game ends itself at its start unless the memory in use is less than its "Program"
        // heap, 250 MB in its HeapDef file, which is the program alone on its console; main+5c9f58, log, 2026-11-24. Here the host's
        // share is in use as well. So the game is told of a console with its heap and a program of 128 MB, and nothing else.)
        if ((type == 6 || type == 7 || type == 21 || type == 22) && heap_max) {
            // (The program, as told: its modules and 2 MB, 128 MB at the most. A game checks it against the "Program" heap of
            // its HeapDef file: Super Mario Bros. Wonder's is 87040 KB (85 MB) and its modules 70 MB, and it ended itself when
            // told of 128 MB, and again of 86 MB (its modules and 16 MB); logs and its HeapDef, 2026-11-26.)
            static u64 program;
            if (!program) {
                u64 bytes = 2ull << 20;
                for (unsigned k = 0; k < nx2_module_count; k++) bytes += nx2_modules[k].end;
                bytes = (bytes + 0xFFFFF) & ~0xFFFFFull;
                program = bytes < (128ull << 20) ? bytes : (128ull << 20);
                nx2_log("[nx2] the game is told its program is %u MB of the memory in use (its modules, and 2 MB)\n", program >> 20);
            }
            // (Memory in use counts the heap the game has set, as on its console: the SDK's nn::os::QueryMemoryInfo takes the heap
            // from it to find the program's size, and with the program alone said to be in use, after the game had set a heap of
            // 7242 MB, that came out below nothing and Super Mario Bros. Wonder ended itself at its HeapDef's Program; log, 2026-11-26.)
            r[0] = 0, r[1] = program + (type == 6 || type == 21 ? heap_max : heap_now);
            return 1;
        }
#endif
        if ((type == 7 || type == 22) && heap_max) {
            u64 total = 0;
            if (R_SUCCEEDED(svcGetInfo(&total, type == 7 ? 6 : 21, (Handle)r[2], 0)) && total > heap_max) {
                r[0] = 0, r[1] = total - heap_max;
                return 1;
            }
        }
        return 0;
    }
    case 0x02:    // SetMemoryPermission(address, size, permission)
    case 0x03: {  // SetMemoryAttribute(address, size, mask, value)
        // A later SDK makes part of each module's data read-only once the modules are put together, and locks it so (the table
        // of the functions a module calls is in that part). The port gives a slot of that table the function itself after the
        // slot's first call is said in the log (loader.c's nx2_plt_called), which is a write there: left read-only, the write
        // ended the game at its first call (seen on a console, 2026-11-25). A module's own data is left writable, then, as it
        // is for a game of an earlier SDK; any other memory the game asks this of is the kernel's to answer.
        const char *module = "?";
        u64 off = r[0];
        const int lock = n == 0x02 ? (u32)r[2] == 1 : ((u32)r[2] & 0x10) && ((u32)r[3] & 0x10);
        if (!nx2_noplt && lock && nx2_where(r[0], &module, &off)) {
            static u32 said;
            if (said++ < 12) nx2_log("[svc] %s of %s+%x, %x bytes, read-only: left writable (the port writes its table of calls)\n", name_of(n), module, off, r[1]);
            r[0] = 0;
            return 1;
        }
        return 0;
    }
    case 0x04: {  // MapMemory(to, from, size): done here, so that a refusal is seen (a thread's stack is mapped so by the SDK)
        const u32 rc = svcMapMemory((void *)r[0], (void *)r[1], r[2]);
        static u32 maps;
        const u32 count = __atomic_add_fetch(&maps, 1, __ATOMIC_RELAXED);
        if (rc || (nx2_debug && r[2] > 0x100000)) nx2_log("[svc] MapMemory %u: %x bytes of %p to %p: %x%s\n", (u64)count, r[2], r[1], r[0], (u64)rc, rc ? " (REFUSED)" : "");
        r[0] = rc;
        return 1;
    }
    case 0x08: {  // CreateThread(out handle, entry, argument, stack top, priority, core): done here, so that what the kernel says is seen
        static u32 made;
        if (nx2_debug) {
            const char *em = "?";
            u64 eo = r[1] & ((1ull << 39) - 1);
            nx2_where(eo, &em, &eo);
            nx2_log("[svc] a thread is asked for: its function %s+%x, given %p, stack top %p" "\n", em, eo, r[2], r[3]);
        }
        Handle h = 0;
        const u32 rc = svcCreateThread(&h, (ThreadFunc)r[1], (void *)r[2], (void *)r[3], (int)r[4], (int)r[5]);
        const u32 count = __atomic_add_fetch(&made, 1, __ATOMIC_RELAXED);
        if (!rc && count <= sizeof nx2_threads / sizeof *nx2_threads) nx2_threads[count - 1] = h;  // (for the watcher, loader.c)
#ifdef __SWITCH2__
        // (A thread the game asks no core for, "the default one": the system here has that be core 0 alone for every such thread,
        // and a game whose threads were made to share three cores had them all take turns on one. A game makes 46
        // threads so; measured, 2026-11-24. Each is given the first three cores to move among, one of them to prefer, in turn.
        // (No file is looked for here to turn it off: this runs as the game, where the C library isn't to be used.)
        if (!rc && (s32)r[5] == -2) {
            static int asked, spread;
            if (!asked) {
                asked = 1;
                u64 allowed = 0;
                svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
                spread = (allowed & 7) == 7 && !nx2_onecore;
                nx2_log("[svc] the game's threads that ask for no core: %s (the system allows cores %x)\n", spread ? "given cores 0 to 2 to move among" : "left on the default core", allowed);
            }
            if (spread && !nx2_pin) svcSetThreadCoreMask(h, (s32)(count % 3), 7);
        }
        // (<folder>/pin: a thread never changes core. A try for a Switch 2 game's code, which signs its return addresses with a key
        // of the processor's: were the key not the same on every core for this process, a thread moved between a function's start
        // and its return would return to nowhere.)
        if (!rc && nx2_pin) {
            const s32 core = (s32)r[5] >= 0 ? (s32)r[5] : (s32)(count % 3);
            svcSetThreadCoreMask(h, core, 1u << core);
        }
#endif
#ifdef NX2_GAME_MB
        if (!rc) {
            s32 now = -9;
            u64 mask_now = 0;
            svcGetThreadCoreMask(&now, &mask_now, h);
            nx2_log("[svc] thread %u of the game's (handle %x): priority %d, core %d asked; it has core %d, cores %x\n", (u64)count, (u64)h, (s64)(s32)r[4],
                    (s64)(s32)r[5], (s64)now, mask_now);
        }
#endif
        if (rc || nx2_debug) nx2_log("[svc] thread %u of the game's: priority %d, core %d: %x%s\n", (u64)count, (s64)(s32)r[4], (s64)(s32)r[5], (u64)rc, rc ? " (REFUSED)" : "");
        if (!rc && nx2_hold_from && (int)count >= nx2_hold_from && (int)count <= nx2_hold_to && held_count < 16) held[held_count++] = h;
        r[0] = rc, r[1] = h;
        return 1;
    }
    case 0x09:  // StartThread(handle): not done for a thread that is held (<folder>/hold)
        for (u32 k = 0; k < held_count; k++)
            if (held[k] == (u32)r[0]) {
                nx2_log("[svc] thread %x is held: not started (<folder>/hold)" "\n", r[0]);
                r[0] = 0;
                return 1;
            }
        return 0;
    case 0x06: {  // QueryMemory(out information, address): the kernel's alias region said reserved, while the SDK isn't told of it
        if (!nx2_vam_sdk() || !r[0]) return 0;
        u32 page = 0;
        const u32 rc = svcQueryMemory((MemoryInfo *)r[0], &page, r[2]);
        if (!rc) nx2_vam_hide_alias((MemoryInfo *)r[0], r[2]);
        r[0] = rc, r[1] = page;
        return 1;
    }
    case 0x2C:
    case 0x2D: {  // MapPhysicalMemory, UnmapPhysicalMemory(address, size): done here, so that what the kernel says is seen
        if (n == 0x2D && nx2_vam_unmap_physical(r[0], r[1])) {  // (memory vam.c put there: cleared, kept)
            r[0] = 0;
            return 1;
        }
        u32 rc = n == 0x2C ? svcMapPhysicalMemory((void *)r[0], r[1]) : svcUnmapPhysicalMemory((void *)r[0], r[1]);
        if (n == 0x2C && rc) rc = nx2_vam_map_physical(r[0], r[1], rc);  // (refused, over a title with no system resource: vam.c)
        static u64 mapped;
        static u32 said;
        const u64 now = n == 0x2C ? __atomic_add_fetch(&mapped, rc ? 0 : r[1], __ATOMIC_RELAXED) : __atomic_sub_fetch(&mapped, rc ? 0 : r[1], __ATOMIC_RELAXED);
        const u32 k = __atomic_add_fetch(&said, 1, __ATOMIC_RELAXED);
        // (A refusal is said the first sixteen times and then at each power of two, not every time: a line's caller waits until
        // the card has the line, log.c, and a game that has taken all the memory there is and asks for more at every picture, sixty
        // times a second, was held up by its own refusals' lines: it stopped for a moment every few seconds; 2026-11-26.)
        static u32 refusals;
        const u32 nth = rc ? __atomic_add_fetch(&refusals, 1, __ATOMIC_RELAXED) : 0;
        if ((rc && (nth <= 16 || (nth & (nth - 1)) == 0)) || (!rc && (k <= 8 || (k & (k - 1)) == 0))) {
            u64 total = 0, used = 0;
            svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            nx2_log("[svc] %s of %x bytes at %x: %x%s (call %u; %u KB mapped so; the process has %u KB free)\n", n == 0x2C ? "MapPhysicalMemory" : "UnmapPhysicalMemory",
                    r[1], r[0], (u64)rc, rc ? " (REFUSED)" : "", (u64)k, now >> 10, (total - used) >> 10);
#ifdef __SWITCH2__
            // (What the graphics server holds in this process's name, "remote memory": the two figures the SDK itself asks for when
            // a mapping of the game's is refused for lack of it; nn::os::GetRemoteMemoryUsage, report of 2026-11-25.)
            u64 remote_a = 0, remote_b = 0;
            const u32 ra = svcGetInfo(&remote_a, 31, CUR_PROCESS_HANDLE, 0), rb = svcGetInfo(&remote_b, 32, CUR_PROCESS_HANDLE, 0);
            nx2_log("[svc]   remote memory: figure 31 is %u KB (%x), figure 32 is %u KB (%x)\n", remote_a >> 10, (u64)ra, remote_b >> 10, (u64)rb);
#endif
        }
        r[0] = rc;
        return 1;
    }
    case 0x45: {  // CreateEvent(out writable handle, out readable handle): done here, so that what the kernel says is seen
        Handle w = 0, rd = 0;
        const u32 rc = svcCreateEvent(&w, &rd);
        static u32 made;
        const u32 k = __atomic_add_fetch(&made, 1, __ATOMIC_RELAXED);
        if (rc || (k & (k - 1)) == 0) nx2_log("[svc] CreateEvent %u: %x%s (handles %x, %x)\n", (u64)k, (u64)rc, rc ? " (REFUSED)" : "", (u64)w, (u64)rd);
        r[0] = rc, r[1] = w, r[2] = rd;
        return 1;
    }
    case 0x15: {  // CreateTransferMemory(out handle, address, size, access): done here, so that what the kernel says is seen
        Handle h = 0;
        const u32 rc = svcCreateTransferMemory(&h, (void *)r[1], r[2], (u32)r[3]);
        static u32 said;
        if (rc || __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 16)
            nx2_log("[svc] CreateTransferMemory of %x bytes at %x, access %u: %x%s\n", r[2], r[1], r[3], (u64)rc, rc ? " (REFUSED)" : "");
        r[0] = rc, r[1] = h;
        return 1;
    }
    case 0x13: {  // MapSharedMemory(handle, address, size, access)
        const u32 rc = svcMapSharedMemory((Handle)r[0], (void *)r[1], r[2], (u32)r[3]);
        static u32 said;
        if (rc || __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 16)
            nx2_log("[svc] MapSharedMemory of %x bytes at %x, access %u: %x%s\n", r[2], r[1], r[3], (u64)rc, rc ? " (REFUSED)" : "");
        r[0] = rc;
        return 1;
    }
#ifdef __SWITCH2__
    case 0x0F: {  // SetThreadCoreMask(thread, core, mask)
        if (!nx2_pin) {
#ifdef NX2_GAME_MB
            // (a Switch 2 game's own: done here, so that what was asked and what the kernel said are seen, every time)
            const u32 h = (u32)r[0], rc = svcSetThreadCoreMask((Handle)r[0], (s32)r[1], (u32)r[2]);
            s32 now = -9;
            u64 mask_now = 0;
            svcGetThreadCoreMask(&now, &mask_now, (Handle)h);
            // (the first of them, and any that is refused: a game that moves its threads between cores as it runs filled the log's
            // end with these lines, in place of what it did last; report, 2026-11-25)
            static u32 moves;
            if (rc || __atomic_fetch_add(&moves, 1, __ATOMIC_RELAXED) < 80)
                nx2_log("[svc] SetThreadCoreMask of thread %x: core %d, cores %x asked: %x%s; it has core %d, cores %x now\n", (u64)h, (s64)(s32)r[1], r[2],
                        (u64)rc, rc ? " (REFUSED)" : "", (s64)now, mask_now);
            r[0] = rc;
            return 1;
#else
            return 0;
#endif
        }
        const u64 mask = r[2] & 0x3F;
        s32 core = (s32)r[1];
        if (!mask) return 0;  // (no change of the mask asked: the kernel's own)
        if (core < 0 || !(mask >> core & 1)) core = __builtin_ctzll(mask);
        r[0] = svcSetThreadCoreMask((Handle)r[0], core, 1u << core);
        return 1;
    }
#endif
    case 0x01: {  // SetHeapSize(out address, size): the host's heap is this program's; the game's is a block of it
        const u64 size = r[1];
        if (live_base) {  // (a Switch 2 game's: the kernel's own heap, made larger after the host's part)
            void *at = NULL;
            // (More than the game was told of: refused with the kernel's own word for it, "limit reached". The SDK turns that
            // and "out of memory" into a result its caller may get, and ends the program on any other: a game that grows its heap
            // until it is refused was ended by the 0xD201 this used to give, which is "out of handles"; reports, 2026-11-25.)
            const u32 rc = size > heap_max ? 0x10801 : svcSetHeapSize(&at, live_host + size);
            nx2_log("[svc] SetHeapSize %x (%u MB): the kernel's heap made the host's %u MB and that: %x%s\n", size, size >> 20, live_host >> 20, (u64)rc,
                    size > heap_max ? " (more than the game was told of)" : rc ? " (REFUSED)" : "");
            if (!rc) heap_now = size;
            r[0] = rc;
            r[1] = live_base + live_host;
            return 1;
        }
        if (!heap_base || size > heap_max) {
            nx2_log("[svc] SetHeapSize %x: more than the %x set aside\n", size, heap_max);
            r[0] = 0x10801;  // (the kernel's "limit reached": what its SDK takes as no more memory)
            return 1;
        }
        heap_now = size;
        r[0] = 0;
        r[1] = (u64)heap_base;
        return 1;
    }
    case 0x07:  // ExitProcess
        nx2_log("[svc] the game exits\n");
        for (u32 k = 0; k < 0xC0; k++)
            if (counts[k]) nx2_log("[svc] %2x %s: %u calls\n", (u64)k, name_of(k), (u64)counts[k]);
        nx2_exit();
    case 0x26: {  // Break(reason, address, size)
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        nx2_log("[svc] Break: reason %x, %x, %x, from %s+%x\n", r[0], r[1], r[2], module, off);
        game_callers_said(r);
        if (r[1] && r[2] == 4) nx2_log("[svc] Break: the result given is %x\n", (u64) * (const u32 *)r[1]);
        if (r[0] & 0x80000000u) {  // a notification only: the game goes on
            r[0] = 0;
            return 1;
        }
        {
            const u32 given = r[1] && r[2] == 4 ? *(const u32 *)r[1] : 0;
            static char why[420];
            if (given == 0x1015 && refused_service[0]) {
                // (put together by hand: this runs as the game's thread, where the C library isn't to be used)
                const char *parts[3] = {"the game's SDK ended the program: the console refuses the service \"", refused_service,
                                        "\" to the game the homebrew menu was started over, and this game needs it. Start the homebrew menu over "
                                        "another game (the game itself, if you can) and run the port again"};
                u32 n = 0;
                for (int k = 0; k < 3; k++)
                    for (const char *c = parts[k]; *c && n + 1 < sizeof why; c++) why[n++] = *c;
                why[n] = 0;
                nx2_crash(why);
            } else if ((given & 0x1FF) == 2 && (given >> 9) >= 4000 && (given >> 9) < 5000)
                nx2_crash("the game's SDK ended the program: it read the game's files and found them damaged (a file system result of the \"data is "
                          "corrupted\" kind). Install the port again, the game's files with it, and see that the card has room for them");
            else if (refused_service[0] && never_stood(refused_service)) {
                // (a service of the kind the port can't stand in for was refused earlier: said with the end, as what may be behind it)
                const char *parts[3] = {"the game's SDK ended the program itself (an abort). Before it, the console refused the service \"", refused_service,
                                        "\" to the game the homebrew menu was started over, and the port can't stand in for that one (graphics, display, "
                                        "controllers, files): if this game needs it, start the homebrew menu over another game, one of the same kind"};
                u32 n = 0;
                for (int k = 0; k < 3; k++)
                    for (const char *c = parts[k]; *c && n + 1 < sizeof why; c++) why[n++] = *c;
                why[n] = 0;
                nx2_crash(why);
            } else {
                // (a game that ends itself with memory left that it wasn't told of may have ended for lack of it: an allocator of
                // its own, sized by what it was told, that ran out; two reports, 2026-11-25. Said with the end, as a thing to try.)
                u64 total = 0, used = 0;
                svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0), svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
                if (heap_max && heap_max < (8992ull << 20) && total > used + (256ull << 20))
                    nx2_crash("the game's SDK ended the program itself (an abort: the lines before \"Break\" at the log's end say what it had just asked "
                              "for). The console still had memory the game wasn't told of: if the game ended for lack of memory, a larger figure "
                              "may let it run (Advanced options, memory: 8992)");
                else
                    nx2_crash("the game's SDK ended the program itself (an abort: the lines before \"Break\" at the log's end say what it had just asked for)");
            }
        }
        nx2_exit();
    }
    case 0x27: {  // OutputDebugString(text, size)
        char text[200];
        const u64 len = r[1] < sizeof text - 1 ? r[1] : sizeof text - 1;
        for (u64 k = 0; k < len; k++) text[k] = ((const char *)r[0])[k];
        text[len] = 0;
        nx2_log("[game] %s%s", text, len && text[len - 1] == '\n' ? "" : "\n");
        r[0] = 0;
        return 1;
    }
    default: return 0;
    }
}
