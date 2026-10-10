#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Types/ScopedSubscription.h"
#include "AssetCore/GUID.h"
#include "SceneBvh/ThreadedBvhPool.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/Core/Handle.h"
#include "SceneBvh/TlasPacker.h"
#include "SceneBvh/UberMaterial.h"

namespace GameEngine
{
struct Mesh;
class Asset;

namespace Rendering
{
class GPUScene;
class IDevice;
namespace RenderGraph
{
class RGFrame;
struct RGBuffer;
}
}  // namespace Rendering
}  // namespace GameEngine

namespace GameEngine::Engine::Renderer
{
class MaterialSystem;
class DDGIMaterialMapAtlas;

// CPU-built scene geometry for the DDGI SOFTWARE trace lane
// (Shaders/ddgi_trace_sw.comp) — the portable counterpart of
// SceneAccelerationStructureService, which serves the hardware ray-query lane.
// Where that service pools per-mesh VkAccelerationStructures and hands out
// device addresses, this one pools per-mesh SceneBvh::ThreadedBvh trees into
// four flat SSBOs plus one packed material/instance/TLAS buffer, all of which
// a plain compute shader can walk with no ray-query, no buffer_reference and
// no int64 support.
//
// The two lanes deliberately share the hardware lane's instance filter (see
// DDGIProbeFeature::DeclareProbePasses) so a scene traces the same geometry
// whichever lane is active: skinned instances are excluded (their vertices
// move after upload and nothing refits them), and so are camera-relative
// sector-tagged instances (neither lane rebases them yet).
//
// PER-INSTANCE MATERIALS. One BLAS per mesh, shared by every instance of it;
// the material a hit shades with rides on the INSTANCE record (TlasPacker.h
// slot 15) rather than on the shared BLAS's triangle table, so a mesh drawn
// twice with two different materials costs one BVH build and still shades each
// instance with its own albedo and emission — the same keying the hardware lane
// gets for free from GPUInstance.materialIndex. Uber materials are deduplicated
// by content into one table, so N instances of one material cost one row.
//
// V1 SCOPE — all real simplifications, not oversights:
//   * PARTIAL material record. Of the reference record's map slots only base
//     colour and emissive are filled (DDGIMaterialMapAtlas);
//     normal/roughness/metalness/alpha stay at -1, as they feed the reference's
//     spectral path tracer, not diffuse GI.
//   * SYNCHRONOUS rebuild. The expensive part (per-mesh BVH construction) is
//     already parallel via ThreadedBvhBuilder::BuildMany; the scheduling
//     around it is not. There is no multi-frame job/completion-token state
//     machine, because unlike the hardware lane there is no GPU command whose
//     execution has to be confirmed before the data may be dereferenced.
//   * NO refit. SceneBvh::ThreadedBvhRefit stays unconsumed: the only thing
//     that would move vertices after upload is skinning, which the instance
//     filter excludes outright.
//
// THREE UPDATE PATHS. Material values patch the existing material records via
// ordered GPU copies. Moving an object is the common case and does not touch a
// single BVH: the trees are in LOCAL space and the instance record carries the
// world transform, so a transform-only change re-packs the TLAS and re-uploads
// one buffer, reusing the pooled node/triangle/vertex buffers untouched. Only a
// change to the mesh set, a mesh's content, the material-table layout or its
// map bindings costs the full rebuild. Structure, transforms and material
// values are tracked separately (see SweepScene) and gated differently.
class DDGISceneService
{
  public:
    // `mapAtlas` supplies each material's base-colour and emissive atlas layers
    // and the uv transform they share; it must outlive this service and must be
    // refreshed before Tick each frame, since its layout epoch is one of Tick's
    // change signals.
    DDGISceneService(Rendering::IDevice* device, Rendering::MeshGPURegistry* meshRegistry,
                     Rendering::GPUScene* gpuScene, MaterialSystem* materials,
                     const DDGIMaterialMapAtlas* mapAtlas);
    ~DDGISceneService();
    DDGISceneService(const DDGISceneService&)            = delete;
    DDGISceneService& operator=(const DDGISceneService&) = delete;

    // One call per app frame. Cheap on a quiet frame: an epoch compare against
    // GPUScene's content epoch, the material content epoch and the mesh
    // registry's reload signal short-circuits everything else, exactly like
    // DDGIProbeFeature's own TLAS gate.
    //
    // When something did move, the sweep re-derives the participating set and
    // hashes it, structure and transforms separately.
    //
    // A STRUCTURAL change (mesh set, mesh content, material-table layout, or which of
    // those an instance references) restarts an idle timer and rebuilds only
    // once the observation has held still for kStructuralIdleGateMs — so
    // dragging a new object into the scene costs one BVH build when the drag
    // stops, not one per frame. Mirrors DDGIProbeFeature::kStructuralIdleGateMs,
    // same value for the same reason.
    //
    // A TRANSFORM-ONLY change is NOT idle-gated: it costs a TLAS re-pack and
    // one buffer upload, and gating it would mean a moving occluder's GI shadow
    // stays behind the occluder for as long as it keeps moving — the very case
    // the cheap path exists to serve.
    //
    // Material-value edits are also ungated. Call DeclareMaterialUploads after
    // Tick, including held-solve frames, to submit any pending record patches.
    // Returns true when a scene update was prepared; rebuilds/re-packs can
    // replace buffer handles, while material patches preserve them. Render-thread only.
    bool Tick(float deltaTimeSeconds);

    // Submit material-value patches through the frame's upload ring and an
    // ordered GPU copy, never by overwriting an in-flight buffer on the CPU.
    // The software classify/trace passes must read the same imported buffer.
    // False means upload allocation failed; keep the pending values and retry.
    bool DeclareMaterialUploads(Rendering::RenderGraph::RGFrame& frame,
                                Rendering::RenderGraph::RGBuffer packedScene);

    // Everything ddgi_trace_sw.comp needs to bind and address the scene.
    // Invalid until the first successful rebuild, and again after a rebuild
    // that found no participating geometry.
    // One skinned instance's pooled BVH range for the per-tick GPU refit —
    // matched by (SkinnedRuntimeId, GpuMeshIndex) against DDGISkinnedGeometry's
    // posed buffers. Vertex fields are pool-space record indices.
    struct SkinnedRefitRange
    {
        uint32_t SkinnedRuntimeId = 0;
        uint32_t GpuMeshIndex = 0;
        uint32_t NodeBegin = 0;
        uint32_t NodeCount = 0;
        uint32_t VertexBegin = 0;
        uint32_t VertexCount = 0;
    };
    const std::vector<SkinnedRefitRange>& GetSkinnedRefitRanges() const
    {
        return m_SkinnedRefitRanges;
    }

    struct SceneBuffers
    {
        Rendering::BufferHandle PackedScene{};  // uber materials | instances | TLAS (float)
        Rendering::BufferHandle Nodes{};
        Rendering::BufferHandle TriangleIndices{};
        Rendering::BufferHandle TriangleMaterials{};
        Rendering::BufferHandle VertexData{};

        uint64_t PackedSceneBytes      = 0;
        uint64_t NodesBytes            = 0;
        uint64_t TriangleIndicesBytes  = 0;
        uint64_t TriangleMaterialsBytes = 0;
        uint64_t VertexDataBytes       = 0;

        // Shader uSwScene lanes: TLAS node count, and the float-element
        // offsets of the instance and TLAS tables inside PackedScene.
        uint32_t TlasNodeCount = 0;
        uint32_t InstanceBase  = 0;
        uint32_t TlasBase      = 0;

        bool IsValid() const
        {
            return PackedScene.IsValid() && Nodes.IsValid() && TriangleIndices.IsValid() &&
                   TriangleMaterials.IsValid() && VertexData.IsValid() && TlasNodeCount > 0;
        }
    };
    const SceneBuffers& GetSceneBuffers() const { return m_Buffers; }

    // Instances that survived the filter AND resolved to real geometry in the
    // most recent rebuild. Zero with valid-looking buffers means every
    // candidate mesh was still streaming in.
    uint32_t GetParticipatingInstanceCount() const { return m_ParticipatingInstanceCount; }

  private:
    // One mesh the participating instances reference, resolved down to its CPU
    // geometry. The shared_ptrs are what keeps `Geometry` alive across the
    // rebuild: a ModelAsset's meshes live in the asset, and a generated mesh
    // lives in the registry entry's cpuMesh.
    struct ResolvedMesh
    {
        uint32_t GpuMeshIndex = 0;
        const GameEngine::Mesh* Geometry = nullptr;
        std::shared_ptr<GameEngine::Asset> AssetOwner;
        std::shared_ptr<const GameEngine::Mesh> CpuMeshOwner;
        // Material slot this mesh's pooled BLAS stamps onto its triangles: the
        // first participant's. Only reached by an instance that declares no
        // material of its own, which this service never emits — it is the
        // shared BLAS's honest default, not a fallback path.
        uint32_t DefaultMaterialSlot = 0;
        uint64_t ContentHash = 0;
        // Non-zero marks a per-INSTANCE slot for a skinned instance (skeleton
        // runtimeId): its pooled BVH range is refitted to the live pose each
        // solve tick (ddgi_bvh_refit.comp) instead of shared across instances.
        uint32_t SkinnedRuntimeId = 0;
    };

    struct Participant
    {
        Mathematics::Matrix4x4 WorldFromLocal{};
        uint32_t GpuMeshIndex = 0;
        uint32_t PlanMeshSlot = 0;  // index into m_PlanMeshes
        uint32_t MaterialSlot = 0;  // index into m_PlanMaterials
        // Mirrors GPUInstance flag bit 6: this instance's emissive is published
        // as a DDGI sphere-proxy light, so the software lane must exclude it
        // from per-hit emissive exactly as the hardware lane does.
        bool GIEmitter = false;
    };

    // Structure covers geometry, table layout, bindings and slot assignments.
    // Transforms tracks instance matrices; Full also includes material values.
    // Equal structure and transforms permit a material-only patch.
    struct PlanHashes
    {
        uint64_t Structure = 0;
        uint64_t Transforms = 0;
        uint64_t Full      = 0;
    };

    // Rebuilds m_PlanMeshes/m_PlanParticipants from the live scene and returns
    // its hashes. Anything that can change the built result is in Full, so a
    // mesh that finishes streaming in later moves it and re-arms the gate.
    PlanHashes SweepScene();

    // Builds every plan mesh's BVH, pools them, packs the TLAS and uploads
    // everything.
    void RebuildFromPlan();

    // The transform-only path: re-packs the TLAS and instance records against
    // the BLAS pool the last full rebuild produced and re-uploads only the
    // packed-scene buffer. Returns false when the cached pool cannot serve the
    // current plan, in which case the caller must fall back to a full rebuild.
    bool RepackInstancesFromPlan();
    bool QueueMaterialUpdatesFromPlan();

    // One TlasInstance per participant that resolved to a non-empty BLAS,
    // reading its range and local bounds out of the parallel per-plan-mesh
    // arrays a build (or the cache) supplies.
    std::vector<SceneBvh::TlasInstance> BuildTlasInstances(
        const std::vector<SceneBvh::PooledBvhRange>& ranges,
        const std::vector<Mathematics::AABB>& localBounds) const;

    // Retires every live buffer (deferred destroy) and clears m_Buffers.
    void ReleaseBuffers();
    // Retires whatever `buffer` held, then creates a fresh device-local buffer
    // of `bytes` and uploads `data` into it. Returns false (leaving the handle
    // invalid) on any failure.
    bool UploadBuffer(Rendering::BufferHandle& buffer, const void* data, uint64_t bytes,
                      const char* debugName);

    Rendering::IDevice* m_Device               = nullptr;
    Rendering::MeshGPURegistry* m_MeshRegistry = nullptr;
    Rendering::GPUScene* m_GpuScene            = nullptr;
    MaterialSystem* m_Materials                = nullptr;
    const DDGIMaterialMapAtlas* m_MapAtlas = nullptr;

    // Unsubscribes on destruction; safe even if the registry died first.
    ScopedSubscription m_ReloadSubscription;
    // Bumped by the registry's reload notification, which is required to be
    // wait-free — so the callback does nothing but this store, and the sweep
    // folds the counter into its hash.
    std::atomic<uint64_t> m_ReloadCounter{0};

    // Cheap change signals, compared before any sweep work happens.
    uint64_t m_LastSceneEpoch    = 0;
    uint64_t m_LastMaterialEpoch = 0;
    uint64_t m_LastReloadCounter = 0;
    uint64_t m_LastAtlasLayoutEpoch = 0;
    bool m_HaveEpochs            = false;

    PlanHashes m_Observed{};      // hashes of the most recent sweep
    uint64_t m_BuiltHash      = 0;  // full hash the current buffers were built from
    uint64_t m_BuiltStructure = 0;  // structure hash the cached BLAS pool was built from
    uint64_t m_BuiltTransforms = 0;
    float m_IdleTimerMs       = 0.0f;

    // Sweep output, retained between the observation and the gate firing.
    std::vector<ResolvedMesh> m_PlanMeshes;
    std::vector<SkinnedRefitRange> m_SkinnedRefitRanges;  // rebuilt with the plan
    std::vector<Participant> m_PlanParticipants;
    // Uber materials the participants shade with, deduplicated by content so a
    // scene of one material and a thousand instances packs one row.
    std::vector<SceneBvh::UberMaterial> m_PlanMaterials;
    std::vector<SceneBvh::UberMaterial> m_BuiltMaterials;
    std::vector<SceneBvh::UberMaterial> m_PendingMaterials;
    // Rebuilt every sweep: gpuMeshIndex -> index into m_PlanMeshes, or a
    // not-resolvable marker so the second instance of a mesh that is still
    // streaming in does not repeat the asset lookup.
    std::unordered_map<uint32_t, uint32_t> m_PlanMeshSlotByGpuIndex;
    // Content hash of a baked uber material -> its slot in m_PlanMaterials.
    // Multi-mapped: a bucket is scanned with an exact byte compare, so a hash
    // collision costs a comparison rather than two materials merging.
    std::unordered_multimap<uint64_t, uint32_t> m_PlanMaterialSlotByHash;

    // What the last full rebuild left behind for the transform-only path to
    // re-pack against: where each plan mesh's BLAS sits in the (unchanged)
    // node buffer, and its local-space bounds. Both are properties of the
    // TREES, which live in local space — which is exactly why moving an
    // instance cannot invalidate either. Indexed by plan mesh slot, and only
    // trustworthy while m_BuiltStructure matches the current sweep.
    std::vector<SceneBvh::PooledBvhRange> m_BuiltBlasRanges;
    std::vector<Mathematics::AABB> m_BuiltLocalBounds;

    SceneBuffers m_Buffers{};
    uint32_t m_ParticipatingInstanceCount = 0;

    // A rebuild replaces every buffer while a previously submitted trace
    // dispatch may still hold descriptors pointing at the old ones, so the
    // superseded handles are retired against a tick clock rather than
    // destroyed on the spot — same hazard and same fix as
    // DDGIProbeFeature::m_RetiredInstanceBuffers.
    struct RetiredBuffer
    {
        Rendering::BufferHandle Buffer{};
        uint64_t FrameStamp = 0;
    };
    std::vector<RetiredBuffer> m_RetiredBuffers;
    uint64_t m_FrameClock = 0;
};

}  // namespace GameEngine::Engine::Renderer
