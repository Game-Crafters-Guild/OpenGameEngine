#pragma once

// AsyncAssetHelpers: schedule an asset load and run a continuation on the main
// thread once it lands. Used by drop, import and inspector handlers so a
// first-time load (a cook, an import) never blocks the editor.

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetManager;
class UIElement;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// Run `onReady` on the main thread once `guid` is loaded into the AssetManager
// cache, or once its load has failed (`onReady` then finds no asset). Fast
// path: if the asset is already loaded, runs `onReady` before returning. Slow
// path: starts the load and runs `onReady` from PollPendingAssetLoads once the
// load has finished, so `assets.GetAsset(guid)` inside it does not wait.
//
// `postTarget` is held weakly: an element destroyed before the load completes
// drops the continuation. A null `postTarget` is for an owner that outlives the
// editor's frame loop.
//
// `world` is the world the continuation writes to. Opening a scene clears that
// world in place (the same object, entity ids reused), so the continuation is
// dropped when the world's lifecycle reset generation has moved since the call:
// a drop aimed at one scene never lands in the next. The world must outlive the
// pending load. Null for a continuation that does not write to a world, or whose
// owner checks it itself.
//
// `pendingLabel` names the load to the user while it is pending (the Scene View
// says "Loading <label>..."); empty for a load the user is not waiting on.
void RunWhenAssetLoaded(AssetManager& assets,
                        const GUID& guid,
                        AssetLoadPriority priority,
                        UIElement* postTarget,
                        const ECS::World* world,
                        std::function<void()> onReady,
                        std::string pendingLabel);

// Runs the continuations of the loads that have finished. Main thread, once
// per frame (PendingAssetLoadsOverlay).
void PollPendingAssetLoads();

// The labels of the pending loads that carry one, oldest first.
std::vector<std::string> PendingAssetLoadLabels();

// Drops every pending continuation without running it (editor teardown).
void ClearPendingAssetLoads();

} // namespace GameEngine::Editor
