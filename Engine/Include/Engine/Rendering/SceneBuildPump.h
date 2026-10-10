#pragma once

// SceneBuildPump: the editor's scene-open build. A scene of 21k entities
// resolves ~20s of GPU mesh/texture uploads and material registration on the
// main thread; run whole, it freezes every frame for that whole window. The
// build hands the freshly opened world to a SceneResolveService of its own and
// steps it in bounded per-frame slices, so the editor keeps presenting frames
// and entities materialize progressively.
//
// While a build runs it is the only setter of IsSceneBuildPumpActive(): the
// runtime bind (BindChangedMeshRenderers) and the skeleton binder hold their
// changes until it finishes. The engine's own resolve service never sets it.
//
// A model that is not resident yet (a cold import, a first decode) is never
// waited for by a step. Only DrainNow, which must leave the world resolved,
// waits for a load.

#include "Engine/Rendering/SceneResolveService.h"

#include <chrono>
#include <cstddef>

namespace GameEngine
{
namespace ECS { class World; }

namespace Engine::Renderer
{
class RenderServices;

class SceneBuildPump
{
  public:
    SceneBuildPump() = default;
    ~SceneBuildPump();
    SceneBuildPump(const SceneBuildPump&) = delete;
    SceneBuildPump& operator=(const SceneBuildPump&) = delete;

    /// Hand the pending resolve work of a freshly loaded world to the build.
    /// Safe on a world with nothing to resolve (complete at once). Resets any
    /// prior state, so it also restarts the build for a new scene.
    void Begin(ECS::World& world, RenderServices& renderServices);

    /// Resolve what has landed until `budget` wall-clock elapses; always takes
    /// one ready item, even under a zero budget. Never waits for a load. The
    /// editor's per-frame driver.
    std::size_t StepBudgeted(ECS::World& world, RenderServices& renderServices, std::chrono::milliseconds budget);

    /// Resolve everything synchronously, waiting for loads still in flight. For
    /// a consumer that needs the world fully resolved now: an undoable edit
    /// capturing a world snapshot mid-build would bake unresolved
    /// (meshGpuHandleId == 0) entities into the undo history.
    std::size_t DrainNow(ECS::World& world, RenderServices& renderServices);

    bool IsActive() const { return m_CountedAsActive; }
    bool IsComplete() const { return m_Begun && !m_CountedAsActive; }

    /// Progress for a load indicator: items resolved (or missed), not items
    /// still waiting. Total grows by the standalone materials the resolved
    /// entities name, which are only known once their models register.
    std::size_t TotalItems() const;
    std::size_t ProcessedItems() const;

    /// Drop all state (a replace-open cancels an in-flight build before clearing
    /// the world the held entity handles point into).
    void Reset();

  private:
    void ReleaseActiveCountWhenComplete();

    SceneResolveService m_Resolves;
    SceneResolveBatch m_Batch = SceneResolveBatch::None;
    bool m_Begun = false;
    bool m_CountedAsActive = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
