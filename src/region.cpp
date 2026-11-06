#include "region.hpp"

namespace region
{
    namespace
    {
        constexpr uint32_t kSignature = 0x94BF;
        constexpr uintptr_t kSignatureUS = 0x82457C, kSignatureEU = 0x8245BC;

        // US range that moves on EU, and where the shift drops from 0x50 to 0x40.
        // Measured against the EU exe: the function just before 0x7466D0 is
        // rewritten (longer) on EU, and every function from 0x7466D0 on moves.
        constexpr uintptr_t kShiftStart = 0x7466D0, kShiftEnd = 0x857000, kShiftSplit = 0x7BA940;

        bool Has(uintptr_t at) { return *reinterpret_cast<const uint32_t*>(at) == kSignature; }
    }

    Exe Detect()
    {
        static const Exe exe = Has(kSignatureUS) ? Exe::US10 : Has(kSignatureEU) ? Exe::EU10 : Exe::Unknown;
        return exe;
    }

    const char* Name(Exe exe)
    {
        switch (exe)
        {
        case Exe::US10: return "1.0 US";
        case Exe::EU10: return "1.0 EU";
        default: return "unknown";
        }
    }

    uintptr_t FromUS(uintptr_t us)
    {
        if (Detect() != Exe::EU10 || us < kShiftStart || us >= kShiftEnd)
            return us;
        return us + (us >= kShiftSplit ? 0x40 : 0x50);
    }
}
