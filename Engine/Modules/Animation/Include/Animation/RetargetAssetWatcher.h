#pragma once

// RetargetAssetWatcher: subscribes to AssetReloadInvalidator events for
// SkeletonProfile / HumanoidRig / RetargetMap and fans them out to
// per-type callback lists. Phase 1 ships the infrastructure; Phase 3+
// uses it to trigger HumanoidRetargetSystem GPU re-bakes when any of the
// three asset types reload.
//
// Lifetime: instantiate one per consumer (e.g., HumanoidRetargetSystem).
// The watcher holds three AssetReloadInvalidators (one per type); each
// fires on AssetReloaded / AssetUnloaded / AssetDestroyed.

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/GUID.h"

#include <functional>
#include <vector>

namespace GameEngine
{
namespace Animation
{

class RetargetAssetWatcher
{
  public:
    using Callback = std::function<void(const ::GameEngine::GUID&)>;

    RetargetAssetWatcher() = default;
    ~RetargetAssetWatcher() = default;

    // Subscribe to a dispatcher. The watcher installs three reload
    // invalidators and routes events to the callback lists. Idempotent
    // if called twice with the same dispatcher; calling with a different
    // dispatcher resets the previous subscription.
    void Attach(::GameEngine::AssetEventDispatcher& dispatcher);

    // Detach from the current dispatcher. Safe to call multiple times.
    void Detach();

    bool IsAttached() const { return m_Attached; }

    // Register callbacks. Multiple callbacks per type are supported and
    // fire in registration order. There is no Unregister at present;
    // expected lifetime is "live for the duration of the owning system".
    void OnProfileReloaded(Callback cb);
    void OnRigReloaded(Callback cb);
    void OnMapReloaded(Callback cb);

    // Test seam: directly fire a callback list without going through the
    // dispatcher. Used by RetargetAssetWatcherTests to verify wiring.
    void FireProfileReloadedForTest(const ::GameEngine::GUID& guid);
    void FireRigReloadedForTest(const ::GameEngine::GUID& guid);
    void FireMapReloadedForTest(const ::GameEngine::GUID& guid);

  private:
    void DispatchProfile(const ::GameEngine::GUID& guid);
    void DispatchRig(const ::GameEngine::GUID& guid);
    void DispatchMap(const ::GameEngine::GUID& guid);

    bool m_Attached = false;
    ::GameEngine::AssetReloadInvalidator m_ProfileWatcher;
    ::GameEngine::AssetReloadInvalidator m_RigWatcher;
    ::GameEngine::AssetReloadInvalidator m_MapWatcher;

    std::vector<Callback> m_ProfileCallbacks;
    std::vector<Callback> m_RigCallbacks;
    std::vector<Callback> m_MapCallbacks;
};

} // namespace Animation
} // namespace GameEngine
