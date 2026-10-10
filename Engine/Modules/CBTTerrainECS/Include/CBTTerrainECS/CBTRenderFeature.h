#pragma once

// CBTRenderFeature — owns the single C3 CBT GPU instance (compute kernels +
// persistent buffers + material) and the per-frame drive state that CBTUpdateSystem
// pushes from the ECS world. CBTRenderNode records the update compute pass and the
// indexed-indirect forward draw from this feature each frame. Lazily initialized on
// the render thread (first DeclareForView), mirroring OceanRenderFeature.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "AssetCore/GUID.h"
#include "CBTTerrain/CBTActivityReadback.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/CBTUpdateRestGate.h"
#include "CBTTerrainECS/CBTTreeSeed.h"
#include "CBTTerrainECS/CBTValidationReadback.h"
#include "CBTTerrainECS/TerrainShadowBake.h"
#include "Components/Terrain/Terrain.h" // kNoTerrainSeaLevel
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Rendering
{
class IDevice;
using ViewId = uint32_t;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
class Material;
struct DrawCommand;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::CBTTerrainECS
{

class CBTRenderFeature final : public Engine::Renderer::IRenderFeature
{
  public:
    CBTRenderFeature() = default;
    ~CBTRenderFeature() override;

    // Lazy one-time GPU bring-up: load kernels from the staged CBT shader dir and
    // request their pipelines, then, on the first call that finds them all built,
    // allocate the persistent buffers and upload the roots + draw resources. Safe to
    // call every frame; a no-op once ready. Returns false while the kernel pipelines
    // are still building (the feature declares nothing meanwhile), and false for good
    // if the compiled kernels are missing or a kernel pipeline fails.
    bool EnsureInitialized(Rendering::IDevice& device, Engine::Renderer::RenderServices& rs);
    bool IsInitialized() const { return m_Initialized; }

    // Device-lost recovery. An in-place device rebuild destroys the 20 kernel pipelines
    // and every persistent buffer/texture/sampler the instance owns, while the cached
    // handles keep reading IsValid() — the next DispatchDirect would then SetPipeline a
    // dead handle (a silently skipped bind) and dispatch with nothing bound. Forget the
    // dead state and re-arm the lazy bring-up so the next EnsureInitialized re-creates it.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

    // The rendering domain + planet tuning CBTUpdateSystem pushes from the Terrain
    // component. DomainMode is kDomainPlanar / kDomainSpherical (CBTLayout.h).
    struct DomainConfig
    {
        uint32_t DomainMode = CBTTerrain::kDomainPlanar;
        float PlanetRadius = 2000.0f;
        float ReliefAmplitude = 60.0f;
        float ReliefFrequency = 6.0f;
        uint32_t ReliefOctaves = 4u; // fBM octaves for the sphere relief (matches TerrainPlanetRelief default)
        // Domain SHAPE only. A terrain's materials do not travel this path: both domains read the
        // terrain material table (TerrainRenderFeature), which TerrainExtractionSystem authors from
        // the same component this config is built from.
    };

    // Pushed by CBTUpdateSystem from the ECS world each frame. `classify` selects the
    // metric (screen-space in production; TargetDepth carries the max-depth cap);
    // `targetPixelError` is the authored split target (px at 1080 rows; BuildFrameParams scales it
    // to the view's render height through CBTTerrain::SplitThresholdPixels); `terrainSeaLevel` is the
    // Terrain component's authored water surface height (Components::kNoTerrainSeaLevel =
    // none; an Ocean module surface overrides it — see BuildFrameParams); `domain` picks
    // planar vs the spherical cube-sphere planet (plan §8 C7).
    void SetActive(bool active, const CBTTerrain::CBTClassifyDesc& classify, float targetPixelError,
                   float terrainSeaLevel, const DomainConfig& domain, uint32_t debugView)
    {
        // A drive change or an edited region makes this frame's update run, which the depth-derived
        // elision readers learn from WritesDynamicDepth before the update is declared.
        const bool edited = classify.DirtyMaxU > classify.DirtyMinU && classify.DirtyMaxV > classify.DirtyMinV;
        if (edited || active != m_Active ||
            std::memcmp(&classify, &m_Classify, sizeof(classify)) != 0 ||
            targetPixelError != m_TargetPixelError || terrainSeaLevel != m_TerrainSeaLevel ||
            std::memcmp(&domain, &m_DomainConfig, sizeof(domain)) != 0)
            m_UpdateAtRest.store(false, std::memory_order_relaxed);
        m_Active = active;
        m_Classify = classify;
        m_TargetPixelError = targetPixelError;
        m_TerrainSeaLevel = terrainSeaLevel;
        m_DebugView = debugView;
        // Force a full-pool VertexEval on the next recorded update when the planet's shape
        // params (radius / relief / octaves / domain) changed: those alter every bisector's
        // world corners, but none are flagged MODIFIED, so the quiescence gate would otherwise
        // leave stale corners until the region re-splits (plan §planet-shading perf).
        if (domain.DomainMode != m_DomainConfig.DomainMode ||
            domain.PlanetRadius != m_DomainConfig.PlanetRadius ||
            domain.ReliefAmplitude != m_DomainConfig.ReliefAmplitude ||
            domain.ReliefFrequency != m_DomainConfig.ReliefFrequency ||
            domain.ReliefOctaves != m_DomainConfig.ReliefOctaves)
            m_ForceVertexEval.store(true, std::memory_order_relaxed);
        m_DomainConfig = domain;
    }

    // True once (clearing the flag) when a full-pool VertexEval is due — after init, a tree
    // re-seed, or a planet-param change. CBTRenderNode reads it to set GateVertexEval on the
    // update it records: 0 (force full) when pending, 1 (quiescence gate) otherwise. Consumed
    // only at record time so a change that lands on a non-recording frame is not lost.
    bool ConsumeForceVertexEval() { return m_ForceVertexEval.exchange(false, std::memory_order_relaxed); }

    // Debug discriminator (cbt_force_vertex_eval IPC): force ONE full-pool VertexEval on
    // the next recorded update, re-sampling every live bisector against the current height
    // texture. On a corrupted parked scene this heals iff the corruption is stale cached
    // corners (the streaming re-eval bug) rather than texture corruption. Un-serialized,
    // zero-cost until fired (just sets the flag ConsumeForceVertexEval already reads).
    void RequestForceVertexEval()
    {
        m_ForceVertexEval.store(true, std::memory_order_relaxed);
        m_UpdateAtRest.store(false, std::memory_order_relaxed);
    }
    bool IsActive() const { return m_Active; }
    // The terrain's depth can change unless its update is at rest (IsUpdateAtRest): a tree that
    // splits or merges, and a sculpt or streamed height edit, change depth with a static camera
    // and no other elision key moving. Read by the frame spine before this frame's update is
    // decided, so besides the last decision it reports every change that will make this frame's
    // update run: one SetActive or RequestForceVertexEval announced, a pending full re-evaluation,
    // a held tree restart, and a terrain retire or a new height source the node has not consumed
    // yet (read live from TerrainRenderFeature). A camera or render-extent change is not reported:
    // every depth-derived reader keys on those itself.
    bool WritesDynamicDepth() const override;

    // Folds the newest landed update reading (CBTActivityReadback) into the rest gate. Called by
    // CBTRenderNode once per frame, before it decides on the update.
    void ConsumeUpdateActivity();
    // True when this frame's CBT update can be skipped: its inputs (`params` from BuildFrameParams,
    // the classify, the bound heightmap) equal the previous update's, the updates since they last
    // changed left the tree at rest (CBTUpdateRestGate), and neither an edited region nor a full
    // vertex re-evaluation is pending. The draw then reads the buffers the last update wrote.
    bool IsUpdateAtRest(const CBTTerrain::CBTFrameParams& params);
    // For the update about to be declared into `frame`: its readback slot (invalid = unread).
    // `forced` = the update re-evaluates every vertex (GateVertexEval 0).
    CBTTerrain::CBTActivitySlot BeginUpdateActivityReadback(const Rendering::RenderGraph::RGFrame& frame,
                                                            bool forced);

    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override
    {
        m_ActivityReadback.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override
    {
        m_ActivityReadback.OnFrameStreamRetired(frame);
    }
    const CBTTerrain::CBTClassifyDesc& GetClassify() const { return m_Classify; }
    const DomainConfig& GetDomainConfig() const { return m_DomainConfig; }
    float GetTargetPixelError() const { return m_TargetPixelError; }
    // The split threshold (render pixels) and the render height BuildFrameParams applied on its last
    // frame: the authored target scaled to that height. Both 0 before the first frame.
    float GetAppliedSplitThresholdPx() const { return m_AppliedSplitThresholdPx; }
    uint32_t GetAppliedRenderHeightPx() const { return m_AppliedRenderHeightPx; }

    CBTTerrain::CBTInstance& GetInstance() { return m_Instance; }
    CBTValidationReadback& GetValidationReadback() { return m_ValidationReadback; }

    // Planet editing (plan §planet-editing): the AUTHORITATIVE editable sculpt layer (dab +
    // baked terrain-modifier stack) now lives in TerrainService — the two writers (editor
    // brush + TerrainModifierSystem) span modules the feature cannot reach. The feature only
    // UPLOADS it: BuildFrameParams reads TerrainService's version + atlas into this frame's
    // ring slot (gated). CBTUpdateSystem reads TerrainService's version + dirty (face,rect) to
    // drive Classify; the physics colliders read TerrainService's mirror directly.

    // The CPU-authoritative planet surface height (metres above PlanetRadius) at a world
    // direction: procedural relief (from the active planet tuning) + TerrainService's editable
    // sculpt layer — the SAME height the GPU VertexEval displaces to. The editor brush refines
    // its ray->planet hit against this so the cursor tracks tall relief / sculpt instead of
    // drifting on the analytic base sphere (#488 follow-up). `dir` need not be unit.
    float SampleSphereSurfaceHeight(float dx, float dy, float dz) const;

    // Restarts the tree from its roots when it no longer matches what it tessellates: the desired
    // domain (from the Terrain component) differs from the seeded one, a terrain was retired since
    // the seed (ConsumeTerrainRetire), or the depth cap fell below the deepest cap the tree was
    // allowed (CBTTreeSeed). The first request restarts at once; repeats within CBTTreeSeed's
    // window wait until they settle, so an inspector drag restarts the tree at its first step
    // and once when it ends. A restarted tree is refined to CBTTreeSeed::kRestartSeedDepth (or the
    // cap) before its first draw, so no frame draws the bare roots. The re-seed also zeroes the work-queue
    // counters, the overflow total included. `frameCounter` is this frame's value from
    // CBTInstance::AdvanceFrameCounter. Called render-side after EnsureInitialized and
    // ConsumeTerrainRetire (owns the device + command lists). No-op when nothing changed. Logs a
    // GE_CBT.Planet line on every (re)seed so a verify can grep the active domain, the roots and
    // the reason.
    void EnsureSeeded(uint32_t frameCounter);

    // In-place re-provision safety. A SamplesPerMeter/size edit that preserves the runtime tiled
    // handle takes the DEBOUNCED re-provision path: ReleaseTerrainResources retires the old unified
    // heightmap (deferred-destroy quarantine) and recreates it at the new resolution. CBT samples
    // that heightmap through its own descriptor ring (bindings 15/18/19), which the per-slot lazy
    // SetHeightSource only refreshes one element per frame — so a retired image can be freed while a
    // ring element still references it (device-lost in VertexEval a few frames later). This polls the
    // terrain feature's retire generation and, on a bump, drains the GPU and rebinds every ring
    // element to its default in the SAME update the re-provision commits (RefreshTerrainSources),
    // then forces a full VertexEval since the new heightmap staled every cached bisector corner.
    // A retire also means the tree grew on a terrain that is gone, so it marks the tree for a
    // restart (EnsureSeeded). Called render-side before EnsureSeeded and BuildFrameParams. No-op
    // when nothing retired.
    void ConsumeTerrainRetire(uint64_t retireGeneration);

    // Gathers this frame's camera (from the view) + terrain params (the first active
    // terrain from TerrainRenderFeature — the SAME heightmap/world-size CDLOD uses)
    // into `out` and binds that heightmap into this frame's height-source ring element
    // (CBTFrameRingSlot(frameIndex) — matches what VertexEval samples this frame; the
    // caller passes the unwrapped counter from CBTInstance::AdvanceFrameCounter, never the
    // device's wrapped frame slot). Returns false
    // when there is no active terrain, in which case the ring slot is reset to the
    // flat default (so no dangling descriptor) and CBT renders nothing this frame; a
    // terrain WITHOUT an uploaded heightmap yet still returns true (the flat-default
    // fallback is intended — CBT draws flat until the upload completes). Called
    // render-side (camera + TerrainRenderFeature are render-thread state).
    bool BuildFrameParams(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId,
                          uint32_t renderWidth, uint32_t renderHeight, uint32_t frameIndex,
                          CBTTerrain::CBTFrameParams& out);

    // The real terrain heightmap bound by the most recent BuildFrameParams, or an
    // invalid handle when the flat 1x1 default is in use (no terrain / heightmap not
    // uploaded yet). CBTRenderNode forwards this to CBTUpdateNode so the update pass
    // can declare its read edge on the texture VertexEval samples (plan §8 C5).
    Rendering::TextureHandle GetBoundHeightmap() const { return m_BoundHeightmap; }
    // The terrain's shadow source set by the most recent BuildFrameParams (no height texture when
    // none is bound or the terrain is atlas-backed), and the bake that turns it into the map.
    const TerrainShadowSource& GetTerrainShadowSource() const { return m_TerrainShadowSource; }
    TerrainShadowBake& GetTerrainShadowBake() { return m_TerrainShadowBake; }

    // Draw/producer inseparability (the fix for the entity-deletion device-lost):
    // the CBT forward draw consumes the CBT.Update compute pass's output, so it may
    // ONLY be emitted into the SAME render graph where that update ran this frame.
    // Keyed on the (graph, RGFrame::FrameIndex()) pair — monotonic + stale-safe, so
    // a second graph in the same engine frame (an extra extraction/preview execute
    // that a deletion can spin up) does NOT re-run the global sim and, crucially,
    // must NOT emit an orphaned draw reading buffers no pass produced in its graph.
    //
    // ClaimUpdate: true iff THIS graph is the first to claim the once-per-frame
    // update (then the caller declares it here, or, at rest, declares nothing and
    // draws the buffers the last update wrote); false iff another graph already
    // claimed it this frame (then the caller must refuse the draw).
    bool ClaimUpdate(const void* graph, uint64_t rgFrameIndex);
    bool UpdateDeclaredInGraph(const void* graph, uint64_t rgFrameIndex) const
    {
        return m_UpdateGraph == graph && m_UpdateRgFrame == rgFrameIndex;
    }

    // Monotonic count of update passes actually declared into a graph (bumped by
    // CBTRenderNode on each ClaimUpdate that records the CBT.Update pass). The
    // render-side confirmation signal the extraction-side dirty-region cursor reads:
    // CBTUpdateSystem only commits its log cursor once this count advances past the
    // value it saw when it pushed a dirty rect, so an edit pushed on a frame whose
    // update never recorded (no scene view, instance not ready) is re-armed next
    // frame instead of silently dropped (plan §8 C5). Cross-thread (render writes,
    // extraction reads); relaxed atomics — only inequality across frames matters, and
    // a stale read self-corrects by re-arming one more frame.
    void NotifyUpdateRecorded() { m_UpdateRecordCount.fetch_add(1u, std::memory_order_relaxed); }
    uint64_t GetUpdateRecordCount() const { return m_UpdateRecordCount.load(std::memory_order_relaxed); }

    // Builds the indexed-indirect CBT DrawCommand for `viewId`. Returns false without
    // logging while the material's base compile is in flight (the terrain draws once
    // its pipeline publishes), and false loudly (log + no draw) if the material's
    // graphics variant failed to compile (DescriptorSetLayouts empty), the POC's
    // silent-null-variant trap (plan §9).
    // The DrawBindings span points into feature-owned storage stable for the frame.
    bool BuildDrawCommand(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId,
                          Rendering::MaterialKeyword passKeywords,
                          Engine::Renderer::DrawCommand& outCmd);

    // Builds the camera-prepass head of `colour` (BuildDrawCommand's draw for `viewId`): the same
    // VISIBLE-stream record, identity index buffer and set-2 buffers, drawn with the material's
    // depth-only variant, which runs the same vertex modifier with no fragment stage. Returns false
    // until the device has built that variant's pipeline for the view's prepass (requested on the
    // first miss; never, if the build failed); the colour draw then writes its own depth.
    bool BuildPrepassHead(Engine::Renderer::RenderServices& rs, Rendering::ViewId viewId,
                          const Engine::Renderer::DrawCommand& colour,
                          Engine::Renderer::DrawCommand& outHead) const;

    // Whether the loaded kernels carry the narrow (u32) heap, which caps the decode depth
    // at the heap's own range (TerrainProvisioning). False until the kernels are loaded.
    bool UsesNarrowHeap() const { return m_KernelSet.IsNarrowHeap(); }

  private:
    // Loads the kernel programs and requests their pipelines (EnsureInitialized's
    // first step). False when the programs are missing.
    bool RequestKernels(Rendering::IDevice& device);
    bool EnsureMaterial(Engine::Renderer::RenderServices& rs);
    // True when TerrainRenderFeature has retired a texture set since ConsumeTerrainRetire last ran,
    // or the height source BuildFrameParams would bind differs from the one it last bound.
    bool TerrainSourceChanged() const;
    // The height texture the update samples for a terrain: its atlas when atlas-backed, else its
    // unified heightmap (invalid until uploaded).
    static Rendering::TextureHandle BoundHeightSource(bool atlasBacked, Rendering::TextureHandle atlas,
                                                      Rendering::TextureHandle heightmap);
    // The height page cache the update samples for terrain `terrainIndex` when its height is paged
    // (TerrainHeightPageFeature latched its page table and created the cache) and these kernels
    // read pages (the wide arm); invalid otherwise. Its page-table words go to `words`.
    Rendering::TextureHandle PagedHeightSource(uint32_t terrainIndex, std::span<const uint32_t>& words,
                                               uint64_t& version) const;
    // Logs "Terrain pages: the CBT draws terrain <index> (generation <g>) from its pages|texture
    // from frame <frameIndex>" when the committed source differs from the last one logged.
    void LogCommittedHeightSource(uint32_t terrainIndex, uint32_t generation, bool paged, uint32_t frameIndex);

    CBTTerrain::CBTKernelSet m_KernelSet;
    CBTTerrain::CBTInstance m_Instance;
    // The update skip at rest (IsUpdateAtRest): the gate, the readback that feeds it, the input
    // bytes compared each frame (a member so the comparison allocates nothing), and the last answer.
    CBTTerrain::CBTUpdateRestGate m_RestGate;
    CBTTerrain::CBTActivityReadback m_ActivityReadback;
    std::vector<uint8_t> m_UpdateInputs;
    std::atomic<bool> m_UpdateAtRest{false};
    // GE_CBT_VALIDATE's counter readback; destroyed with the feature, before the device.
    CBTValidationReadback m_ValidationReadback;
    Engine::Renderer::Material* m_Material = nullptr;
    // Set-2 SSBO bindings resolved by MaterialBinder by reflected instance name.
    // The CBT buffer handles are persistent, so this is built once and its span
    // stays valid every frame (no per-view scratch needed).
    std::vector<Engine::Renderer::DrawBindings::BufferEntry> m_DrawBuffers;
    // Compat only: cbt_surface declares its terrain maps as named set-2 bindings there, because
    // WebGPU exposes no binding array to index. Empty on the bindless profile.
    std::vector<Engine::Renderer::DrawBindings::TextureEntry> m_DrawTextures;
    CBTTerrain::CBTClassifyDesc m_Classify{};
    // Planet-editing sculpt upload gate (v2): the authoritative paged sculpt store lives in
    // TerrainService (dab + baked modifier stack, shared across modules); the feature only uploads
    // it. The gate refreshes each ring slot once per sculpt version so an idle edited planet uploads
    // nothing; the scratches hold the copied page pool + page table for that upload.
    CBTTerrain::SphereSculptUploadGate m_SphereSculptUploadGate;
    std::vector<float> m_SphereSculptScratch;
    std::vector<uint32_t> m_SphereSculptTableScratch;
    float m_TargetPixelError = Components::kDefaultTerrainTargetPixelError;
    float m_AppliedSplitThresholdPx = 0.0f;
    uint32_t m_AppliedRenderHeightPx = 0u;
    // The Terrain component's authored water surface height (Components::kNoTerrainSeaLevel =
    // none). BuildFrameParams resolves it against the Ocean module's surface.
    float m_TerrainSeaLevel = Components::kNoTerrainSeaLevel;
    uint32_t m_DebugView = 0u;            // TerrainDebugView (from Terrain) -> surface DebugMode
    uint32_t m_LoggedDebugView = 0u;      // last-logged value for the one-shot toggle signal
    DomainConfig m_DomainConfig{};       // desired domain + planet tuning (from Terrain)
    CBTTreeSeed m_TreeSeed; // what the instance's tree was seeded for (domain, cap, retire)
    // Last terrain-texture retire generation consumed (see ConsumeTerrainRetire). Synced to the
    // terrain feature's live value on first sight so a re-provision that happened before CBT went
    // active does not trigger a spurious full-ring rebind; thereafter a bump drives one refresh.
    uint64_t m_LastTerrainRetireGeneration = 0;
    bool m_TerrainRetireGenerationSynced = false;
    // This frame's ring element (CBTFrameRingSlot of the unwrapped frame counter), stashed
    // by BuildFrameParams so BuildDrawCommand can offset-bind the per-slot graphics buffers
    // (surface params + sculpt atlas).
    uint32_t m_FrameSlot = 0u;
    // Phase E: last-uploaded indirection-table version per ring slot. The atlas rows are
    // re-uploaded to a ring slot only when the residency table changed since that slot was last
    // written (quiescence — a parked camera uploads nothing). Value-initialized (all 0) rather than
    // a hardcoded four ~0ull, which would silently under-seed the array if the ring depth changed;
    // the first upload is forced by the m_AtlasBoundTexture change below (invalid -> the real atlas).
    uint64_t m_AtlasUploadedTableVersion[CBTTerrain::kCBTFrameParamsRing] = {};
    // The atlas height texture whose rows were last uploaded into each ring slot. A bound-terrain
    // change brings a different texture whose TableVersion may numerically equal the previous
    // terrain's; the version gate alone would then skip the row upload and the new terrain would
    // resolve through the old terrain's rows. A texture change resets the slot's uploaded version.
    Rendering::TextureHandle m_AtlasBoundTexture[CBTTerrain::kCBTFrameParamsRing] = {};
    // The paged height resolve's page-table upload gate per ring slot, as the atlas rows' above: the
    // terrain whose words each slot holds and their version (a terrain change re-uploads).
    uint64_t m_PageUploadedTableVersion[CBTTerrain::kCBTFrameParamsRing] = {};
    uint32_t m_PageBoundTerrain[CBTTerrain::kCBTFrameParamsRing] = {};
    // The terrain BuildFrameParams last bound (its render handle index), for TerrainSourceChanged.
    uint32_t m_BoundTerrainIndex = 0u;
    // The height source the frame parameters last committed (the terrain's handle, paged or not),
    // logged with the frame when it changes: the source that draws, not the page predicate's intent.
    uint32_t m_CommittedSourceTerrain = ~0u;
    uint32_t m_CommittedSourceGeneration = ~0u;
    bool m_CommittedSourcePaged = false;
    // Set on init / tree re-seed / planet-param change; consumed once at update-record time
    // to force a full-pool VertexEval (the quiescence gate skips unchanged bisectors).
    std::atomic<bool> m_ForceVertexEval{true};
    bool m_SphereShadingSignalLogged = false; // one-shot GE_CBT.SphereShading signal
    bool m_MultiTerrainWarned = false; // warn-once: CBT renders only terrain 0
    // The real terrain heightmap bound this frame (invalid = flat default). Set by
    // BuildFrameParams, read by CBTRenderNode for the CBT.Update read edge.
    Rendering::TextureHandle m_BoundHeightmap{};
    TerrainShadowSource m_TerrainShadowSource{};
    TerrainShadowBake m_TerrainShadowBake;
    Rendering::IDevice* m_Device = nullptr;
    // The owning RenderServices (set by EnsureInitialized), for TerrainSourceChanged.
    Engine::Renderer::RenderServices* m_Services = nullptr;
    // (graph, RGFrame index) the CBT.Update was declared into this frame — the
    // draw/producer inseparability key (see ClaimUpdate).
    const void* m_UpdateGraph = nullptr;
    uint64_t m_UpdateRgFrame = ~0ull;
    // Render->extraction confirmation counter (see NotifyUpdateRecorded).
    std::atomic<uint64_t> m_UpdateRecordCount{0};
    bool m_Initialized = false;
    bool m_InitAttempted = false; // one bring-up attempt; failures don't retry-spam
    bool m_Active = false;
    bool m_MaterialAttempted = false;
};

} // namespace GameEngine::CBTTerrainECS
