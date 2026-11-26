// A Switch 2 game's code signs its return addresses (pacibsp at a function's start, retab at its end): the processor mixes the
// address with a key of its own, and the return fails if the key isn't the one the address was signed with. The key is the
// process's, kept by the kernel across every switch of thread and core. This program runs in the process of an installed Switch
// game, whose own code signs nothing, so whether the kernel keeps the key for it is not known: measured here (<folder>/pactest),
// one address signed again and again, on each core and for some seconds, and every change of the signature counted.
#ifdef __SWITCH2__
#include <stdio.h>
#include <switch.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);

// x17 signed with key B and x16 (pacib1716, an instruction older processors take for nothing)
static u64 sign_b(u64 address, u64 with) {
    register u64 x17 __asm__("x17") = address;
    register u64 x16 __asm__("x16") = with;
    __asm__ volatile(".inst 0xd503215f" : "+r"(x17) : "r"(x16));
    return x17;
}
static u64 sign_a(u64 address, u64 with) {  // (pacia1716)
    register u64 x17 __asm__("x17") = address;
    register u64 x16 __asm__("x16") = with;
    __asm__ volatile(".inst 0xd503211f" : "+r"(x17) : "r"(x16));
    return x17;
}

// While the game runs: a thread of the port's signs the same address over and over, free to move among the cores, and says each
// change of the signature. The system's own programs sign too and share these cores with the game: were the key theirs after they
// had the core, the game's returns would fail the more it asks of them.
static Thread pac_thread;
static void pac_watch(void *arg) {
    (void)arg;
    const u64 address = 0x12345678ull, with = 0x7654321000ull;
    u64 last = sign_b(address, with), changes = 0, rounds = 0, lasta = sign_a(address, with);
    nx2_log("[pac] watching while the game runs: key B signs %x as %x\n", address, last);
    for (;;) {
        const u64 now = sign_b(address, with), nowa = sign_a(address, with);
        if (now != last || nowa != lasta) {
            if (changes++ < 12)
                nx2_log("[pac] the signature CHANGED while the game runs: key B %x to %x, key A %x to %x, on core %u, after %u rounds\n", last, now, lasta,
                        nowa, (u64)svcGetCurrentProcessorNumber(), rounds);
            last = now, lasta = nowa;
        }
        if (!(++rounds & 255)) svcSleepThread(20000);
        if (!(rounds & 0x1FFFFF)) {  // (and what memory the process has left, a few times a second)
            u64 total = 0, used = 0;
            svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            nx2_log("[pac] %u rounds, %u changes so far; the process has %u KB free\n", rounds, changes, (total - used) >> 10);
        }
    }
}

void nx2_pac_test(void) {
    FILE *f = fopen(NX2_F("pactest"), "rb");
    if (!f) return;
    fclose(f);
    if (R_SUCCEEDED(threadCreate(&pac_thread, pac_watch, NULL, NULL, 0x4000, 0x2C, -2))) {
        svcSetThreadCoreMask(pac_thread.handle, 0, 0x3F);
        threadStart(&pac_thread);
    }
    if (1) return;  // (the rest: the same measured with nothing else running, done once on 2026-11-24: no change)
    const u64 address = 0x12345678ull, with = 0x7654321000ull;
    u64 allowed = 0;
    svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    u64 first = 0;
    for (int core = 0; core < 8; core++) {
        if (!(allowed >> core & 1)) continue;
        if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) continue;
        svcSleepThread(1000000);
        const u64 b = sign_b(address, with), a = sign_a(address, with);
        if (!first) first = b;
        nx2_log("[pac] core %d (running on %u): key B signs %x as %x, key A as %x%s\n", (s64)core, (u64)svcGetCurrentProcessorNumber(), address, b, a,
                b == address ? " (not signed at all)" : b == first ? "" : " (NOT as on the first core)");
    }
    // some seconds on each of three cores, yielding now and then so that other threads and processes have the core in between
    for (int core = 0; core < 3; core++) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
        svcSleepThread(1000000);
        u64 last = sign_b(address, with), changes = 0, rounds = 0;
        const u64 until = armGetSystemTick() + armGetSystemTickFreq() * 3;
        while (armGetSystemTick() < until) {
            for (int k = 0; k < 20000; k++) {
                const u64 now = sign_b(address, with);
                if (now != last) {
                    if (changes++ < 6) nx2_log("[pac] core %d: the signature changed, %x to %x, after %u rounds\n", (s64)core, last, now, rounds);
                    last = now;
                }
            }
            rounds++;
            if (!(rounds & 63)) svcSleepThread(100000);
        }
        nx2_log("[pac] core %d: %u changes of the signature in three seconds (%u rounds of 20000)\n", (s64)core, changes, rounds);
    }
    // and with the thread free to move among all the cores
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, (u32)(allowed & 0x3F));
    {
        u64 last = sign_b(address, with), changes = 0, rounds = 0;
        u32 on = svcGetCurrentProcessorNumber(), moves = 0;
        const u64 until = armGetSystemTick() + armGetSystemTickFreq() * 4;
        while (armGetSystemTick() < until) {
            const u64 now = sign_b(address, with);
            const u32 core = svcGetCurrentProcessorNumber();
            if (core != on) moves++, on = core;
            if (now != last) {
                if (changes++ < 6) nx2_log("[pac] free to move: the signature changed, %x to %x, on core %u\n", last, now, (u64)core);
                last = now;
            }
            if (!(++rounds & 1023)) svcSleepThread(50000);
        }
        nx2_log("[pac] free to move: %u changes of the signature in four seconds, %u moves between cores\n", changes, (u64)moves);
    }
}
#endif
