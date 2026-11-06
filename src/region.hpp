#pragma once
#include <cstdint>

// GTA San Andreas 1.0 comes as US and EU executables. The EU one has 0x50
// bytes more code before 0x7BA940 and 0x40 after it, which moves everything
// from 0x7466D0 up to 0x857000; the rest of the image is identical. The mod
// is written against US addresses and translates them here.
namespace region
{
    enum class Exe : uint8_t { Unknown, US10, EU10 };

    // Identified once, from the same signature word at each exe's own offset.
    Exe Detect();
    const char* Name(Exe exe);

    // A 1.0 US address as it is in the running exe. Not idempotent on EU:
    // translate each address exactly once.
    uintptr_t FromUS(uintptr_t us);
}
