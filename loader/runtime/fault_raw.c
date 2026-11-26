// A fault written to the log as the kernel hands it over (fault_entry.s has the story): where it was, what it touched, who called.
#include <switch.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);

void nx2_fault_raw(u64 kind, const u64 *left, const u64 *frame) {
    static u32 told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) > 2) return;
    if (!left) {
        nx2_log("[fault] kind %x, and nothing of the thread's registers\n", kind);
        return;
    }
    // x0 to x8, the link register, the stack pointer, the program counter; then four words of 32 bits and the address touched
    const u64 lr = left[9], sp = left[10], pc = left[11];
    const u32 *tail = (const u32 *)(left + 12);
    const char *m1 = "?", *m2 = "?";
    u64 o1 = pc, o2 = lr & ((1ull << 39) - 1);
    nx2_where(pc, &m1, &o1), nx2_where(lr, &m2, &o2);
    nx2_log("[fault] kind %x at %s+%x, called from %s+%x; the address touched %x (class %x); sp %x\n", kind, m1, o1, m2, o2, *(const u64 *)(tail + 4),
            (u64)tail[3], sp);
    nx2_log("[fault]   x0 %x, x1 %x, x2 %x, x3 %x, x4 %x, x5 %x, x6 %x, x7 %x, x8 %x\n", left[0], left[1], left[2], left[3], left[4], left[5], left[6], left[7],
            left[8]);
    for (int k = 0; k < 20 && frame && !((u64)frame & 15) && (u64)frame >= sp && (u64)frame - sp < 0x400000; k++) {
        const char *m = "?";
        u64 o = frame[1] & ((1ull << 39) - 1);
        nx2_where(frame[1], &m, &o);
        nx2_log("[fault]   called from %s+%x\n", m, o);
        if (frame[0] <= (u64)frame) break;
        frame = (const u64 *)frame[0];
    }
}

// The loader's side of it. The kernel enters the process's start, which is the homebrew loader's, and the loader's code there goes
// on to the program it loaded, if it has that program's address in a variable of its own: "adrp x7; ldr x7, [x7, #n]; cbz x7; br x7".
// Found here in the loader's code, so that the variable can be read (and said): with nothing in it a fault goes nowhere.
#ifdef __SWITCH2__
extern char _start[];
void nx2_fault_loader(void) {
    for (u64 at = 0;;) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, at)) || mi.addr + mi.size <= at) break;
        at = mi.addr + mi.size;
        if (mi.type != MemType_CodeStatic || mi.perm != Perm_Rx || mi.size < 0x1000 || mi.size > 0x100000) continue;
        const u32 *code = (const u32 *)mi.addr;
        if (code[2] != 0x454d4f48) continue;  // ("HOME", of "HOMEBREW": a program made with the host library)
        for (u64 k = 0; k + 3 < mi.size / 4; k++) {
            if ((code[k] & 0x9F00001F) != 0x90000007 || (code[k + 1] & 0xFFC003FF) != 0xF94000E7 || (code[k + 2] & 0xFF00001F) != 0xB4000007 ||
                code[k + 3] != 0xD61F00E0)
                continue;
            const u64 page = ((mi.addr + 4 * k) & ~0xFFFull) + ((u64)(((code[k] >> 5) & 0x7FFFF) << 2 | ((code[k] >> 29) & 3)) << 12);
            u64 *var = (u64 *)(page + (u64)((code[k + 1] >> 10) & 0xFFF) * 8);
            MemoryInfo vi;
            if (R_FAILED(svcQueryMemory(&vi, &pi, (u64)var)) || !(vi.perm & Perm_R)) {
                nx2_log("[fault] the loader (at %x) keeps the program's address at %x, which can't be read\n", mi.addr, (u64)var);
                return;
            }
            nx2_log("[fault] the loader (at %x) hands a fault on to the program at %x (its variable at %x, memory of access %u); this program is at %x\n",
                    mi.addr, *var, (u64)var, (u64)vi.perm, (u64)_start);
            if (*var != (u64)_start && (vi.perm & Perm_W)) {
                *var = (u64)_start;
                nx2_log("[fault] the loader's variable is given this program's address\n");
            }
            return;
        }
        nx2_log("[fault] the loader (at %x): its code that hands a fault on wasn't found\n", mi.addr);
        return;
    }
    nx2_log("[fault] the loader's code wasn't found\n");
}
#endif
