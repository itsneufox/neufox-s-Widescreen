#include "mem.hpp"
#include "config.hpp"
#include "region.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mem
{
    // Every address passed in is a 1.0 US address, turned into the running
    // exe's by region::FromUS. That must happen exactly once per address (EU
    // addresses in the shifted range would shift again), so the public
    // functions translate and the Raw* ones work on final addresses.
    static uintptr_t Region(uintptr_t us) { return region::FromUS(us); }

    static bool RawWrite(uintptr_t addr, const void* data, size_t len)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::memcpy(reinterpret_cast<void*>(addr), data, len);
        VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
        return true;
    }

    static bool RawMatches(uintptr_t addr, const uint8_t* expect, size_t len)
    {
        return std::memcmp(reinterpret_cast<const void*>(addr), expect, len) == 0;
    }

    static void EncodeRel(uint8_t* out, uint8_t opcode, uintptr_t from, const void* to)
    {
        out[0] = opcode;
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (from + 5));
        std::memcpy(out + 1, &rel, 4);
    }

    static bool RawWriteJump(uintptr_t at, const void* to)
    {
        uint8_t buf[5];
        EncodeRel(buf, 0xE9, at, to);
        return RawWrite(at, buf, sizeof(buf));
    }

    bool Write(uintptr_t addr, const void* data, size_t len) { return RawWrite(Region(addr), data, len); }
    bool Matches(uintptr_t addr, const uint8_t* expect, size_t len) { return RawMatches(Region(addr), expect, len); }
    bool WriteJump(uintptr_t at, const void* to) { return RawWriteJump(Region(at), to); }

    bool WriteCall(uintptr_t at, const void* to, size_t len)
    {
        at = Region(at);
        uint8_t buf[16];
        if (len < 5 || len > sizeof(buf))
            return false;
        std::memset(buf, 0x90, len);
        EncodeRel(buf, 0xE8, at, to);
        return RawWrite(at, buf, len);
    }

    uintptr_t Address(uintptr_t us) { return Region(us); }

    bool RepointOperand(uintptr_t insn, const uint8_t* opcode, size_t opcodeLen,
                        uint32_t expectedOperand, const void* newOperand)
    {
        insn = Region(insn);
        expectedOperand = static_cast<uint32_t>(Region(expectedOperand));
        uint32_t current;
        std::memcpy(&current, reinterpret_cast<const void*>(insn + opcodeLen), 4);
        if (!RawMatches(insn, opcode, opcodeLen) || current != expectedOperand)
        {
            Log("skip: operand at %08X is not [%08X]", insn, expectedOperand);
            return false;
        }
        uint32_t ptr = reinterpret_cast<uintptr_t>(newOperand);
        return RawWrite(insn + opcodeLen, &ptr, 4);
    }

    uint8_t* AllocCode(size_t len)
    {
        static uint8_t* pool = nullptr;
        static size_t used = 0, size = 0;
        len = (len + 15) & ~size_t(15);
        if (!pool || used + len > size)
        {
            size = 0x10000;
            pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            used = 0;
            if (!pool)
                return nullptr;
        }
        uint8_t* p = pool + used;
        used += len;
        return p;
    }

    void* Detour(const char* name, uintptr_t addr, const uint8_t* expect, size_t len, const void* hook, int relAt)
    {
        addr = Region(addr);
        auto* code = reinterpret_cast<const uint8_t*>(addr);
        uint8_t* tramp = AllocCode(len + 5);
        if (!tramp)
            return nullptr;

        if (RawMatches(addr, expect, len))
        {
            std::memcpy(tramp, code, len);
            if (relAt >= 0)
            {
                int32_t rel;
                std::memcpy(&rel, code + relAt + 1, 4);
                EncodeRel(tramp + relAt, code[relAt], reinterpret_cast<uintptr_t>(tramp + relAt), code + relAt + 5 + rel);
            }
            EncodeRel(tramp + len, 0xE9, reinterpret_cast<uintptr_t>(tramp + len), code + len);
        }
        else if (code[0] == 0xE9)
        {
            // Someone hooked this before us: run their hook as "the original".
            int32_t rel;
            std::memcpy(&rel, code + 1, 4);
            EncodeRel(tramp, 0xE9, reinterpret_cast<uintptr_t>(tramp), code + 5 + rel);
            Log("%s: chaining behind existing hook at %08X", name, addr);
        }
        else
        {
            Log("skip: %s at %08X has an unknown prologue", name, addr);
            return nullptr;
        }

        if (!RawWriteJump(addr, hook))
            return nullptr;
        return tramp;
    }
}

void Log(const char* fmt, ...)
{
    static FILE* file = nullptr;
    if (!config::logPath[0])
        return;
    if (!file && !(file = std::fopen(config::logPath, "w")))
        return;

    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}
