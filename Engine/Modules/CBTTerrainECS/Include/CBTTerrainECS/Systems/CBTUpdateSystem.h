#pragma once

// CBTUpdateSystem — the ECS -> renderer bridge for CBT terrain. Each Extraction
// tick it reads the active Terrain component and pushes the active/classify state
// onto CBTRenderFeature; CBTRenderNode reads that state to record the GPU work. The
// node cannot query the ECS world (it has only RenderServices), so this system
// owns the world read. Quiescent when no live Terrain entity exists (feature goes
// inactive -> the node early-outs, no CBT passes recorded).

#include "ECS/ECS.h"     // EntityHandle (active-planet identity for the sculpt reset)
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace GameEngine::Components
{
struct Terrain;
} // namespace GameEngine::Components

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::TerrainECS
{
class TerrainRenderFeature;
} // namespace GameEngine::TerrainECS

namespace GameEngine::CBTTerrain
{
struct CBTClassifyDesc;
struct SphereEditRegions;
} // namespace GameEngine::CBTTerrain

namespace GameEngine::CBTTerrainECS
{

class CBTRenderFeature;

class CBTUpdateSystem : public ECS::ISystem
{
  public:
    explicit CBTUpdateSystem(Engine::Renderer::RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "CBTUpdateSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Fills `classify`'s dirty UV rect from CBT's cursor into the first terrain's
    // region-dirty log (E0). No-op (leaves the rect empty) when nothing new was
    // logged since CBT last reclassified — that empty rect keeps Classify quiescent.
    // `updateRecordCount` is CBTRenderFeature::GetUpdateRecordCount() this frame: the
    // cursor only COMMITS (advances past a consumed edit) once that count advances,
    // proving a CBT.Update pass recorded the pushed rect; otherwise the rect is
    // re-armed so a frame that skipped the update (no scene view / instance not ready)
    // does not silently drop the edit. Public so it is directly unit-testable with an
    // ECS world + a real TerrainService terrain (no RenderServices needed).
    void ConsumeDirtyRegion(ECS::World& world, uint64 updateRecordCount,
                            CBTTerrain::CBTClassifyDesc& classify);

    // Spherical analogue of ConsumeDirtyRegion (plan §planet-editing): drives Classify's
    // (DirtyFace + face-local UV rect) reclassification from an edit's touched regions. The GPU
    // Classify push carries ONE (face,rect); a dab or modifier near a cube edge touches two faces
    // (three at a corner), and EVERY touched face's bisectors must re-evaluate or the neighbour
    // face keeps its pre-edit heights (a cross-face seam that heals only when the exact edge is
    // brushed again). So each touched face is accumulated into a per-face dirty set (union) and
    // DRAINED one face per recorded CBT.Update, round-robin — the neighbour face reclassifies on
    // the very next update, no re-brush needed. `regions` is the clear-on-read drain from
    // TerrainService::ConsumeSphereSculptDirtyRegions (the UNION of every edit since the last
    // frame, so a fast stroke's several dabs are all ingested, not just the last); every non-empty
    // call is new content and is ingested. The same update-record confirmation as the planar path
    // re-arms a face whose update did not record. `sphereVersion` only drives SphereSculptEnabled
    // (turns on for good once any edit lands, gating the VertexEval sample). Public for unit tests.
    void ConsumeSphereSculpt(uint64 sphereVersion, uint64 updateRecordCount,
                             const CBTTerrain::SphereEditRegions& regions,
                             CBTTerrain::CBTClassifyDesc& classify);

    // Tiled-streaming analogue of ConsumeDirtyRegion: applies TerrainRenderFeature's
    // unified-height dirty rect (streamed / re-baked tile uploads) to `classify` with the
    // same in-flight / record-count confirm discipline. `takenHasRect` + the rect are this
    // frame's drained accumulator (TakeUnifiedHeightDirtyRect result); the consumer unions
    // unconfirmed rects and re-arms until a CBT.Update records them, so a rect drained on a
    // frame whose update never ran (no scene view / instance not ready) is not dropped.
    // A tiled terrain zeroes its heightfield DirtyRegionLog handle, so the log-based
    // ConsumeDirtyRegion is inert for it — this feeds the same channel from the GPU
    // texture side instead. Public so it is directly unit-testable (no RenderServices).
    void ConsumeUnifiedDirtyRect(bool takenHasRect, float minU, float minV, float maxU,
                                 float maxV, uint64 updateRecordCount,
                                 CBTTerrain::CBTClassifyDesc& classify);

    // Update()'s tiled-planar routing: resolves the active tiled terrain's global GPU
    // handle (the key its unified textures + region uploads use) from `tuning`'s tiled
    // handle via TerrainService, drains that terrain's unified-height dirty rect from
    // `feature`, and feeds it to ConsumeUnifiedDirtyRect. `feature` is injected (Update()
    // passes RenderServices' TerrainRenderFeature) so this is directly unit-testable with a
    // real feature + TerrainService tiled terrain; a null feature / missing tiled data runs
    // the discipline with nothing drained (a pending rect still re-arms / commits).
    void ConsumeUnifiedTiledDirtyRegion(TerrainECS::TerrainRenderFeature* feature,
                                        uint64 updateRecordCount,
                                        const Components::Terrain& tuning,
                                        CBTTerrain::CBTClassifyDesc& classify);

    // True when any enabled Terrain entity is live — the gate that makes CBT (the
    // only terrain renderer) active and lets CBTRenderNode emit its draw. A terrain
    // is live when EITHER its single-terrain handle OR its tiled handle is set:
    // extraction ZEROES TerrainDataHandle and sets TiledTerrainHandle when a terrain
    // auto-tiles, so a single-handle-only test leaves a tiled terrain inactive and
    // unrendered. Public + static so it is directly unit-testable (no RenderServices).
    static bool ComputeTerrainActive(ECS::World& world);

  private:
    // Emits the one-shot Terrain.Provision signal (domain + radius/size + derived
    // MaxDepth) once a config settles, silent otherwise (debounces inspector drags).
    void LogProvisionSignal(const Components::Terrain& tuning, uint32 resolvedMaxDepth);

    Engine::Renderer::RenderServices* m_RenderServices = nullptr;

    // Provision-signal debounce keys (hash of domain + radius/size + depth). A real
    // key is always >= 2^33, so these small distinct sentinels never collide: Settled
    // = last key logged at Info; Pending = last key seen (logs once seen twice = held).
    uint64 m_ProvisionSettledKey = 1u;
    uint64 m_ProvisionPendingKey = 2u;

    // CBT's OWN cursor into each terrain's HeightfieldDirtyLog (the E0 per-consumer
    // model). Keyed by TerrainData slot index; a generation mismatch (slot reused)
    // resets it so the log is re-read from the CreateTerrain full-dirty entry.
    // CollectSince never clears the log, so this never starves extraction or physics
    // (their cursors are separate).
    struct RegionCursor
    {
        uint32 Generation = 0;
        // Highest log version CBT has CONFIRMED reclassified (the render side recorded
        // the pushed rect). CollectSince re-collects everything newer than this, so
        // holding it back until confirmation re-arms an unconsumed edit automatically.
        uint64 CommittedVersion = 0;
        // A rect has been pushed but not yet confirmed recorded.
        bool InFlight = false;
        // HeightfieldVersion of the pushed-but-unconfirmed rect (committed on confirm).
        uint64 InFlightVersion = 0;
        // Update-record count observed when the rect was pushed; confirmation = the
        // feature's count has moved past this.
        uint64 InFlightRecordCount = 0;
    };
    std::unordered_map<uint32, RegionCursor> m_RegionCursors;

    // Sphere sculpt cursor (plan §planet-editing) — the per-face analogue of the planar
    // RegionCursor. An edit near a cube edge dirties two faces (three at a corner); the GPU
    // Classify push reclassifies one face at a time, so the touched faces are accumulated here
    // (per-face rect union) and drained one per recorded CBT.Update, round-robin. The neighbour
    // face reclassifies on the next update — no re-brush — closing the cross-face seam. Each face
    // is re-armed until an update records its rect (the no-scene-view guard), and a face whose
    // pushed rect grew before it was recorded is re-pushed at the grown extent, not dropped.
    static constexpr uint32 kSphereFaceCount = 6u; // the six cube faces (== CBTTerrain::kCubeFaceCount)

    struct SphereFaceDirty
    {
        bool Pending = false; // needs reclassification (union since the face was last recorded)
        float MinU = 0.0f, MinV = 0.0f, MaxU = 0.0f, MaxV = 0.0f;
    };
    std::array<SphereFaceDirty, kSphereFaceCount> m_SphereFaces{};
    int32 m_SphereInFlightFace = -1;     // face pushed but not yet confirmed recorded (-1 = none)
    uint64 m_SphereInFlightRecordCount = 0;
    float m_SphereInFlightMinU = 0.0f, m_SphereInFlightMinV = 0.0f; // snapshot of the pushed rect,
    float m_SphereInFlightMaxU = 0.0f, m_SphereInFlightMaxV = 0.0f; // to detect growth before record
    uint32 m_SphereDrainCursor = 0;      // round-robin start so no face starves under a fast drag

    // Force a full-pool VertexEval refresh while a planet's editable sculpt is actively changing.
    // The region-scoped MODIFIED re-eval (the dirty rect above) only re-evaluates the bisectors the
    // rect covers THIS frame. A modifier dragged across the surface bumps the sculpt version at a new
    // footprint every frame while the per-frame dirty rect trails behind the motion, so the bisectors
    // the modifier already passed were never re-evaluated and kept their stale (flattened) height —
    // the round-8 flatten-move cracks (a persistent sliver-crack trail behind the disc). While the
    // sculpt version advances, and for a short settle after it stops, force the whole live pool to
    // re-evaluate against the current sculpt so every bisector matches the edited height and no stale
    // trail survives. Fires ONLY while a planet is being edited: a quiescent planet keeps the gated
    // region-scoped re-eval, so the idle CBT.Update cost is byte-identical (the quiescence gate).
    uint64 m_LastSphereSculptVersion = 0;
    uint32 m_SphereForceEvalFrames = 0;

    // Identity of the active spherical planet last seen. When it changes to a DIFFERENT planet or to
    // none (the planet was deleted, or its domain switched away from Spherical), the service-global
    // sculpt layer is RESET so the next planet re-derives fresh geometry (Dv from ITS radius) with an
    // empty pool — instead of inheriting the deleted planet's frozen dim + stale dab content (the
    // create->edit->delete->recreate flow that resurrects "brush does nothing at 20k"). Invalid until
    // a spherical planet is first seen; a same-entity persist keeps the frozen geometry (by-design).
    ECS::EntityHandle m_ActivePlanetEntity{};

    // Tiled-streaming dirty-rect cursor (ConsumeUnifiedDirtyRect). TerrainRenderFeature's
    // accumulator is drained destructively each frame, so the consumer holds the union of
    // taken-but-unconfirmed rects here and re-arms it until a CBT.Update records it. Same
    // confirm model as the sphere cursor: commit (drop the pending union) once the render
    // record count moves past what we saw when we pushed.
    bool m_UnifiedInFlight = false;
    uint64 m_UnifiedInFlightRecordCount = 0;
    bool m_UnifiedPendingHasRect = false;
    float m_UnifiedPendingMinU = 0.0f;
    float m_UnifiedPendingMinV = 0.0f;
    float m_UnifiedPendingMaxU = 0.0f;
    float m_UnifiedPendingMaxV = 0.0f;
};

} // namespace GameEngine::CBTTerrainECS
