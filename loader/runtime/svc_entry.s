// Where the game's system calls come: each `svc #n` in its code is now a branch to a thunk (tools/relink.py) that calls this with n
// in w16. The registers go on the stack as nx2_svc's array (x0-x7 first), and it answers the call itself (1) or not (0), in which
// case the real one runs, with the registers as they came. Everything but x0-x7 is given back as it was.
// The port's own code, when a thread of the game's comes here, runs on a stack of the port's: a system call costs a thread no
// stack on the console (the kernel has its own), and a game gives some threads very little. A game has threads of
// 4 KB; this port's handler of a system call uses more than that, below the thread's stack, in whatever memory was there, and
// the game was closed at some moment between its twelfth second and its eighteenth, never the same (a Switch 2, 2026-11-24).
// A stack is taken from a pool (a byte each says it is in use) and given back after; with none free, the thread's own is used.
// x9 to x15 are used up.
    .set OWN_STACKS, 96
    .macro OWN_STACK fn
    adrp x9, nx2_own_used
    add x9, x9, :lo12:nx2_own_used
    mov x10, #0
10: add x12, x9, x10
11: ldaxrb w13, [x12]
    cbnz w13, 12f
    mov w14, #1
    stlxrb w15, w14, [x12]
    cbnz w15, 11b
    b 13f
12: clrex
    add x10, x10, #1
    cmp x10, #OWN_STACKS
    b.ne 10b
    bl \fn
    b 14f
13: adrp x11, nx2_own_stacks
    add x11, x11, :lo12:nx2_own_stacks
    add x10, x10, #1
    add x11, x11, x10, lsl #16
    mov x13, sp
    mov sp, x11
    stp x13, x12, [sp, #-16]!
    bl \fn
    ldp x13, x12, [sp], #16
    mov sp, x13
    stlrb wzr, [x12]
14:
    .endm
    .section .bss.nx2_own, "aw", %nobits
    .balign 16
nx2_own_stacks:
    .space OWN_STACKS << 16
nx2_own_used:
    .space OWN_STACKS

    .section .text.nx2_svc_entry, "ax", %progbits
    .balign 4
    .global nx2_svc_entry
    .type nx2_svc_entry, %function
nx2_svc_entry:
    sub sp, sp, #176
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x17, x18, [sp, #128]
    stp x29, x30, [sp, #144]
    str x16, [sp, #160]
    mov x0, sp
    mov w1, w16
    OWN_STACK nx2_svc
    cbnz w0, 2f
    ldr x16, [sp, #160]
    adrp x17, nx2_svc_stubs
    add x17, x17, :lo12:nx2_svc_stubs
    add x17, x17, x16, lsl #3
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    blr x17
    b 3f
2:
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
3:
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x17, x18, [sp, #128]
    ldp x29, x30, [sp, #144]
    add sp, sp, #176
    ret

// the real calls, one for each number: 8 bytes each
    .balign 8
    .global nx2_svc_stubs
nx2_svc_stubs:
    .set n, 0
    .rept 0xC0
    svc n
    ret
    .set n, n + 1
    .endr

// Calls between modules (a module's PLT slots), told to the log: the loader points each slot of the game's at one of these, which
// says which it is (w17: x16 and x17 are a PLT's to use) and goes on to the function through nx2_plt_called.
    .section .text.nx2_plt, "ax", %progbits
    .balign 4
    .global nx2_plt_pool
nx2_plt_pool:
    .set n, 0
    .rept 8192
    mov w17, #n
    b nx2_plt_trace
    .set n, n + 1
    .endr
nx2_plt_trace:
    // (x9 to x15 too: they are a caller's to lose in an ordinary call, but a function may be made to keep them, and the code between
    // here and the function uses them)
    sub sp, sp, #288
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x30, [sp, #64]
    stp x29, x18, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    stp x9, x10, [sp, #224]
    stp x11, x12, [sp, #240]
    stp x13, x14, [sp, #256]
    str x15, [sp, #272]
    mov w0, w17
    mov x1, x30
    OWN_STACK nx2_plt_called
    mov x16, x0
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x30, [sp, #64]
    ldp x29, x18, [sp, #80]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    ldp x9, x10, [sp, #224]
    ldp x11, x12, [sp, #240]
    ldp x13, x14, [sp, #256]
    ldr x15, [sp, #272]
    add sp, sp, #288
    br x16

// Calls from the game into the host's own libraries (the Switch 2's OpenGL in place of the game's driver; gl2.c). A thread of the
// game's has the SDK's data in the upper half of its thread-local region, where the host library keeps a thread's own (its record of
// the thread, its C library state, the slots of pthread keys): so for the length of the call that half is the host's, as it was
// when the port's main thread handed over to the game, and the game's is put back after. (One thread: the game's main one.)
// Each entry says which function (w17); x0-x8, the vector arguments and 64 bytes of stack arguments go through as they came.
    .section .text.nx2_host, "ax", %progbits
    .balign 4
    .global nx2_host_pool
nx2_host_pool:
    .set n, 0
    .rept 1024
    mov w17, #n
    b nx2_host_call
    .set n, n + 1
    .endr
nx2_host_call:
    // (Any thread of the game's, several at once: what the thread had in that half is kept on its own stack, not in one place for
    // all of them, and the host's half is given the thread's own handle (nx2_thread_handle, gl2.c): the host library's locks are
    // marked with it, and with the main thread's in every thread's the kernel woke nobody when one was given back. A game stood still so, one thread inside the OpenGL driver for ever; a Switch 2, 2026-11-23.)
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov w21, w17
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    bl nx2_thread_handle
    mov w22, w0
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldr x8, [sp, #64]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    add sp, sp, #224
    sub sp, sp, #320
    ldp x9, x10, [x29, #48]
    stp x9, x10, [sp, #0]
    ldp x9, x10, [x29, #64]
    stp x9, x10, [sp, #16]
    ldp x9, x10, [x29, #80]
    stp x9, x10, [sp, #32]
    ldp x9, x10, [x29, #96]
    stp x9, x10, [sp, #48]
    adrp x9, nx2_host_targets
    add x9, x9, :lo12:nx2_host_targets
    ldr x19, [x9, w21, uxtw #3]
    mrs x20, tpidrro_el0
    add x20, x20, #0x100
    add x11, sp, #64
    adrp x12, nx2_tls_host
    add x12, x12, :lo12:nx2_tls_host
    mov x13, #0x20              // (from 0x120: the 32 bytes before are the kernel's own, the thread's handle among them)
1:
    ldr x9, [x20, x13]
    str x9, [x11, x13]
    ldr x10, [x12, x13]
    str x10, [x20, x13]
    add x13, x13, #8
    cmp x13, #0x100
    b.ne 1b
    cbz w22, 3f
    str w22, [x20, #0xe4]
3:
    blr x19
    add x11, sp, #64
    mov x13, #0x20
2:
    ldr x10, [x11, x13]
    str x10, [x20, x13]
    add x13, x13, #8
    cmp x13, #0x100
    b.ne 2b
    add sp, sp, #320
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
