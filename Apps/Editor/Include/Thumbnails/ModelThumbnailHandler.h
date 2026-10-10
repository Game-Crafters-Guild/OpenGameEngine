#pragma once

#include "Events/Event.h"
#include "Thumbnails/AssetThumbnailHandler.h"
#include "Thumbnails/FolderBakeReport.h"
#include "Thumbnails/ModelThumbnailCachePolicy.h"

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "UI/UITextureSpace.h"

#include <array>
#include <deque>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine { namespace Animation { class AnimationClip; } }
namespace GameEngine { namespace Rendering { namespace RenderGraph {
class RGFrame;
struct RGTexture;
} } }

namespace GameEngine
{

using AnimationClip = ::GameEngine::Animation::AnimationClip;
class Asset;
class AssetManager;
class ModelAsset;
namespace ECS { class World; }
namespace Mathematics { struct Matrix4x4; }

namespace Engine::Renderer
{
class RenderServices;
class Material;
class AnimationSystem;
class HumanoidRetargetSystem;
class SkinningUploadSystem;
class TransformHierarchySystem;
class RenderExtractionSystem;
class LensFlareExtractionSystem;
}

// Thumbnail handler responsible for model-like assets (ModelAsset) and
// material assets (.material). Material assets are rendered onto a
// configurable primitive mesh (sphere by default).
class ModelThumbnailHandler : public AssetThumbnailHandler
{
  public:
    enum class PreviewShape { Sphere = 0, Cube, Plane };

    ModelThumbnailHandler(AssetManager* assetManager,
                          Engine::Renderer::RenderServices* renderServices);
    ~ModelThumbnailHandler() override;

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

    // RenderGraph arm (slice 8c-2b): same CPU phase, declarations into the window's
    // frame (bucketer twin → world → tonemap → encode into the slot's
    // handler-owned device texture). A window ticks on exactly one arm.
    void TickRenderRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame);

    // RenderGraph-mode binding. Pure frames (pureFrame != null): per-frame
    // SetExternalTextureRG + PublishExternalTextureRG of every ready slot —
    // the UI pass's declared read is the producer→sampler edge. Hybrid frames
    // (pureFrame == null): stable device-handle binds; the RenderGraph frame executes
    // before the old graph on the same queue with the slot exported
    // ShaderReadOnly, so the old UI's descriptor-direct sample is ordered.
    bool RegisterReadyThumbnailsRG(uint64_t windowId, class UIManager* ui,
                                   Rendering::RenderGraph::RGFrame* pureFrame);

    // Called when the editor switches projects / reinitializes AssetManager.
    // Keeps GPU resources alive but drops GUID mappings and pending work so we
    // can't accidentally show stale thumbnails from the previous project.
    void ResetForProjectSwitch() override;

    /// On-demand render trigger: when the UI requests an engine thumbnail name
    /// (engine:thumb-/listthumb-/material-) with no ready slot, acquire + enqueue
    /// the matching (list-aware) slot for this window. Restores the pre-RG
    /// ResolveEngineTextureForWindow path the immediate-mode cutover dropped;
    /// without it the list variant (hierarchy/inspector icons) never renders.
    void EnsureEngineThumbnailRequested(uint64_t windowId, const std::string& engineName);

    // Route material asset thumbnails through the same 3-D preview pipeline.
    // Returns engine:editor_material_thumb_<guid> immediately.
    std::string GetOrRequestMaterial(const std::filesystem::path& assetPath,
                                     int desiredSize,
                                     std::function<void(const std::string&)> onReady);

    // Engine-rendered live preview for .lensflare definitions. Both grid/list
    // thumbnails and Asset View use this path (non-square when focused in
    // the preview panel).
    std::string GetOrRequestLensFlare(const std::filesystem::path& assetPath,
                                      int desiredSize,
                                      std::function<void(const std::string&)> onReady);

    // Focused live material preview surface for UI panels such as the material
    // graph preview. This bypasses asset thumbnail lookup/cache callbacks while
    // reusing the same material-ball render pass.
    std::string RequestLiveMaterialPreview(uint64_t windowId,
                                           const GUID& materialGuid,
                                           uint32_t widthPx,
                                           uint32_t heightPx,
                                           bool previewIblEnabled = true);

    // Returns true once the slot backing `engineName` has produced a rendered
    // texture and that texture is currently bound for UI sampling. Callers
    // can use this to defer swapping the bound resource (e.g. a focused
    // preview pane) until the new render is ready, so the previous content
    // stays visible during the transition instead of going blank.
    bool IsEngineThumbnailReadyForWindow(uint64_t windowId, const std::string& engineName) const;

    // Where to write rendered material thumbnails as PNG so future navigations
    // bypass the live-render queue (see ThumbnailService::GetOrRequest material
    // disk-cache fast path). Empty disables the write side.
    // The next thumbnail tick deletes, on a worker, the model PNGs that other
    // bake versions wrote there.
    void SetCacheRoot(const std::filesystem::path& root) override;

    // The node-graph live preview material is regenerated on every edit and must
    // never be served from (or written to) the PNG disk cache — a cached frame
    // would freeze the preview on stale content. Identified by its generated
    // location under Generated/GraphPreview/.
    static bool IsEphemeralPreviewMaterialPath(const std::filesystem::path& path);
    // Disk-cache PNG names use the same asset GUID on read and write, independent
    // of path spelling or filesystem case sensitivity.
    static std::string MakeMaterialCacheFileName(const GUID& guid, bool previewIblEnabled);
    static std::string MakeModelCacheFileName(const GUID& guid);
    // Deletes the model PNGs under `cacheRoot` whose name carries another bake
    // version than MakeModelCacheFileName. Returns the number deleted.
    static size_t RemoveStaleModelCacheFiles(const std::filesystem::path& cacheRoot);

    // One asset of a folder bake (ThumbnailService::GenerateFolderThumbnails).
    struct BakeItem
    {
        GUID guid;
        bool isMaterial = false;
    };
    // Queues the grid-tile PNGs of `items` for baking after the visible tiles:
    // each takes a free slot (it never evicts a tile), renders like a grid tile,
    // persists its PNG and frees the slot. An item whose PNG is current is
    // skipped when its turn comes. Returns how many items were queued.
    // `missingFromDisk` are the files under the folder the asset database
    // lists but the disk no longer has; the report names them.
    size_t QueueBakes(const std::vector<BakeItem>& items,
                      const std::vector<std::filesystem::path>& missingFromDisk);
    // Receives the folder bake's report whenever it changes (main thread).
    void SetFolderBakeListener(std::function<void(const FolderBakeReport&)> listener);

    // The asset browser shows this model's grid tile from its PNG (the
    // ThumbnailService disk-cache path) from now on, so the next tick frees the
    // grid slot that drew it: a tile never holds both the slot's texture and the
    // PNG in the UI texture registry. Callable from any thread.
    void ReleaseGridSlotServedByPng(const GUID& guid);
    // Models queued by ReleaseGridSlotServedByPng that BeginFrame has not freed yet.
    size_t PendingGridSlotReleaseCount() const;

    // A scroll gesture in the asset browser. Grid thumbnails are the work the
    // gesture creates and the work the user cannot read while it lasts, so the
    // queue lanes hold for a short settle window and existing images stay
    // bound. The focused preview keeps its own lane. Repeated calls extend the
    // window.
    static void NoteAssetListScrollActivity();
    static bool IsAssetBrowserScrolling();

    // GetOrRequest for a caller that already holds the model GUID (the hierarchy binds
    // an entity's icon from its MeshRenderer). The render is driven purely by the
    // returned name — EnsureEngineThumbnailRequested parses the GUID back out of it
    // when the UI binds it — so this is the whole request; the path form only exists
    // to recover the GUID first, which an absolute OPFS path cannot do on web.
    // listStatic selects the fixed-yaw hierarchy/list slot over the rotating preview
    // slot.
    static std::string GetOrRequestByGuid(const GUID& guid, bool listStatic);

  private:
    static std::string MakeEngineThumbnailId(const GUID& guid, bool listStatic);

    struct PendingThumbRequest
    {
        GUID Guid{};
        bool ListStatic = false;
        bool IsMaterial = false;
        bool IsLensFlare = false;
        bool PreviewIblEnabled = true;
        // s_ScrollGeneration when this thumbnail was last asked for.
        uint64_t ScrollGeneration = 0;
    };

    static std::string MakeUITextureKey(const GUID& guid, bool listStatic);

    static bool TryParseGuidFromEngineName(const std::string& engineName, GUID& outGuid, bool& outListStatic);

    struct Slot
    {
        // Handler-owned SINGLE device texture, imported into the window's frame
        // per use (dedup-by-physical). The UI's declared read of the published
        // texture orders after the same frame's encode write. Never pooled
        // (pool persistents age out after kPersistentMaxIdleFrames; a static
        // grid tile must outlive idle periods).
        Rendering::TextureHandle deviceTex{};
        // False until the first encode into the texture has been declared:
        // import at Undefined (discard — encode is a full overwrite), then at
        // ShaderResource (the post-MarkOutput state) ever after.
        bool deviceTexInitialized = false;
        // deviceTex holds the tile's PNG from the disk cache (RGBA8 sRGB, at
        // the PNG size) instead of a live render target: once a grid tile's
        // bake is persisted it shows the same image every later session shows.
        // The focused preview and a reloaded model render live again.
        bool showsBakedImage = false;
        // A worker is decoding the tile's PNG for showsBakedImage; a PNG not
        // written yet is retried from bakedImageRetryFrame on.
        bool bakedImagePending = false;
        uint64_t bakedImageRetryFrame = 0;
        // The tile comes back as its PNG without an import or a render: its
        // bake was persisted this session and its slot was evicted since.
        bool restoresBakedImage = false;
        // A folder-bake slot (QueueBakes): nothing shows it, and it is freed
        // once its PNG is persisted. A request from a tile makes it a tile.
        bool bakeOnly = false;
        // Start of the bake's timeout (kBakeTimeout). A hold for a pipeline
        // build restarts it: the build finishes, however long the compile
        // queue is.
        std::chrono::steady_clock::time_point bakeClockStart{};

        GUID guid{};
        std::string uiKey; // cached MakeUITextureKey result
        bool occupied = false;
        // True once the slot has a completed thumbnail render and is safe for UI sampling.
        bool ready = false;
        // True for the frame the slot is being rendered (we delay ready until next frame
        // to avoid UI sampling a texture that will be written later in the same graph).
        bool inFlight = false;
        // When true, defer starting a new render for this slot until after one
        // frame where UI has had a chance to detach/remove the old external binding.
        // This prevents same-frame read/write overlap (UI sample vs thumbnail write).
        bool needsUiDetachBeforeRender = false;
        // Tracks whether this slot is currently bound into UIManager's external
        // texture map for this window.
        bool uiBound = false;
        // "This slot wants another render this dispatch." Producers:
        //   - DispatchPreviewLane before each orbit or animation re-render of
        //     the focused slot.
        //   - Animation step, zoom, LOD and focus-change paths that explicitly
        //     request a re-render of an already ready slot.
        // Consumers:
        //   1. RunAnimationClipManagement gates animTime advancement on it.
        //   2. DispatchPreviewLane and DrainNextPendingSlot re-render a ready
        //      slot only while the flag is set.
        // Cleared by SubmitThumbnailRenderRG after submission.
        bool needsOrbitRerender = false;
        // When true, this slot is for static list UI (e.g. Hierarchy); never shares
        // the global orbit angle or the Asset View rotating texture.
        bool listStaticThumb = false;
        // When this slot started waiting for its asset to finish loading
        // (default: not waiting). Names a thumbnail that can never render (see
        // kLoadWaitWarnAfter) instead of letting it wait silently.
        std::chrono::steady_clock::time_point loadWaitSince{};
        bool loadWaitReported = false;
        // The model import this slot renders from, requested as
        // AssetResidency::Transient: nothing else keeps the model in memory
        // once ReleaseSlotModel lets go of it. Empty for material and lens-flare slots.
        std::shared_future<std::shared_ptr<Asset>> modelImport;
        uint64_t lastUsed = 0;
        // Bounded count of consecutive dispatches deferred because the spawned
        // model's GPU instance was not yet extracted (see SubmitThumbnailRenderRG).
        uint32_t gpuNotReadyRetries = 0;
        // The texture holds a render made after that wait gave up: it does not
        // show the model, so it is never cached and a folder bake reports it.
        bool renderGaveUp = false;

        // The clip that poses the model: the embedded ClipStore clip at the
        // selected position (bound on the first dispatch), or the Animation
        // window's external preview clip.
        std::shared_ptr<AnimationClip> animClip;
        float animTime = 0.0f;
        bool usingExternalPreviewClip = false;
        // Indices into the model's embedded clip list (GetEmbeddedClipGuids).
        std::vector<uint32_t> animAvailableIndices;
        // Position into animAvailableIndices (0..N-1). -1 means "none selected".
        int animSelectedPos = -1;
        int animLoadedPos = -1;
        // ClipStore index of the bound clip and the position it was resolved
        // for, so orbit re-render frames skip the lookup.
        uint32_t cachedClipIndex = 0;
        int cachedClipPos = -1;

        // World-space AABB used only for thumbnail center/scale. Computed once per
        // slot assignment (see DispatchModelThumbnail); reused while orbit rotates
        // or animation advances so the preview does not reframe every frame.
        bool framingBoundsValid = false;
        float framingMin[3]{};
        float framingMax[3]{};

        // Current pixel dimensions of this slot's cached textures. 0 means not yet allocated.
        // Grid/list slots use square s_ThumbnailResolutionPx; the focused preview slot
        // tracks s_PreviewWidthPx/s_PreviewHeightPx so the RT aspect matches the panel.
        uint32_t texWidth = 0;
        uint32_t texHeight = 0;

        // True when this slot is a material (primitive preview) rather than a model.
        bool isMaterial = false;
        bool isLensFlare = false;
        bool previewIblEnabled = true;
        // The material slot's texture holds a settled render, the only material
        // frame the disk cache stores.
        bool materialRenderSettled = false;
    };

    // An isolated thumbnail world with its own view. A lane declares at most one
    // slot render per frame; the render finalizes on the next tick.
    struct RenderLane
    {
        std::unique_ptr<ECS::World> world;
        Rendering::CameraId cameraId = 0;
        Rendering::ViewId viewId = 0;

        // ModelEntityFactory creates per-submesh entities; the orbit root
        // carries the orbit transform + framing scale.
        GUID spawnedModelGuid{};
        Engine::Renderer::ModelEntityResult spawnedResult{};
        // The slot whose model or material is spawned here; the settle sticky
        // re-dispatches it.
        size_t spawnedSlot = kMaxResidentSlots;
        // Fully-ready renders of the current spawn (model or material) since it
        // was spawned. The mesh GPU upload races the inline thumbnail draw, and
        // extraction consumes the prior frame's slot buffers, so a spawn must stay
        // (no teardown) for a short settle window before its thumbnail renders
        // non-blank. The drain keeps re-dispatching the spawn until this reaches
        // ThumbnailCachePolicy::kSpawnSettleFrames, then advances to the next
        // pending request.
        uint32_t spawnedSettleFrames = 0;
        ECS::EntityHandle orbitRootEntity{};
        ECS::EntityHandle keyLightEntity{};
        ECS::EntityHandle ambientLightEntity{};

        // Material preview state. A single primitive entity (sphere/cube/etc.)
        // replaces the model entity set when a material thumbnail is being rendered.
        bool isSpawnedMaterial = false;
        GUID spawnedMaterialGuid{};
        GUID spawnedMaterialShapeGuid{};
        ECS::EntityHandle primitiveEntity{};

        // The asset the settle window runs for: the spawned material, else the
        // spawned model.
        const GUID& SpawnedGuid() const { return isSpawnedMaterial ? spawnedMaterialGuid : spawnedModelGuid; }

        // Lens-flare preview state. The source lives in the lane's world but is
        // rendered by the runtime flare post pass, not the mesh world pass.
        bool isSpawnedLensFlare = false;
        GUID spawnedLensFlareGuid{};
        ECS::EntityHandle lensFlareEntity{};

        // The slot this lane declared a render for, finalized as ready on the
        // next tick.
        size_t inFlightSlot = kMaxResidentSlots;
        uint64_t inFlightFrame = 0;
    };

    // The preview lane renders only the focused Asset View preview, so an
    // orbiting preview never takes the frame from the grid. The queue lanes
    // drain the pending queue in parallel: each spawned model needs several
    // frames to settle, and one lane per model would serialize a folder of
    // them.
    static constexpr size_t kPreviewLane = 0;
    static constexpr size_t kFirstQueueLane = 1;
    static constexpr size_t kRenderLaneCount = 4;

    struct WindowState
    {
        uint64_t frameCounter = 0;
        float orbitAngleY = 0.0f;
        // LRU residency
        uint64_t lruCounter = 0;
        // Consecutive ticks HasSlotWork found nothing to start or finish.
        uint32_t quietTicks = 0;
        std::vector<Slot> slots;
        std::unordered_map<GUID, size_t> guidToSlot;
        std::unordered_map<GUID, size_t> guidToListSlot;
        std::unordered_map<GUID, size_t> guidToMaterialSlot;
        std::unordered_map<GUID, size_t> guidToMaterialNoIblSlot;
        std::unordered_map<GUID, size_t> guidToLensFlareSlot;
        // Allocation-free key for the pending dedup set. ListStatic carries
        // distinct semantics in the queue (a static list thumbnail is a
        // separate request from the same GUID's dynamic preview), so it's
        // part of the identity.
        struct PendingKey
        {
            GUID Guid;
            bool ListStatic;
            bool IsMaterial;
            bool IsLensFlare;
            bool PreviewIblEnabled;
            bool operator==(const PendingKey& other) const noexcept
            {
                return ListStatic == other.ListStatic && IsMaterial == other.IsMaterial &&
                       IsLensFlare == other.IsLensFlare &&
                       PreviewIblEnabled == other.PreviewIblEnabled && Guid == other.Guid;
            }
        };
        struct PendingKeyHash
        {
            size_t operator()(const PendingKey& k) const noexcept
            {
                size_t h = std::hash<GUID>{}(k.Guid);
                if (k.ListStatic)
                    h ^= 0x9E3779B97F4A7C15ull;
                if (k.IsMaterial)
                    h ^= 0xBF58476D1CE4E5B9ull;
                if (k.IsLensFlare)
                    h ^= 0xD6E8FEB86659FD93ull;
                if (!k.PreviewIblEnabled)
                    h ^= 0x94D049BB133111EBull;
                return h;
            }
        };
        std::deque<PendingThumbRequest> pending;
        std::unordered_set<PendingKey, PendingKeyHash> pendingSet;

        std::array<RenderLane, kRenderLaneCount> lanes;

        // True while a lane still holds this model inside its settle window, so
        // the slot's texture may not yet show the model.
        bool IsModelSettling(const GUID& guid) const
        {
            for (const RenderLane& lane : lanes)
            {
                if (ThumbnailCachePolicy::IsSpawnSettling(guid, lane.spawnedModelGuid, lane.spawnedSettleFrames))
                    return true;
            }
            return false;
        }

        // UIManager external-texture keys evicted from slots since the last
        // RegisterReadyThumbnails call (includes list vs rotating suffix).
        std::vector<std::string> evictedUiKeys;

        // Latched on the first TickRenderRG: this window's slots are device
        // textures and its binding flows through RegisterReadyThumbnailsRG.
        // A window ticks on exactly one arm for its whole lifetime (the RenderGraph
        // frame exists per-window per-session, never per-frame).
        bool rg2Mode = false;
    };

    static constexpr size_t kMaxResidentSlots = 256;
    // Ticks a window keeps running its slot passes after its last activity: the
    // follow-ups of a finished render (the PNG readback, the PNG write a worker
    // finishes a few frames later, the baked-image retries) land well inside it.
    static constexpr uint32_t kQuietTicksBeforeIdle = 120;
    // Folder-bake queue entries StartQueuedBakes inspects per tick, skipped ones
    // included: a folder of current PNGs drains over several ticks instead of
    // stalling one.
    static constexpr size_t kMaxBakeItemsInspectedPerTick = 8;
    // A thumbnail whose asset has not finished loading after this long is
    // reported once.
    static constexpr std::chrono::seconds kLoadWaitWarnAfter{10};
    static constexpr float kDefaultOrbitVelocityY = 0.02f;
    static constexpr float kOrbitVelocityEaseLerp = 0.12f;

    static inline bool s_RotatePreviewsEnabled = true;
    // Plain scroll dollies the focused Asset View 3D preview when true (default).
    static inline bool s_PreviewScrollZoomEnabled = true;
    static inline float s_OrbitVelocityY = kDefaultOrbitVelocityY;
    static inline float s_OrbitTargetVelocityY = kDefaultOrbitVelocityY;
    static inline float s_OrbitAngleY = 0.0f;
    static inline bool s_AngleAdvancedThisFrame = false;
    // Real frame time as a multiple of a 60 FPS reference frame, refreshed each
    // BeginFrame. Per-frame preview advances (orbit rotation, animation playback)
    // multiply by this so they run at a consistent wall-clock rate regardless of
    // the editor's render frame rate. Defaults to 1 (one reference frame).
    static inline float s_PreviewFrameScale = 1.0f;
    static inline float s_LastNonZeroOrbitVelocityY = kDefaultOrbitVelocityY;
    static inline bool s_ClickPaused = false;
    static inline GUID s_OrbitFocusGuid{};
    static inline int s_ThumbnailResolutionPx = 1024;
    // Pixel dimensions the focused Asset View preview panel wants its thumbnail RT to be.
    // Zero until SetPreviewSize is called; the handler then allocates non-square targets
    // for the focused slot so the rendered image matches the panel aspect without letterboxing.
    static inline uint32_t s_PreviewWidthPx = 0;
    static inline uint32_t s_PreviewHeightPx = 0;
    // Preview-only lighting controls for model thumbnails and asset 3D views.
    static inline float s_PreviewLightIntensity = 1.2f;
    static inline float s_PreviewAmbientIntensity = 0.15f;
    static inline bool s_PreviewIblEnabled = false;
    // Forced LOD for the focused model preview (0xFFFFFFFF = auto). See SetPreviewForcedLOD.
    static inline uint32_t s_PreviewForcedLOD = 0xFFFFFFFFu;
    // Global preview orbit speed scale (multiplies all velocities set from the editor UI).
    static inline float s_OrbitSpeedScale = 1.0f;
    // User dolly zoom for the focused Asset View 3D preview (1 = default framing).
    static inline float s_PreviewZoomScale = 1.0f;
    static constexpr float kPreviewZoomMin = 0.25f;
    static constexpr float kPreviewZoomMax = 4.0f;
    static constexpr float kPreviewZoomScrollSpeed = 0.08f;
    // Largest wheel delta one scroll event may contribute, in notches. macOS
    // accelerates a spun mouse wheel into single events of 5-10 notches, and
    // the dolly is exponential in the delta, so an uncapped event jumps most
    // of the zoom range at once. Trackpad deltas are fractional and unaffected.
    static constexpr float kPreviewZoomMaxNotchesPerEvent = 2.0f;
    // Scroll/button clip cycling: ~9 clips/sec normally; fast wheel bursts
    // compress toward ~20 clips/sec so aggressive scrolling feels responsive.
    static constexpr int kAnimationStepMinIntervalMs = 110;
    static constexpr int kAnimationStepFastIntervalMs = 50;
    static constexpr int kAnimationStepBurstWindowMs = 80;
    static inline std::chrono::steady_clock::time_point s_LastAnimationStepTime{};
    static inline std::chrono::steady_clock::time_point s_LastAnimationAttemptTime{};
    static inline GUID s_AnimationPreviewGuid{};
    static inline std::shared_ptr<AnimationClip> s_AnimationPreviewClip;
    static inline float s_AnimationPreviewTime = 0.0f;
    static inline std::mutex s_AnimationPreviewMutex;
    static inline ModelThumbnailHandler* s_Instance = nullptr;

    static inline PreviewShape s_MaterialPreviewShape = PreviewShape::Sphere;
    static inline bool s_MaterialShapeChanged = false;
    static inline GUID s_MaterialOrbitFocusGuid{};
    static inline bool s_MaterialOrbitFocusIblEnabled = true;
    // The Asset View's material focus while it waits for the material's tile PNG
    // (PromotePendingMaterialFocus), and since when.
    static inline GUID s_PendingMaterialFocusGuid{};
    static inline bool s_PendingMaterialFocusIblEnabled = true;
    static inline std::chrono::steady_clock::time_point s_PendingMaterialFocusSince{};
    static inline GUID s_LensFlareFocusGuid{};

    // Cleared at the start of each frame in `BeginFrame`. Tracks GUIDs whose
    // shader prewarm has already been kicked this frame so the three-or-so
    // GetOrRequest calls per click (grid cell + inspector header + Asset
    // View preview) don't each enqueue an identical JobSystem task.
    static inline std::unordered_set<GUID> s_MaterialPrewarmedThisFrame;
    // Variant publishes arrive from PipelineVariantCache's VariantPublished
    // event (fired by the serial publish apply); buffered here and drained on
    // the main thread in BeginFrame. A set, not a vector: one material publishes a variant per
    // pass and per keyword permutation, so the same GUID arrives repeatedly
    // between drains and each duplicate would cost another re-render request.
    // Deduping on insert also bounds the buffer by distinct materials rather
    // than by publish count. Kept behind a mutex rather than made lock-free:
    // the producer is an occasional compile completion, not a per-frame path,
    // and the critical section is a single insert.
    static inline std::mutex s_PublishedVariantGuidsMutex;
    static inline std::unordered_set<GUID> s_PublishedVariantGuids;
    // Materials whose asset load completed on a worker and whose base shader is
    // still to be prewarmed. The load callback only records the GUID here: the
    // prewarm reads MaterialSystem's build context, which is serial-thread state,
    // so it runs from the main-thread drain in TickRenderImpl. A static like the
    // buffer above, so a completion that lands after this handler dies touches
    // nothing freed. A set: the same material can complete once per requesting
    // window between drains.
    static inline std::mutex s_PrewarmReadyMaterialsMutex;
    static inline std::unordered_set<GUID> s_PrewarmReadyMaterials;
    // Detaches on destruction, so a publish arriving after this handler dies
    // cannot reach a freed static.
    EventSubscription m_VariantPublishedSub;
    // Deadline until which the queue lanes hold; see NoteAssetListScrollActivity.
    static inline std::chrono::steady_clock::time_point s_ListRenderHeldUntil{};
    // Counts browser scroll events. Requests made since the latest scroll are
    // the tiles on screen now; the queue lanes render them first.
    static inline uint64_t s_ScrollGeneration = 0;

    // The RenderGraph frame a tick is declaring into.
    struct RenderArm
    {
        Rendering::RenderGraph::RGFrame* frame = nullptr;
    };

    // Graph-free bookkeeping (slot pick/evict/LRU). Texture creation is
    // deferred to dispatch (EnsureSlotDeviceTexture) so the request path can
    // acquire slots during pure-frame primitive generation without touching
    // the frame.
    size_t AcquireSlotForGuid(WindowState& ws, const GUID& guid,
                              bool listStatic, bool isMaterial = false,
                              std::optional<bool> previewIblEnabled = std::nullopt,
                              bool isLensFlare = false);
    // Requests drain in arrival order: the browser hands visible cells out in
    // bind order, so the rows on screen render first.
    static void EnqueuePending(WindowState& ws, const GUID& guid, bool listStatic,
                               bool isMaterial = false,
                               std::optional<bool> previewIblEnabled = std::nullopt,
                               bool isLensFlare = false);
    // Puts a request the drain took back at the end of the queue, unchanged.
    static void RequeuePending(WindowState& ws, const PendingThumbRequest& request);
    WindowState& GetOrCreateWindowState(uint64_t windowId);

    bool ResolveModelThumbnailPath(const std::filesystem::path& assetPath,
                                   std::filesystem::path& outResolved,
                                   GUID& outGuid);

    // Teardown spawned model entities and their GPUScene instances before
    // clearing the lane's world. Must be called before world->Clear().
    void TeardownSpawnedModel(RenderLane& lane);

    // Teardown the spawned material primitive entity (GPU instance removal).
    // Must be called before world->Clear().
    void TeardownSpawnedMaterial(RenderLane& lane);

    // Spawn (or reuse) a primitive entity with materialGuid applied.
    // Clears world and recreates if material GUID or shape changed.
    void SpawnOrUpdateMaterialEntity(RenderLane& lane, const GUID& materialGuid,
                                     const Mathematics::Matrix4x4& orbitMatrix);
    void SpawnOrUpdateLensFlareEntity(RenderLane& lane, const GUID& flareGuid,
                                      float orbitYaw);

    // Returns true when the material is compiled and ready for rendering.
    // Re-queues the slot and returns false if the material is not yet ready.
    bool EnsureMaterialReady(WindowState& ws, const GUID& guid, const Slot& slot);

    static std::string MakeMaterialEngineThumbnailId(const GUID& guid, bool previewIblEnabled = true);
    static std::string MakeMaterialUITextureKey(const GUID& guid, bool previewIblEnabled = true);
    static bool TryParseMaterialGuidFromEngineName(const std::string& engineName,
                                                   GUID& outGuid,
                                                   bool* outPreviewIblEnabled = nullptr);
    static std::string MakeLensFlareEngineThumbnailId(const GUID& guid);
    static std::string MakeLensFlareUITextureKey(const GUID& guid);
    static bool TryParseLensFlareGuidFromEngineName(const std::string& engineName, GUID& outGuid);

    // Desired slot pixel dims (focused slots track the live preview size).
    static void ComputeDesiredSlotDims(const Slot& slot, const GUID& guid, bool listStatic,
                                       bool isMaterial, bool isLensFlare,
                                       uint32_t& outW, uint32_t& outH);
    // Lazy creation of the single device texture (graph-free).
    void EnsureSlotDeviceTexture(Slot& slot, size_t slotIdx);

    // TickRender sub-steps. Each operates on the WindowState for the current window.
    void TickRenderImpl(uint64_t windowId, RenderArm arm);
    // Prewarms the base shader of every material s_PrewarmReadyMaterials holds.
    // Main thread only.
    void DrainPrewarmReadyMaterials();
    void FinalizeInFlightRender(WindowState& ws, RenderLane& lane);
    void ResizeSlotTextures(WindowState& ws);
    // The slot the Asset View (or the graph editor's live material preview)
    // focuses, or kMaxResidentSlots. Only the preview lane renders it.
    static size_t FindFocusedSlot(const WindowState& ws);
    // Renders the focused slot in the preview lane: its first render and settle
    // frames, then a re-render every frame its orbit advances or its animation
    // plays, and any re-render a preview control asks for.
    void DispatchPreviewLane(WindowState& ws, RenderArm arm, bool shouldAdvanceAngle);
    // Each idle queue lane takes the next pending request, or keeps
    // re-rendering the model it is settling.
    void DispatchQueueLanes(WindowState& ws, RenderArm arm);
    void DispatchSlotRender(WindowState& ws, RenderLane& lane, RenderArm arm, const GUID& guid,
                            size_t slotIdx);
    bool DrainNextPendingSlot(WindowState& ws, const RenderLane& lane, GUID& outGuid,
                              size_t& outSlotIdx);
    // True while another lane settles this slot's model; that lane re-renders it.
    static bool IsSettlingInAnotherLane(const WindowState& ws, const RenderLane& lane,
                                        size_t slotIdx);
    bool EnsureAssetLoaded(WindowState& ws, const GUID& guid, Slot& slot,
                           std::shared_ptr<class ModelAsset>& outModelAsset);
    // Starts the slot's transient model import once; returns the model when it
    // is imported, null while it imports or after it failed.
    std::shared_ptr<ModelAsset> AcquireSlotModel(Slot& slot, const GUID& guid);
    static std::shared_ptr<ModelAsset> ImportedSlotModel(const Slot& slot);
    static bool IsSlotModelImporting(const Slot& slot);
    // Lets go of the slot's model import and asks the AssetManager to unload
    // it. Idle lanes that still show the model tear it down first; a lane that
    // renders it postpones the release (returns false).
    bool ReleaseSlotModel(Slot& slot);
    // ReleaseSlotModel for an import held under `guid`: clears `modelImport`
    // and unloads the model once nothing else holds an import of it.
    bool ReleaseModelImport(const GUID& guid, std::shared_future<std::shared_ptr<Asset>>& modelImport);
    // True while a slot or a deferred release still holds an import of `guid`.
    bool IsModelImportHeld(const GUID& guid) const;
    // Keeps the slot's import until the lane rendering it finishes: a slot
    // reassigned mid-render must still unload the model it imported.
    void DeferModelRelease(Slot& slot);
    // Releases the deferred imports whose lanes have finished.
    void ReleaseDeferredModelImports();
    // Lets go of every slot's import (a project switch, after the lanes are
    // reset), so no transient model outlives the slots that imported it.
    void ReleaseAllModelImports();
    // Releases the imports of tiles that are baked and in the PNG cache.
    void ReleaseBakedModelImports(WindowState& ws);
    // A grid tile's bake is final once it is persisted, settled, out of focus
    // and has no render queued. It then needs neither its model nor its render
    // target: RequestBakedImages decodes the tile's PNG on a worker, and
    // ShowBakedImages uploads it and frees the target.
    bool IsTileBakeFinal(const WindowState& ws, const Slot& slot) const;
    bool WantsBakedImage(const WindowState& ws, const Slot& slot) const;
    void RequestBakedImages(WindowState& ws);
    // Returns true when a decoded PNG arrived, whether or not a tile took it.
    bool ShowBakedImages();
    // Whether the window's slot passes (texture resize, PNG readback, baked
    // images, model release, bakes) can have anything to do this tick: work
    // queued or rendering, a focused preview, a bake, a decoded image, or a
    // recent one of these (kQuietTicksBeforeIdle). An idle window skips them.
    bool HasSlotWork(WindowState& ws, bool imagesArrived);
    // Gives the slot's texture back for a live render; the tile shows nothing
    // until that render completes.
    void DropBakedImage(WindowState& ws, Slot& slot);
    // Detaches the slot from its asset: guid maps, queued request, lane spawn
    // and model import. The slot keeps its texture for its next asset.
    void ReleaseSlotAssignment(WindowState& ws, size_t slotIdx);
    // Frees, in every window, the grid slots of the models queued by
    // ReleaseGridSlotServedByPng. A slot that renders, bakes or holds the
    // focused preview keeps its asset until the LRU takes it.
    void ReleaseGridSlotsServedByPng();
    // Gives `guid` a slot for a tile the UI asked for: the baked PNG comes
    // back when the cache holds it (a tile freed by ReleaseGridSlotsServedByPng
    // and bound again), otherwise the model is imported and rendered.
    void RequestModelTile(WindowState& ws, const GUID& guid, bool listStatic);
    bool HasCurrentCachedPng(const GUID& guid, bool isMaterial) const;
    // Folder bakes run only while no tile waits, one per queue lane.
    void StartQueuedBakes(WindowState& ws);
    // Frees the bake slots whose PNG is persisted, or that cannot bake.
    void FinishBakes(WindowState& ws);
    // A readback with no covered pixel: nothing was drawn.
    static bool IsFullyTransparent(const std::vector<uint8_t>& pixels, Rendering::TextureFormat format);
    // An empty capture is persisted only when the asset renders empty twice.
    // The first time, this queues another settled render of its tiles and
    // returns true (the capture is dropped).
    bool RetryEmptyCapture(const GUID& guid);
    std::unordered_set<GUID> m_EmptyCaptureRetried;
    // Notes that the slot re-queued for a still-loading asset, and reports the
    // asset once it has waited kLoadWaitWarnAfter.
    void NoteLoadWait(Slot& slot, const class Asset& asset, const char* reason);
    void ComputeFramingBounds(Slot& slot, const class ModelAsset& modelAsset);
    void SpawnOrUpdateModel(RenderLane& lane, Slot& slot, const GUID& guid,
                            const class ModelAsset& modelAsset,
                            const GameEngine::Mathematics::Matrix4x4& modelMatrix);
    void DisablePreviewShadowing(RenderLane& lane);
    void RunAnimationClipManagement(RenderLane& lane, Slot& slot, const GUID& guid,
                                    const class ModelAsset& modelAsset);
    void UpdatePreviewLights(RenderLane& lane, float orbitYaw);
    void EnsureViewAndCamera(RenderLane& lane);
    void TickThumbnailSystems(ECS::World& thumbWorld);
    void SetupCameraMatrices(const RenderLane& lane, uint32_t attachmentW, uint32_t attachmentH);
    // RenderGraph arm: frame-transient MSAA/depth/resolve/tonemapped intermediates
    // sized to the slot; bucketer twin → world → tonemap → encode into the
    // imported slot device texture → MarkOutput(ShaderReadOnly). Also starts
    // the disk-cache ticket readback when due.
    void SubmitThumbnailRenderRG(WindowState& ws, RenderLane& lane, Slot& slot, size_t slotIdx,
                                 const GUID& guid, Rendering::RenderGraph::RGFrame& frame);

    // True once the spawned model has a live GPU scene instance (extraction
    // done) so the world pass has geometry to draw — see SubmitThumbnailRenderRG.
    bool IsSpawnedModelGpuReady(const RenderLane& lane) const;

    // True once every submesh material of the spawned model has finished binding
    // its textures to the device. Texture uploads are async and size-dependent,
    // so a large/texture-heavy model is drawable (instance live) before its
    // textures land — rendering then bakes a blank/untextured thumbnail. Broken
    // textures resolve (the pending bind is dropped), so this never stays false
    // forever. See SubmitThumbnailRenderRG.
    bool AreSpawnedModelTexturesReady(const RenderLane& lane) const;
    // A material of the spawned model still compiles its base pipeline or a pass
    // variant: its draws are skipped until the pipeline is published.
    bool AreSpawnedModelPipelinesCompiling(const RenderLane& lane) const;
    // True while the material's base pipeline or one of its pass variants
    // compiles; a draw in that window is skipped.
    bool IsMaterialPipelineCompiling(const GUID& materialGuid) const;

    // Disk-cache write helpers. MaybeStart declares a readback of one settled
    // grid slot whose PNG is missing; PollDiskCacheReadback drains a completed
    // readback (one at a time) onto a background thread for PNG encode + save.
    std::filesystem::path ComputeMaterialCachePath(const GUID& guid,
                                                   bool previewIblEnabled) const;
    std::filesystem::path ComputeModelCachePath(const GUID& guid) const;
    void PollDiskCacheReadback();
    // No re-render for the PNG: a ready slot keeps its persistent device
    // texture, so this imports it and declares a readback directly. Models and
    // materials alike; it retries every frame until the readback starts.
    void MaybeStartDiskCacheReadbackRG(WindowState& ws, Rendering::RenderGraph::RGFrame& frame);
    // Gives the Asset View its pending material focus once the material's tile PNG of
    // that variant is written, or after a few seconds without one
    // (SetMaterialOrbitFocusFromEngineName).
    void PromotePendingMaterialFocus();

    AssetManager* m_AssetManager = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;

    // ECS systems ticked manually on the thumbnail world each dispatch.
    // Order mirrors the scene-render wave: AnimationSystem may bootstrap
    // a HumanoidRetargeterComponent for cross-rig pairs, then
    // HumanoidRetargetSystem produces the per-character pose (CPU or GPU
    // path), then SkinningUpload/Transform/Extraction finalize.
    std::unique_ptr<Engine::Renderer::AnimationSystem> m_AnimationSystem;
    std::unique_ptr<Engine::Renderer::HumanoidRetargetSystem> m_HumanoidRetargetSystem;
    std::unique_ptr<Engine::Renderer::SkinningUploadSystem> m_SkinningUploadSystem;
    std::unique_ptr<Engine::Renderer::TransformHierarchySystem> m_TransformSystem;
    std::unique_ptr<Engine::Renderer::RenderExtractionSystem> m_ExtractionSystem;
    std::unique_ptr<Engine::Renderer::LensFlareExtractionSystem> m_LensFlareExtractionSystem;

    std::unordered_map<uint64_t, WindowState> m_Windows;

    uint32 m_AssetEventCallbackHandle = 0;

    // Disk-cache write side. After a material slot finishes rendering and the
    // PNG doesn't exist yet, we request a single readback at a time and write
    // the resolved color to disk on a background thread. Subsequent navigations
    // hit ThumbnailService's cache check and bypass the live-render queue.
    struct PendingDiskCache
    {
        GUID guid{}; // material or model
        std::filesystem::path outputPath;
        std::filesystem::path assetPath; // source asset, for the written-callback
        std::shared_ptr<Rendering::RGReadbackTicket> ticket; // RenderGraph arm (8c-1)
        UI::UITextureSpace srcSpace; // no default: the declaration states it
        // Long-edge cap applied before the PNG encode (0 = keep readback size).
        // Model slots render at s_ThumbnailResolutionPx; grid tiles sample at
        // <=128 px and path-valued thumbnails upload at full PNG size, so the
        // cache write is where the downscale must happen.
        uint32_t maxPngLongEdge = 0;
    };
    std::optional<PendingDiskCache> m_PendingDiskCache;
    std::filesystem::path m_DiskCacheRoot;
    bool m_StaleCacheSweepPending = false;
    // Materials we've already requested a readback for (success or pending).
    // Prevents repeatedly re-firing on stale-cache misses while a readback is
    // being processed (or PNG is being encoded on the worker thread).
    std::unordered_set<GUID> m_DiskCacheRequested;

    // Tile PNGs decoded by worker jobs for showsBakedImage, drained on the main
    // thread. Shared with the jobs, which can outlive the handler.
    struct BakedImage
    {
        GUID guid;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels; // RGBA8, sRGB-encoded; empty when the PNG is not fresh yet
    };
    struct BakedImageInbox
    {
        std::mutex mutex;
        std::vector<BakedImage> images;
    };
    std::shared_ptr<BakedImageInbox> m_BakedImages = std::make_shared<BakedImageInbox>();

    // Model imports whose slots moved on while a lane rendered them
    // (DeferModelRelease); released by ReleaseDeferredModelImports.
    struct DeferredModelRelease
    {
        GUID guid;
        std::shared_future<std::shared_ptr<Asset>> modelImport;
    };
    std::vector<DeferredModelRelease> m_DeferredModelReleases;

    // Models whose grid tiles the browser now shows from their PNG
    // (ReleaseGridSlotServedByPng), drained by the next tick.
    mutable std::mutex m_ServedByPngMutex;
    std::vector<GUID> m_ServedByPng;

    // Folder bakes not started yet (QueueBakes), shared by every window, and
    // the outcome of the batch the log reports once the queue drains.
    std::deque<BakeItem> m_BakeQueue;
    std::unordered_set<GUID> m_BakeQueued;
    struct BakeBatch
    {
        size_t queued = 0;
        size_t baked = 0;
        size_t skipped = 0;
        std::vector<std::filesystem::path> failed;
        std::vector<std::filesystem::path> missingFromDisk;
        bool active = false;
    };
    BakeBatch m_BakeBatch;
    std::function<void(const FolderBakeReport&)> m_FolderBakeListener;
    FolderBakeReport m_PublishedBakeReport;
    // Counts a folder-bake asset that cannot bake and names it in the log.
    void NoteBakeFailure(const GUID& guid);
    // Sends the batch's report to the listener when it differs from the last one sent.
    void PublishFolderBakeReport();

  public:
    /// Call once per application frame before any TickRender so orbit angle advances at most once per frame.
    static void BeginFrame();
    static void SetRotatePreviewsEnabled(bool enabled);
    static bool GetRotatePreviewsEnabled() { return s_RotatePreviewsEnabled; }

    // When true (default), plain mouse-wheel over the Asset View 3D preview
    // dollies in/out. When false, plain scroll does nothing; Ctrl+scroll still
    // cycles animations.
    static void SetPreviewScrollZoomEnabled(bool enabled) { s_PreviewScrollZoomEnabled = enabled; }
    static bool GetPreviewScrollZoomEnabled() { return s_PreviewScrollZoomEnabled; }
    // Mark a specific asset as the orbit-rotation focus. Pass the engine
    // texture name (without the "engine:" prefix) emitted by GetOrRequest().
    // The name is parsed internally to extract the asset GUID.
    static void SetOrbitFocusFromEngineName(const std::string& engineName);
    static void ClearOrbitFocus()
    {
        s_OrbitFocusGuid = GUID{};
        ResetFocusedPreviewZoom();
    }
    // Set the desired pixel size for the focused preview. Pass the Asset View panel's
    // current image element dimensions; the handler will (re)allocate non-square RTs
    // for the focused slot so the rendering matches the panel aspect.
    // Pass (0, 0) to fall back to square s_ThumbnailResolutionPx for the focused slot.
    static void SetPreviewSize(uint32_t widthPx, uint32_t heightPx);
    static void SetOrbitVelocity(float velY);
    static void EaseToOrbitVelocity(float velY);
    static void PauseClickRotation();

    // Dolly the focused Asset View 3D preview in/out. Positive scrollY dollies
    // out (model smaller); negative dollies in — same sign as Scene View dolly.
    static void AdjustFocusedPreviewZoom(float scrollY);
    static void ResetFocusedPreviewZoom();
    static float GetFocusedPreviewZoomScale() { return s_PreviewZoomScale; }

    // Cycle the focused model's embedded animation clip selection.
    // Positive deltaSteps selects the next clip, negative the previous.
    static void StepFocusedAnimation(float deltaSteps);
    // Returns a label for the focused model: current clip name, total embedded animation count
    // (e.g., \"Walk - 3 animations\"), or an empty string if none are available.
    static std::string GetFocusedAnimationLabel();
    static size_t GetFocusedAnimationCount();
    static void SetExternalAnimationPreview(const std::filesystem::path& modelPath,
                                            const std::shared_ptr<AnimationClip>& clip,
                                            float currentTime);
    static void ClearExternalAnimationPreview();

    // Runtime-configurable thumbnail resolution (currently supports 256 or 1024).
    static void SetThumbnailResolution(int px);
    static int GetThumbnailResolution() { return s_ThumbnailResolutionPx; }

    // Runtime-configurable light intensity multiplier for 3D previews (model thumbnails,
    // asset view). This does not affect Scene/Game views.
    static void SetPreviewLightIntensity(float value);
    static float GetPreviewLightIntensity() { return s_PreviewLightIntensity; }

    // Flat ambient fill for 3D previews (lifts the key light's shadow side).
    static void SetPreviewAmbientIntensity(float value);
    static float GetPreviewAmbientIntensity() { return s_PreviewAmbientIntensity; }

    // Image-based lighting for 3D previews. When disabled, model/material previews
    // use only the preview key light and flat ambient fill.
    static void SetPreviewIblEnabled(bool enabled);
    static bool GetPreviewIblEnabled() { return s_PreviewIblEnabled; }

    // Force a specific mesh LOD in the focused model preview (Asset View),
    // overriding coverage-based auto-selection (which always picks LOD0 because
    // the preview frames the model large). 0xFFFFFFFF restores auto. Driven by
    // the model inspector's LOD slider; affects only the focused preview view.
    // On change, forces a one-shot re-render of the focused slot so the new LOD
    // shows even when orbit rotation is paused or the model isn't animating.
    static void SetPreviewForcedLOD(uint32_t lod);
    static uint32_t GetPreviewForcedLOD() { return s_PreviewForcedLOD; }

    static void SetOrbitSpeedScale(float scale);
    static float GetOrbitSpeedScale() { return s_OrbitSpeedScale; }

    // Material preview shape (shared across all material thumbnails).
    static void SetMaterialPreviewShape(PreviewShape shape);
    static PreviewShape GetMaterialPreviewShape() { return s_MaterialPreviewShape; }

    // Orbit focus for the material preview in the Asset View panel.
    static void SetMaterialOrbitFocusFromEngineName(const std::string& engineName);
    static void ClearMaterialOrbitFocus()
    {
        s_MaterialOrbitFocusGuid = GUID{};
        s_MaterialOrbitFocusIblEnabled = true;
        s_PendingMaterialFocusGuid = GUID{};
        ResetFocusedPreviewZoom();
    }

    static void SetLensFlareFocusFromEngineName(const std::string& engineName);
    static void ClearLensFlareFocus() { s_LensFlareFocusGuid = GUID{}; }

    /** Mark a material thumbnail slot dirty and re-enqueue rendering (e.g. live graph preview).
     *  When propertyValuesOnly is true, keep the spawned preview entity and only rerender
     *  with updated material UBO values. Otherwise tear down the entity (shader/pipeline change). */
    static void InvalidateMaterialThumbnail(const GUID& guid, bool propertyValuesOnly = false);

    // Current orbit angle (used by material thumbnail handler to sync rotation).
    static float GetOrbitAngleY() { return s_OrbitAngleY; }
};


} // namespace GameEngine
