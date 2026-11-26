// the menu's font (tools/font.py)
    .section .rodata.nx2_font, "a", %progbits
    .balign 8
    .global nx2_font
nx2_font:
    .incbin "font.bin"
