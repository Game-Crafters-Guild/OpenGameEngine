#pragma once

// Who is allowed to own managed element types.
//
// ManagedElementTypes takes an owner id as a plain number and trusts it. That is correct for
// C++ callers: native code is inside the trust boundary, and anything it could achieve by
// passing someone else's id it could achieve by calling UnregisterFactoriesOwnedBy directly.
// Managed code is NOT inside that boundary — a script can declare its own DllImport and call
// the C ABI with any number it likes. Without this registry the ownership property that slice
// 3a built would be decorative on the only surface it was built for.
//
// So the id is MINTED HERE and never chosen by the caller. A managed load context asks for
// one at registration time and hands it back at unload; every C-ABI entry point in the family
// validates its owner against the live set first. Three of the four ways to name someone
// else's types are closed outright:
//
//   chosen   a caller cannot supply an id, only receive one
//   spoofed  an id that was never minted is refused, and so is 0 (the engine)
//   reused   a released id is retired for the life of the process and never minted again
//
// The fourth — GUESSING a currently-live id — is not closable by validation in a flat C ABI,
// so it is closed by the value instead: ids come from a 64-bit random source rather than a
// counter, which is why this is not simply `++s_Next`. That is a probabilistic barrier and it
// is stated as one; it is not a cryptographic guarantee, and a script that legitimately learns
// another context's id can still act on it.
//
// Threading: unlike the rest of the managed-element machinery this is NOT main-thread-only.
// Acquire runs on the load path and Release runs from the ALC unload seam, which may be any
// thread. The lock below guards this registry's own two containers and NOTHING else — it says
// nothing about the UI tree, and a caller must not read it as making the element machinery
// safe to drive from an arbitrary thread.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace GameEngine::Scripting
{

// Which C-ABI entry point refused an owner. An enum rather than the call-site string, so a
// refusal record is a fixed-size key: a caller that forges ids in a loop must not be able to
// grow this registry's memory by naming call sites.
enum class OwnerEntryPoint : std::uint8_t
{
    ReleaseTypeOwner,
    RegisterElementType,
    OrphanElementTypes,
    NotifyReloadCompleted,
};

class ManagedTypeOwners
{
public:
    static ManagedTypeOwners& Instance();

    // A fresh owner id for one managed load context. Never 0, never equal to a live or a
    // retired id. The caller must hand it back through Release when its context unloads.
    std::uint64_t Acquire();

    // Retire an owner id. Returns false if it was not live — which is what a double-release
    // or a forged id looks like, and the ABI turns that into a refusal rather than acting on
    // it. Does NOT sweep the owner's types: releasing the id and orphaning the elements are
    // separate steps with a required order, and the ABI performs them in it.
    bool Release(std::uint64_t owner);

    // Is this a live minted id? The silent predicate — see ValidateOwner for the one the C ABI
    // actually gates on.
    bool IsLive(std::uint64_t owner) const;

    // IsLive, and a refusal is REPORTED: one warning naming the rejected id and the entry point
    // that rejected it. Every owner-taking C-ABI entry point gates on this rather than on
    // IsLive, because a return code alone is invisible — a forged or stale id would be
    // indistinguishable from a call that legitimately had nothing to do.
    bool ValidateOwner(std::uint64_t owner, OwnerEntryPoint entryPoint);

    // Report a refusal this registry did not decide itself. Release answers false rather than
    // reporting, because "not live" is its return VALUE; its ABI caller turns that into the
    // same refusal record every other entry point produces.
    void ReportRejected(std::uint64_t owner, OwnerEntryPoint entryPoint);

    // Refusals since process start, or since the last ResetForTests. The observable half of the
    // contract above, so a test asserts on a number instead of on log text.
    std::size_t RejectedCallCount() const;

    // Test seams.
    std::size_t LiveCount() const;
    void ResetForTests();

private:
    ManagedTypeOwners() = default;

    // Has this (owner, entry point) not been reported yet, and is there budget left to report
    // it? Dedup alone would let a loop over forged ids grow the set without bound, which is the
    // same flood in a different dimension; past the cap refusals are still COUNTED, just not
    // logged. Caller must hold m_Mutex.
    bool ShouldReportLocked(std::uint64_t owner, OwnerEntryPoint entryPoint);

    // Distinct (owner, entry point) pairs this process will name in the log.
    static constexpr std::size_t kMaxReportedRejections = 64;

    mutable std::mutex m_Mutex;
    std::unordered_set<std::uint64_t> m_Live;
    // Retired ids stay here so a released id can never be minted again. An id names one load
    // context for the life of the process, exactly as an element instance id names one element.
    std::unordered_set<std::uint64_t> m_Retired;
    std::unordered_set<std::uint64_t> m_Reported; // (owner, entry point) pairs already logged
    std::size_t m_RejectedCalls = 0;
};

} // namespace GameEngine::Scripting
