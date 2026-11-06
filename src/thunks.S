/* Calling-convention-agnostic glue. Only eax/ecx/edx and the stack are
   touched; the x87 stack is left alone so float return values survive. */
    .intel_syntax noprefix
    .text

/* Stack on entry: [hook] [return address] [args...] */
    .globl _ws_scope_enter
_ws_scope_enter:
    push eax
    push ecx
    push edx
    lea eax, [esp + 16]             /* &return address */
    push eax
    push dword ptr [esp + 16]       /* hook */
    call _ScopeEnter
    add esp, 8
    mov [esp + 12], eax             /* replace hook slot with trampoline */
    pop edx
    pop ecx
    pop eax
    ret                             /* -> trampoline */

/* The scoped function returns here (stack already cleaned if stdcall). */
    .globl _ws_scope_exit
_ws_scope_exit:
    push eax                        /* slot for the real return address */
    push eax
    push ecx
    push edx
    call _ScopeLeave
    mov [esp + 12], eax
    pop edx
    pop ecx
    pop eax
    ret

/* Replaces `mov [m_nMousePosWinX], ecx` in the window procedure. */
    .globl _ws_menu_mouse_x
_ws_menu_mouse_x:
    push eax
    push edx
    push ecx
    call _RemapMenuMouseX
    pop ecx
    mov dword ptr [0xBA6828], eax
    pop edx
    pop eax
    ret

/* Replaces `fld [TheCamera.m_f3rdPersonCHairMultX]` in the aim code: the
   horizontal offset of the crosshair from the centre, widened by Hor+. */
    .globl _ws_aim_mult_x
_ws_aim_mult_x:
    fld dword ptr [0xB6EC14]
    fsub dword ptr [_g_half]
    fmul dword ptr [_g_horFactor]
    fadd dword ptr [_g_half]
    ret

/* Replaces `fld [CDraw::ms_fFOV]` in the frustum setup: the horizontal FOV
   actually rendered, 2 * atan(tan(fov / 2) * horFactor), in degrees. */
    .globl _ws_render_fov
_ws_render_fov:
    fld dword ptr [0x8D5038]
    fmul dword ptr [_g_degToRadHalf]
    fptan
    fstp st(0)
    fmul dword ptr [_g_horFactor]
    fld1
    fpatan
    fmul dword ptr [_g_radToDegTwice]
    ret
