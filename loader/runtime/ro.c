// The modules a Switch 2 game loads as it runs (nn::ro: a game has 36 of them in its files, one for each of
// its games and demonstrations), put in memory by kaiHEN's loader service, which is the console's "ldr:ro" for a homebrew program.
// What the game's SDK says to that service is seen here (svc.c hands every request on a session named ldr:ro to nx2_ro_ipc), because
// kaiHEN's is not the console's in these ways, each met on the console (logs, 2026-11-25):
//
//   - It serves one session only, the one it made ready for this process and handed the program at its start (the host has it as an
//     override; whole.c maps the game's own modules through it). On the session the SDK opens itself, the SDK's "this is my process"
//     (command 4) was refused, 1759, and the SDK ended the game. So that is answered here, and every other request is sent on the
//     ready session.
//   - The game's list of the modules it may load (an NRR; commands 2, 3, 10) is not for it: it loads a module unasked, as it loads
//     this port's own. Answered here.
//   - It maps eight images for a program, the program's own among them (measured: <folder>/rocount, whole.c): a ninth is refused,
//     e16. The game's own modules kept whole use some of the eight, which is why as few of them as can be are kept whole
//     (tools/relink.py). A refusal is said in the log with how many are loaded.
//
// What is the same as the console's, and was once taken for a difference: a module whose header has flag 1 is given read access
// only to its first page, where its header is, and the SDK finds it so.
// The loader's own part in this (rtld's) is in loader.c: three pointers of the SDK's that rtld sets, of which two were left null.
#ifdef __SWITCH2__
#include <switch.h>

#include "nx2.h"

u32 nx2_ro_kernel;
static u32 loaded;

static u32 rd(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd8(const u8 *p) { return (u64)rd(p) | (u64)rd(p + 4) << 32; }

// the answer in the message buffer: its result and where its data is (NULL: none there)
static const u8 *answered(const u8 *m, u32 *result) {
    for (u32 k = 8; k + 16 <= 0x100; k += 4)
        if (m[k] == 'S' && m[k + 1] == 'F' && m[k + 2] == 'C' && m[k + 3] == 'O') {
            *result = rd(m + k + 8);
            return m + k + 16;
        }
    *result = ~0u;
    return NULL;
}

void nx2_ro_start(void) {}

int nx2_ro_ipc(u8 *m, const u8 *q, u32 cmd, const char *module, u64 off) {
    const Handle ready = smGetServiceOverride(smEncodeName("ldr:ro"));
    if (ready == INVALID_HANDLE) return 0;
    nx2_ro_kernel = 0;
    if (cmd == 4 || cmd == 2 || cmd == 3 || cmd == 10) {
        nx2_log("[ro] ldr:ro command %u (%s): answered here\n", (u64)cmd, cmd == 4 ? "the SDK says whose process this is" : "the game's list of its modules, an NRR");
        // (an answer with nothing but its result, as a plain session's is: not a domain's)
        for (u32 k = 0; k < 0x100; k++) m[k] = 0;
        m[4] = 8;  // (words of data: 8 bytes to the next 16, the 16 of the header, 8 after)
        m[0x10] = 'S', m[0x11] = 'F', m[0x12] = 'C', m[0x13] = 'O';
        return 1;
    }
    const u8 *raw = q + 16;  // (after "SFCI", its version, the command: the process's number's place, then the rest)
    const u64 a1 = rd8(raw + 8), a2 = rd8(raw + 16);
    nx2_ro_kernel = svcSendSyncRequest(ready);
    u32 result = ~0u;
    const u8 *a = nx2_ro_kernel ? NULL : answered(m, &result);
    if (cmd == 0) {  // LoadNro(image, size, bss, bss size): where it was put
        const u32 n = result ? __atomic_load_n(&loaded, __ATOMIC_RELAXED) : __atomic_add_fetch(&loaded, 1, __ATOMIC_RELAXED);
        nx2_log("[ro] a module of %x bytes (at %p) loaded for the game, from %s+%x: kernel %x, result %x%s; it is at %x; %u loaded now\n", a2, a1, module, off,
                (u64)nx2_ro_kernel, (u64)result, result == 0xE16 ? " (REFUSED: kaiHEN maps eight images for a program, this port's and the game's own kept whole among them)" : "",
                a && !result ? rd8(a) : 0, (u64)n);
    } else if (cmd == 1) {  // UnloadNro(where it is)
        const u32 n = result ? __atomic_load_n(&loaded, __ATOMIC_RELAXED) : __atomic_sub_fetch(&loaded, 1, __ATOMIC_RELAXED);
        nx2_log("[ro] the module at %x unloaded: kernel %x, result %x; %u loaded now\n", a1, (u64)nx2_ro_kernel, (u64)result, (u64)n);
    } else {
        nx2_log("[ro] ldr:ro command %u, from %s+%x: sent on the ready session: kernel %x, result %x\n", (u64)cmd, module, off, (u64)nx2_ro_kernel, (u64)result);
    }
    return 1;
}
#endif
