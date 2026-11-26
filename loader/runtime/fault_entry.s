// A fault, as the kernel hands it to this program: the program's start is entered again with the kind of fault in x0 and, in x1, where
// the kernel left the faulting thread's registers (x0 to x8, the link register, the stack pointer, the program counter, then the
// state, two auxiliary words, the fault's class and its address). The host library's own code for this said nothing on a Switch 2
// (a fault made on purpose left no line; 2026-11-24), so this comes first (the linker's --wrap, tools/build.py): the fault is
// written to the log by nx2_fault_raw (loader.c), on a stack of the port's, and then the host library's code goes on as it would.
    .section .text.nx2_fault_entry, "ax", %progbits
    .balign 4
    .global __wrap___libnx_exception_entry
    .type __wrap___libnx_exception_entry, %function
__wrap___libnx_exception_entry:
    adrp x9, nx2_fault_room
    add x9, x9, :lo12:nx2_fault_room
    add x9, x9, #0x8000
    mov x10, sp
    mov sp, x9
    stp x0, x1, [sp, #-48]!
    stp x10, x30, [sp, #16]
    stp x29, x18, [sp, #32]
    mov x2, x29
    bl nx2_fault_raw
    ldp x29, x18, [sp, #32]
    ldp x10, x30, [sp, #16]
    ldp x0, x1, [sp], #48
    mov sp, x10
    b __real___libnx_exception_entry

    .section .bss.nx2_fault_room, "aw", %nobits
    .balign 16
nx2_fault_room:
    .space 0x8000
