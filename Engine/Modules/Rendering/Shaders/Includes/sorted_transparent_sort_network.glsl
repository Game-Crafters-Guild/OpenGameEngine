// Shared-memory bitonic sort network for the sorted transparent GPU sort
// (transparency-scale S1/S2). Single definition site so the standalone sort
// primitive (sorted_transparent_sort.comp) and the fused drain
// (sorted_transparent_drain.comp) can never drift.
//
// Sorts (key, value) pairs ASCENDING by a 64-bit key stored as a uvec2
// {x = lo, y = hi}. Key-encoding-agnostic — it only compares; the producer
// encodes so ascending order means whatever it needs (back-to-front for the
// transparent drain). Padding slots carry key {0xFFFFFFFF, 0xFFFFFFFF} and
// sort to the end.
//
// Contract (F6): the includer declares, BEFORE including this file,
//
//     const uint kCapacity = <power of two>;   // whole array in shared memory
//     shared uvec2 sKeys[kCapacity];
//     shared uint  sVals[kCapacity];
//
// then populates sKeys/sVals (every slot, padding included) and calls
// SortSharedBitonic(gl_LocalInvocationID.x, gl_WorkGroupSize.x) from EVERY
// invocation. The network runs in ONE workgroup — the dispatch MUST be
// (1, 1, 1); a second workgroup would race the same shared-memory picture of
// different data. SortSharedBitonic opens with a barrier (covering the
// caller's population writes) and ends barriered, so sKeys/sVals are safe to
// read from any invocation on return.

#ifndef GE_SORTED_TRANSPARENT_SORT_NETWORK_GLSL
#define GE_SORTED_TRANSPARENT_SORT_NETWORK_GLSL

// Strict weak ordering on the 64-bit key: high word dominates, low word breaks
// ties. Returns true when a should come strictly before b (ascending).
bool KeyLess(uvec2 a, uvec2 b)
{
    if (a.y != b.y)
        return a.y < b.y;
    return a.x < b.x;
}

void SortSharedBitonic(uint lid, uint threads)
{
    barrier();

    // Bitonic network. For each merge size k and sub-step j, every index i is
    // compare-exchanged with i^j; the sort direction alternates per k-block so
    // the network builds bitonic sequences that collapse to fully ascending.
    for (uint k = 2u; k <= kCapacity; k <<= 1u)
    {
        for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            for (uint i = lid; i < kCapacity; i += threads)
            {
                const uint ixj = i ^ j;
                if (ixj > i)
                {
                    // ascending block when the k-bit of i is 0, else descending.
                    const bool ascending = ((i & k) == 0u);
                    const bool outOfOrder = KeyLess(sKeys[ixj], sKeys[i]);
                    if (ascending == outOfOrder)
                    {
                        const uvec2 tk = sKeys[i];
                        sKeys[i] = sKeys[ixj];
                        sKeys[ixj] = tk;
                        const uint tv = sVals[i];
                        sVals[i] = sVals[ixj];
                        sVals[ixj] = tv;
                    }
                }
            }
            barrier();
        }
    }
}

#endif // GE_SORTED_TRANSPARENT_SORT_NETWORK_GLSL
