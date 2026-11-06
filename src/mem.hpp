#pragma once
#include <cstddef>
#include <cstdint>

// Low-level patching. Every patch checks the bytes it replaces first, so a
// different game version or another mod's patch is detected and skipped
// instead of corrupting code.
namespace mem
{
    // All addresses are 1.0 US addresses; on 1.0 EU they are translated
    // (region::FromUS). Use Address() for anything not patched through here.
    uintptr_t Address(uintptr_t us);

    bool Write(uintptr_t addr, const void* data, size_t len);
    bool Matches(uintptr_t addr, const uint8_t* expect, size_t len);

    // Replaces `len` bytes at `at` with `call to` followed by NOPs.
    bool WriteCall(uintptr_t at, const void* to, size_t len);
    bool WriteJump(uintptr_t at, const void* to);

    // Repoints the 32-bit absolute memory operand of an instruction, e.g.
    // `fdiv dword ptr [0xC3EFA4]` -> `fdiv dword ptr [&ours]`. `opcode` is
    // the instruction bytes preceding the operand.
    bool RepointOperand(uintptr_t insn, const uint8_t* opcode, size_t opcodeLen,
                        uint32_t expectedOperand, const void* newOperand);

    // Small pool of executable memory for trampolines and thunks.
    uint8_t* AllocCode(size_t len);

    // Hooks the function at `addr`: its first `len` bytes (whole instructions
    // matching `expect`) move to a trampoline and are
    // replaced by a jump to `hook`. `relAt` is the offset of a call/jmp rel32
    // inside those bytes (or -1), which is relocated into the trampoline.
    // If another mod already put a `jmp` there, we chain behind it. Returns
    // the trampoline (call it to run the original), or nullptr if the
    // prologue is not recognised.
    void* Detour(const char* name, uintptr_t addr, const uint8_t* expect, size_t len, const void* hook,
                 int relAt = -1);
}

void Log(const char* fmt, ...);
