#pragma once

#include "AssetCore/GUID.h"

#include <cstddef>
#include <functional>
#include <span>

namespace GameEngine::AssetDatabase
{

class IAssetStore;

// Longest redirect chain any resolve follows. One hop is emitted per healed
// rename, so this bounds how many renames deep a stale reference can be and
// still resolve.
inline constexpr int kMaxRedirectChainDepth = 8;

/// Which record the chain-final GUID must have for RedirectChain::TargetAccepted.
/// Each resolve site states its own rule here: the registry's resident map and
/// the store hold different sets (the store keeps journaled records for GUIDs
/// that are not currently registered), so the two are not interchangeable.
enum class RedirectTargetCheck
{
    None,           ///< Nothing is required of the chain-final GUID.
    StoreRecord,    ///< The store must hold an AssetRecord for it.
    ResidentRecord  ///< The caller's resident-record probe must accept it.
};

/// "Is there a live record for this GUID?" answered outside the store — the
/// asset registry's resident map. Consulted only for
/// RedirectTargetCheck::ResidentRecord, and then at most once, on the
/// chain-final GUID, so a probe may cache what it looked up.
using RedirectResidentProbe = std::function<bool(const GUID&)>;

/// Outcome of following a redirect chain to its end.
struct RedirectChain
{
    /// Chain-final GUID: the queried GUID itself when nothing resolved.
    GUID Final = GUID::Null();
    /// Final differs from the queried GUID.
    bool Redirected = false;
    /// Redirected AND the chain-final GUID satisfied the requested check
    /// (RedirectTargetCheck::None requires nothing, so it accepts any
    /// redirected target). False whenever nothing redirected.
    bool TargetAccepted = false;
    /// The chain revisited a GUID. Final is the queried GUID.
    bool CycleDetected = false;
};

/// Follows `from` through `store`'s redirects to the chain-final GUID.
///
/// A cycle resolves to `from` — the queried GUID speaks for itself when the
/// graph cannot name a successor — and raises a rate-limited warning naming
/// the members. A self-redirect (from -> from) is a terminator, not a cycle:
/// it resolves to itself at any chase length and says nothing diagnostic.
/// Cycles are reported, never repaired.
RedirectChain ChaseRedirectChain(const IAssetStore& store,
                                 const GUID& from,
                                 RedirectTargetCheck check,
                                 const RedirectResidentProbe& residentProbe);

/// Rewrites every redirect that points AT one of `sources` so it points at
/// that source's chain-final target instead, and returns how many hops were
/// rewritten.
///
/// Call this immediately before removing the sources' own outgoing hops: it is
/// what carries the rename direction across the removal (C -> A -> B keeps
/// reaching B once A's hop goes). Retargeting on its own never changes what
/// any GUID resolves to — it only shortens chains — so it is safe to run even
/// if the removal that follows does not happen.
size_t RetargetIncomingRedirects(IAssetStore& store, std::span<const GUID> sources);

} // namespace GameEngine::AssetDatabase
