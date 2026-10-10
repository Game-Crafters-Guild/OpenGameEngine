#pragma once

// ECS change signaling, piece 1: per-(chunk x column) uint64
// write-grant versions stamped when write access is granted, plus the
// Changed<T> query filter that skips chunks whose filtered columns did not
// change since a consumer's last run.
//
// All mutable filter state lives on World / ArchetypeTable members, never in
// inline globals. This is load-bearing, learned in the Step-0 prototype
// (PR #374): ECS template code instantiates in EVERY image that uses it —
// Editor.exe AND GameEngine.Native.dll — and each image materializes its own
// copy of any inline global (exports.def strips data exports). A first cut
// with namespace-scope inline atomics had editor-side writes stamping an
// Editor.exe counter while Engine.dll-side systems gated on the DLL's —
// movers froze. World members are one memory location no matter which
// image's code runs (the design's M9/R10 dual-image hazard applies INSIDE
// the host process, not just to the managed DLL).
//
// The only per-image state here is the consumer-lane env switch below
// (process-constant, identical in every image).

#include <cstdint>
#include <cstdlib>

namespace GameEngine::ECS
{

// Per-(system, query) gate for Changed<T> filtering. Zero-init means "see
// everything once" — under the monotonic uint64 compare a fresh gate is the
// oldest possible value, and uint64 makes wrap unreachable for the process
// lifetime (design C8: per-access increment rates wrap uint32 in minutes;
// at 10^9 increments/s uint64 lasts ~292 years).
//
// Consumer contract (design M14, pinned by GateWriteBackIsEntrySample):
// entry-sample World::GetGlobalSystemVersion() BEFORE iterating and write
// that sample back as the next gate — never a fresh end-of-run resample.
// Same-wave systems run concurrently; an end-of-run resample can exceed a
// concurrent writer's stamp and skip it permanently.
//
// One gate per (system, query) — sharing a gate across unrelated queries
// silently over/under-filters (design R8).
struct ChangeGate
{
    uint64_t LastRunVersion = 0;
};

namespace ChangeFilter
{

// Consumer-lane kill switch: GE_ECS_CHANGE_FILTER=0 disables the gated
// consumers (TransformHierarchySystem's flat gate, ensure gate, and the
// serial all-or-nothing skip), restoring pre-filter behavior exactly.
// Stamping itself is always on — the write grants are the production
// mechanism and their cost is one relaxed uint64 store per visited chunk
// column (design R12: noise next to the callback).
inline bool Enabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_ECS_CHANGE_FILTER");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

} // namespace ChangeFilter

} // namespace GameEngine::ECS
