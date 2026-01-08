#include "mimicdb/scan.h"
#include "mimicdb/simd.h"

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace mimicdb {

ScanLoopFn GetScanKernel() {
    return &ScanLoop;
}

ScanLoopMaskedFn GetScanKernelMasked() {
    return &ScanLoopMasked;
}

bool CpuHasAvx2() {
#if defined(__x86_64__) || defined(__i386__)
    static const bool has_avx2 = []() {
        unsigned int eax = 0;
        unsigned int ebx = 0;
        unsigned int ecx = 0;
        unsigned int edx = 0;
        if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
            return false;
        }
        if (eax < 7) {
            return false;
        }
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            return false;
        }
        const bool osxsave = (ecx & (1u << 27)) != 0;
        const bool avx = (ecx & (1u << 28)) != 0;
        if (!(osxsave && avx)) {
            return false;
        }
        uint32_t xcr0_lo = 0;
        uint32_t xcr0_hi = 0;
        __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        const uint64_t xcr0 = (static_cast<uint64_t>(xcr0_hi) << 32) | xcr0_lo;
        if ((xcr0 & 0x6) != 0x6) {
            return false;
        }
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        return (ebx & (1u << 5)) != 0;
    }();
    return has_avx2;
#else
    return false;
#endif
}

bool CpuHasNeon() {
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
    static const bool has_neon = []() {
        const unsigned long hwcap = getauxval(AT_HWCAP);
#if defined(__aarch64__)
        return (hwcap & HWCAP_ASIMD) != 0;
#else
        return (hwcap & HWCAP_NEON) != 0;
#endif
    }();
    return has_neon;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return true;
#else
    return false;
#endif
}

}  // namespace mimicdb
