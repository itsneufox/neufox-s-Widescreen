/* The documented neufoxWidescreen.ini, written next to the .asi on start
   when there is none (main.cpp). */
    .section .rdata,"dr"
    .globl _g_defaultIni
_g_defaultIni:
    .incbin "neufoxWidescreen.ini"
    .byte 0
