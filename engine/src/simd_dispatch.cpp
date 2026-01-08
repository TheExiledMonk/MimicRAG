#include "mimicdb/scan.h"
#include "mimicdb/simd.h"

namespace mimicdb {

ScanLoopFn GetScanKernel() {
    return &ScanLoop;
}

ScanLoopMaskedFn GetScanKernelMasked() {
    return &ScanLoopMasked;
}

bool CpuHasAvx2() {
#if defined(__AVX2__)
    return true;
#else
    return false;
#endif
}

bool CpuHasNeon() {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return true;
#else
    return false;
#endif
}

}  // namespace mimicdb
