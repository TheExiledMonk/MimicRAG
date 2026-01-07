# PCDB Standards

- No RTTI in hot paths
- No exceptions in hot paths
- No virtual dispatch in scan loops
- No per-row allocations in engine code
- Every optimization must reduce rows scanned or bytes touched per row
