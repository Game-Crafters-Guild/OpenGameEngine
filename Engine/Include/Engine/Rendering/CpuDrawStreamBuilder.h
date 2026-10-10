// CpuDrawStreamBuilder: the compatibility profile's CPU twin of the GPU
// scatter's per-slice instance stream.

#pragma once

#include "Rendering/CameraTypes.h"
#include "Types/Types.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
namespace Rendering
{
class GPUScene;
class MeshGPURegistry;
} // namespace Rendering

namespace Engine::Renderer
{
class ViewRegistry;
class WorldDrawBuilder;

// A device without buffer_device_address cannot reach the GPU scatter's
// indirection buffer, so the compatibility profile resolves the visible set on
// the CPU instead: frustum culling, batch keying and the instance lists all
// happen here, once per frame, and the colour/depth recorders replay the
// result as direct instanced draws.
//
// Each view owns one instance index list: for every (materialIndex,
// meshIndex) batch and instance set, the batch's surviving GPUScene instance
// indices sit contiguously in it, the CPU equivalent of the scatter's
// indirection records. The world pass uploads the list once per (frame, view)
// and binds it as the set-0 SSBO `CompatInstanceList`; the vertex stage reads
// `GPUInstances[CompatInstanceList[first + gl_InstanceIndex]]`
// (instance_io.glsl's compat branch). A batch is therefore ONE DrawIndexed
// with instanceCount = Count and one push-constant write carrying `First`,
// however its GPUScene indices are spread.
//
// Keys match WorldDrawBuilder::BatchKey identity exactly: under the compat
// profile the colour-class merge and the pool-group consolidation are both off,
// so colorClassId == materialIndex and the key is (materialIndex, meshIndex).
class CpuDrawStreamBuilder
{
  public:
    // The set-0 SSBO's reflected instance name for the uploaded index list
    // (instance_io.glsl's compat branch declares it).
    static constexpr const char* kIndexListBindingName = "CompatInstanceList";

    // One instanced draw: `Count` instances whose GPUScene indices are the
    // view's index list entries [First, First + Count).
    struct InstanceList
    {
        uint32_t First = 0;
        uint32_t Count = 0;
    };

    // Which consumer an instance set serves. The two differ in their filter,
    // not their keying.
    enum class InstanceSet : uint8_t
    {
        // Colour pass + main-view depth prepass: culled against the view
        // frustum.
        Camera = 0,
        // Shadow families (cascade / area / spot / point): cast-shadow
        // instances only, NOT camera-culled — a caster outside the camera
        // frustum still casts into it. Culling these against their own light
        // frustums needs the per-slice light view-projections, which are
        // produced later in the frame than this build; leaving them unculled
        // over-draws, which is correct.
        ShadowCasters = 1,
    };

    // Rebuild every view's lists from the frame's merged submissions. `pool`
    // may be null (tests, single-threaded configs) — the per-instance work
    // then runs inline on the calling thread. Must be called from a
    // non-worker thread: the culling fan-out is a blocking ParallelFor.
    void Build(const WorldDrawBuilder& drawBuilder,
               const Rendering::MeshGPURegistry& meshRegistry,
               const Rendering::GPUScene& scene,
               const ViewRegistry& views,
               JobSystem::WorkStealingThreadPool* pool);

    // Rebuild ONE view's lists, leaving every other view's untouched. The
    // editor's one-shot views (thumbnails, previews) declare after the frame's
    // main build, and a full rebuild there would reshape the scene views'
    // streams mid-frame. Must run before that view's passes declare: the
    // uploaded list and the recorded batches have to come from one build.
    void BuildForView(Rendering::ViewId viewId, const WorldDrawBuilder& drawBuilder,
                      const Rendering::MeshGPURegistry& meshRegistry,
                      const Rendering::GPUScene& scene, const ViewRegistry& views,
                      JobSystem::WorkStealingThreadPool* pool);

    // The instances of one batch of one view. Count 0 when the batch has no
    // surviving instance in that set — the caller skips the draw entirely.
    InstanceList GetInstances(Rendering::ViewId viewId, uint32_t materialIndex,
                              uint32_t meshIndex, InstanceSet set) const;

    // The view's whole index list (every batch, both sets): what the world
    // pass uploads as `CompatInstanceList`. Empty when the view draws nothing.
    std::span<const uint32_t> GetIndexList(Rendering::ViewId viewId) const;

    // Per-frame totals over every view, for the render-stats readout.
    struct Stats
    {
        uint32_t Candidates = 0;     // submissions that resolved to a batch
        uint32_t CameraVisible = 0;  // survivors of the frustum test
        uint32_t CameraBatches = 0;  // draws a camera-set pass issues
        uint32_t ShadowCasters = 0;
        uint32_t ShadowBatches = 0;  // draws a shadow pass issues
    };
    const Stats& GetStats() const { return m_Stats; }

    void Clear();

  private:
    // Per-submission scratch, filled in parallel and compacted serially. Key is
    // (materialIndex << 32 | meshIndex); Instance indexes GPUScene.
    struct Candidate
    {
        uint64_t Key;
        uint32_t Instance;
        uint8_t State; // CandidateState
    };

    struct KeyedInstance
    {
        uint64_t Key;
        uint32_t Instance;
    };

    struct ViewLists
    {
        std::vector<uint32_t> Indices;
        std::unordered_map<uint64_t, InstanceList> Index[2];
    };

    void BuildView(Rendering::ViewId viewId, const WorldDrawBuilder& drawBuilder,
                   const Rendering::MeshGPURegistry& meshRegistry,
                   const Rendering::GPUScene& scene, const ViewRegistry& views,
                   JobSystem::WorkStealingThreadPool* pool);

    // Sort `entries` by (Key, Instance), append each key's distinct instances
    // to `view.Indices` and index the key's span under `set`. Returns the
    // number of batches appended.
    uint32_t EmitLists(ViewLists& view, InstanceSet set, std::vector<KeyedInstance>& entries,
                       JobSystem::WorkStealingThreadPool* pool);

    std::unordered_map<Rendering::ViewId, ViewLists> m_ByView;

    // Frame-persistent scratch: sized once, reused every frame so a steady
    // scene allocates nothing in the build.
    std::vector<Candidate> m_Candidates;
    std::vector<KeyedInstance> m_CameraEntries;
    std::vector<KeyedInstance> m_ShadowEntries;

    Stats m_Stats{};
};

} // namespace Engine::Renderer
} // namespace GameEngine
