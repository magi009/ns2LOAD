// The ways this program's own side can end it, said before they do: the host library ends the program (a "break" to the kernel) when
// something of its own fails, and the C library's abort does the same. To the console that is the game closed "because of an
// error", and to the log it was nothing: only the game's own system calls are watched (svc.c). Each of these is reached through
// the linker's --wrap (tools/build.py), says who called and with what, and goes on to the real one.
#include <switch.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);
void nx2_log_settle(void);

static void said(const char *what, u64 a, u64 b, u64 c, void *from) {
    const char *m = "?";
    u64 o = (u64)from;
    nx2_where((u64)from, &m, &o);
    nx2_log("[host] the port's own side ends the program: %s (%x, %x, %x), called from %s+%x (address %p)\n", what, a, b, c, m, o, (u64)from);
    const u64 *frame = __builtin_frame_address(1);
    for (int k = 0; k < 10 && frame && !((u64)frame & 15) && (u64)frame > 0x10000; k++) {
        const char *fm = "?";
        u64 fo = frame[1];
        nx2_where(frame[1], &fm, &fo);
        nx2_log("[host]   called from %s+%x (address %p)\n", fm, fo, frame[1]);
        if (frame[0] <= (u64)frame) break;
        frame = (const u64 *)frame[0];
    }
    // (on the card before the program is gone: the kernel ends it at once, and these lines were left in memory, so a report of
    // such an end said nothing of it; report, 2026-11-25)
    nx2_log_settle();
}

Result __real_svcBreak(u32 reason, uintptr_t address, uintptr_t size);
Result __wrap_svcBreak(u32 reason, uintptr_t address, uintptr_t size) {
    said("svcBreak", reason, size == 4 && address ? *(const u32 *)address : address, size, __builtin_return_address(0));
    return __real_svcBreak(reason, address, size);
}

void __real_svcExitProcess(void);
void __wrap_svcExitProcess(void) {
    said("svcExitProcess", 0, 0, 0, __builtin_return_address(0));
    __real_svcExitProcess();
    for (;;) {}
}

void __real_svcReturnFromException(Result res);
void __wrap_svcReturnFromException(Result res) {
    said("svcReturnFromException", res, 0, 0, __builtin_return_address(0));
    __real_svcReturnFromException(res);
    for (;;) {}
}

void __real_abort(void);
void __wrap_abort(void) {
    said("abort", 0, 0, 0, __builtin_return_address(0));
    __real_abort();
    for (;;) {}
}

void __real_svcExitThread(void);
void __wrap_svcExitThread(void) {
    // (a thread of the port's own that ends: said, since none is meant to while the game runs)
    said("svcExitThread (a thread of the port's ends)", 0, 0, 0, __builtin_return_address(0));
    __real_svcExitThread();
    for (;;) {}
}
