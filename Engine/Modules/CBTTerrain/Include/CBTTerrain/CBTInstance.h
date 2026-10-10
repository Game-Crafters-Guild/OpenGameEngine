#pragma once

// CBTInstance — owns one CBTResources and drives the per-frame CBT update as a
// single hand-barriered compute sequence (the Ocean pattern, plan §5). C1 scope:
// CPU root init + one RecordUpdate that records the 19-step kernel chain into a
// caller-provided command list. No render-graph / ECS integration yet (C2).

#include <cstdint>
#include <vector>

#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTResources.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

namespace GameEngine::Rendering
{
class CommandList;
}

namespace GameEngine::CBTTerrain
{

// Drives the Classify metric. Mode selects between the C4 production metric
// (kClassifyScreenSpace — projected edge length vs a target pixel error, camera +
// terrain from CBTFrameParams) and the deterministic depth-band metric
// (kClassifyDepthTarget — FocusRoot's subtree to TargetDepth, the rest to
// BaseDepth) used by the unit tests / bring-up. Under screen-space, TargetDepth is
// reused as the max-subdivision cap.
struct CBTClassifyDesc
{
    uint32_t Mode = kClassifyDepthTarget;
    uint32_t FocusRoot = kFocusRootAll;
    uint32_t TargetDepth = kDefaultBaseDepth;

    // C5 region-dirty reclassification (plan §8 C5): the terrain footprint edited
    // this frame, in UV [0,1], sourced from the region-dirty log (CBTUpdateSystem
    // maps texels -> UV with its own log cursor). Classify flags any bisector whose
    // UV AABB overlaps this rect into the MODIFIED draw stream; the new heights reach
    // the screen-space metric through VertexEval's re-sample, so the region re-refines
    // under the normal split test (an edit never coarsens — the merge threshold is not
    // lowered for dirty bisectors). Empty (DirtyMaxU <= DirtyMinU) => no edit => the
    // reclassification path is skipped and the update loop stays quiescent.
    float DirtyMinU = 0.0f;
    float DirtyMinV = 0.0f;
    float DirtyMaxU = 0.0f;
    float DirtyMaxV = 0.0f;

    // Planet-editing v1 (plan §planet-editing). On the SPHERE, the dirty rect above is a
    // FACE-LOCAL UV rect and DirtyFace selects the cube face — Classify reclassifies only
    // bisectors on that face. SphereSculptEnabled gates VertexEval's additive sculpt
    // sample (true once any dab has landed). Both ignored in planar mode.
    uint32_t DirtyFace = 0u;
    uint32_t SphereSculptEnabled = 0u;

    // Quiescence gate for VertexEval (plan §planet-shading perf). 0 (default) evaluates the
    // whole live pool, refreshing screen-space classification inputs before culling after a
    // parameter change. 1 evaluates only the compact MODIFIED stream, so a quiescent frame
    // launches no vertex workgroups. First use always initializes the root geometry.
    uint32_t GateVertexEval = 0u;

    // Edit-driven retessellation A/B (round-8b). 1 (default) = Classify carries the geometric-error
    // crease term that refines a facet straddling a live sculpt cliff the area metric cannot see;
    // 0 = the pre-slice area-only metric (the fails-before behavior). CBTUpdateSystem drives it
    // from GE_CBT_EDIT_RETESS so the fix can be A/B'd — and disabled — on the same build.
    uint32_t EditRetessEnabled = 1u;

    // Near-field force-split occupancy gate A/B (round-8e saturation-deadlock fix). 1 (default) =
    // under pool pressure, stop force-splitting invisible near-plane/behind-eye grazing geometry so
    // the pool never pins at 100% and a static-camera edit / TargetPixelError change can re-tessellate;
    // 0 = the pre-slice unconditional near-plane force-split (the fails-before behavior). CBTUpdateSystem
    // drives it from GE_CBT_NEARFIELD_GATE so the fix can be A/B'd — and disabled — on the same build.
    uint32_t NearFieldGate = 1u;

    // Earth-scale (sector, local) gVertex storage A/B (decode-precision arc S2a). 1 = spherical
    // VertexEval decodes corners via the df64 path (cbt_deep_decode.glsl, GLSL twin of
    // CBTDeepDecode.h) and stores (integer 1024 m sector anchor, fp32 local offset) — the
    // GE_ClipFromSectorLocal representation, immune to the Earth-magnitude fp32 store
    // quantization the S1 probes measured. 0 (default) = the fp32 world store, byte-identical to
    // the pre-S2a pipeline (the dark-ship). CBTUpdateSystem drives it from GE_CBT_DEEP_DECODE.
    uint32_t DeepDecode = 0u;

    // Planar pool-pressure scale. 1 (default) = a planar view whose visible field exceeds the pool
    // has its split/merge thresholds scaled up until the pool holds it with headroom (Kernel_Reset
    // steps the scale; Kernel_Classify applies it). 0 holds the scale at 1x. This is an A/B arm for
    // the two oracles that characterise the pinned regime (CBTUpdateSystem drives it from
    // GE_CBT_POOL_PRESSURE for the same purpose on a live build); it is not a setting.
    uint32_t PoolPressure = 1u;
};

// Inert classify (nothing to split or merge): leaves the tree unchanged.
inline CBTClassifyDesc CBTInertClassify()
{
    return CBTClassifyDesc{kClassifyDepthTarget, kFocusRootAll, kDefaultBaseDepth};
}

// Identity terrain mapping for the depth-target path (tests / bring-up with no
// terrain): world XZ == the unit-square UV (size 1, origin 0), flat (heightScale 0).
// The camera fields are unused under kClassifyDepthTarget.
inline CBTFrameParams CBTIdentityFrameParams()
{
    CBTFrameParams p{};
    p.TerrainSize[0] = 1.0f; // sizeX
    p.TerrainSize[1] = 1.0f; // sizeZ
    p.TerrainSize[2] = 0.0f; // heightScale (flat)
    p.TerrainSize[3] = 0.0f; // originY
    return p;
}

// Tessellation-health snapshot for the confirmation stats readout. The frozen-topology fingerprint
// is LiveCount == PoolSize with splits going unserved and the merge freeing nothing; MergeDemand >>
// MergeServed on its own is normal (see CBTPoolHealth.h, which owns the interpretation). Populated
// by ReadTessellationStats (a self-contained submit+wait, state-restoring — for the on-demand debug
// query, not the per-frame path).
struct CBTTessellationStats
{
    uint32_t LiveCount = 0;    // sum-tree root: live bisectors this frame
    uint32_t PoolSize = 0;     // bisector pool capacity
    // Seeded roots for this domain (2 planar, 24 spherical). LiveCount == RootCount means the tree
    // has never refined: the roots are the whole mesh, or nothing is drawn when no terrain reached
    // the renderer (CBTPoolHealth.h owns that interpretation).
    uint32_t RootCount = 0;
    int32_t FreeCount = 0;     // free-slot budget at the last Reset (pool - live)
    int32_t SplitDemand = 0;   // split candidates Classify enqueued last frame
    int32_t SplitServed = 0;   // allocations Split satisfied last frame
    int32_t MergeDemand = 0;   // simplify candidates Classify enqueued last frame
    // Merge groups PrepareSimplify built last frame. 0 is NOT by itself a stall: a converged tree
    // carries standing merge demand no LEB diamond can ever satisfy (CBTPoolHealth.h).
    int32_t MergeServed = 0;
    int32_t OverflowTotal = 0; // cumulative Split rollbacks, saturating at INT32_MAX until re-seed
    // Cumulative dispatch-width clamps since init (kWQDispatchClampCounter). Non-zero means a
    // GPU-written dispatch count (sum-tree root / work-queue counter) exceeded the pool bound
    // and was clamped before it could become an unbounded indirect dispatch (the TDR class) —
    // i.e. a corrupted count was produced AND caught. The first-load device-loss discriminator.
    int32_t DispatchClampTotal = 0; // saturates at INT32_MAX
    // Planar pool-pressure scale step (kWQPressureStep); threshold scale =
    // 2^(step / kPressureStepsPerOctave). 0 = 1x.
    int32_t PressureStep = 0;
    // Planar off-frustum keep step (kWQOffFrustumKeepStep): how far the off-frustum keep band has
    // collapsed under pool pressure, 0 (ungated) to kOffFrustumKeepSteps (band 0).
    int32_t OffFrustumKeepStep = 0;
};

class CBTInstance
{
  public:
    CBTInstance() = default;
    ~CBTInstance() = default;

    CBTInstance(const CBTInstance&) = delete;
    CBTInstance& operator=(const CBTInstance&) = delete;

    bool Initialize(Rendering::IDevice& device, const CBTKernelSet& kernelSet,
                    uint32_t poolSize = kDefaultBisectorPoolSize);
    void Shutdown();

    // Device-rebuild recovery: same reset as Shutdown(), but forgets the resources'
    // dead handles instead of destroying them (the rebuild teardown already freed the
    // VkObjects). Leaves the instance not-ready so a later Initialize re-allocates
    // against the new device. See CBTResources::ReprovisionAfterDeviceRebuild.
    void ReprovisionAfterDeviceRebuild();

    bool IsReady() const { return m_Ready; }
    CBTResources& GetResources() { return m_Resources; }
    uint32_t GetDomainMode() const { return m_DomainMode; }
    uint32_t GetBaseDepth() const { return m_BaseDepth; }
    uint32_t GetRootCount() const { return m_RootCount; }
    uint32_t GetPoolSize() const { return m_PoolSize; }
    // Neighbor ping-pong parity after the last RecordUpdate: true => NeighborsA holds this frame's
    // final topology (read), false => NeighborsB. Diagnostics only — lets a debug readback select the
    // buffer that holds the current links.
    bool GetNeighborsReadIsA() const { return m_NeighborsReadIsA; }

    // Clears free-slot guards/counters and uploads the domain's root bisectors
    // (HeapID = 2^baseDepth + i). Planar seeds 2 twin unit-square triangles
    // (baseDepth 1); spherical seeds the 24 cube-sphere pie slices with cross-face
    // twins (baseDepth 5, CBTSphereRoots.h). Copies and reduction share one graphics
    // submission. A live restart retires the prior graphics token before replacing sculpt-ring
    // descriptors; the upload retires with its submission. No device-wide drain is needed.
    // Unreachable payload records retain their bytes; new live slots are initialized before use.
    bool InitializeRoots(uint32_t domainMode = kDomainPlanar);

    // Refines a freshly seeded tree to `depth` everywhere: the depth-target update, one level
    // per pass, each pass waiting its graphics token before the next. A restarted tree then draws
    // facets of that depth from its first frame instead of its roots (4 or 24 triangles), which
    // cannot show relief. Corners are evaluated against the flat identity mapping; the caller
    // forces a full VertexEval on the next update. Call right after InitializeRoots, between
    // frames, with this frame's counter (its UBO ring element is rewritten by that frame's
    // update). No-op when `depth` does not exceed the base depth.
    void RefineUniform(uint32_t depth, uint32_t frameCounter);

    // This frame's ring frame counter — see CBTResources::AdvanceFrameCounter, which owns it.
    // Every per-frame entry point below takes the returned value, and RecordUpdate pushes it
    // for the GPU to reduce the same way, so CPU and GPU land on the same ring element.
    uint32_t AdvanceFrameCounter(uint32_t deviceFrameIndex)
    {
        return m_Resources.AdvanceFrameCounter(deviceFrameIndex);
    }

    // Records the full update sequence (Reset -> Classify -> split path -> neighbor
    // copy -> Bisect -> propagate -> merge path -> reduce -> vertex-eval ->
    // indexation -> Validate) into `cl`, then flips the neighbor ping-pong parity.
    // `classify` selects the metric; `params` (camera + terrain) is uploaded to this
    // frame's UBO ring slot before the first dispatch, so Classify (screen-space) and
    // VertexEval (displacement) read it. The caller binds the height source once via
    // SetHeightSource. The caller wraps cl.Begin()/End() and executes it. Usable both
    // stand-alone and from an RG pass exec lambda (ctx.Cmd is a CommandList&).
    // `frameCounter` is this frame's value from AdvanceFrameCounter: it picks the UBO ring
    // element written here AND is pushed verbatim, so the GPU reduces the same number.
    void RecordUpdate(Rendering::CommandList& cl, const CBTClassifyDesc& classify,
                      const CBTFrameParams& params, uint32_t frameCounter);

    // The debug Validate kernel (link reciprocity / budget / zombie invariants) is a WHOLE-POOL
    // dispatch that produces nothing the render consumes — pure overhead in ship, and the one
    // pool-linear per-frame cost the S3 indirect conversion did not remove (it must scan the free
    // slots too, so it cannot ride the live compact stream). Defaults ON so every test keeps
    // asserting the invariants; the shipping owner (CBTRenderFeature) turns it OFF so the update's
    // idle floor does not grow with the pool. The ReadValidation* accessors report the counters
    // only while it is enabled.
    void SetValidateEachUpdate(bool enabled) { m_ValidateEachUpdate = enabled; }
    bool ValidatesEachUpdate() const { return m_ValidateEachUpdate; }

    // Rebinds the terrain height texture the VertexEval kernel samples for this frame
    // (binding 15 ring element CBTFrameRingSlot(frameCounter)), or the built-in flat
    // default when invalid. Call with the SAME frameCounter passed to RecordUpdate this
    // frame — the shader reduces the pushed counter identically — so a mid-session handle
    // change never rewrites an in-flight ring element. The same texture the CDLOD path
    // samples (plan §8 C4).
    void SetHeightSource(uint32_t frameCounter, Rendering::TextureHandle heightTexture);

    // In-place re-provision safety (plan §re-provision). Retires the graphics work already
    // submitted, then rewrites EVERY height/atlas/coarse ring element back to its built-in default
    // in one shot (see CBTResources::RebindTerrainSourcesToDefault), so a retired terrain heightmap
    // is dropped from the whole ring deterministically instead of relying on the per-slot lazy
    // refresh to rotate through every element before the image is freed. That timeline wait — one
    // value on one queue, not a device drain — is why this is safe to call from the render-graph
    // declare, and why it stays reserved for the rare re-provision rather than the per-frame path.
    // Returns true when a slot changed.
    bool RefreshTerrainSources();
    uint64_t GetTerrainSourceGeneration() const { return m_Resources.GetTerrainSourceGeneration(); }
    uint32_t CountTerrainSourceSlots(Rendering::TextureHandle texture) const
    {
        return m_Resources.CountTerrainSourceSlots(texture);
    }

    // Phase E resident-window atlas (design §3). Rebinds the frame's atlas height texture
    // (binding 18 ring) and uploads the frame's indirection rows (binding 17 ring). Call
    // with the SAME frameCounter passed to RecordUpdate; a host-visible ring write that
    // completes before submit (like the params UBO). SetAtlasSource with an invalid handle
    // resets the slot to the flat default. See CBTResources.
    void SetAtlasSource(uint32_t frameCounter, Rendering::TextureHandle atlasTexture)
    {
        m_Resources.SetAtlasSource(frameCounter, atlasTexture);
    }
    void SetCoarseSource(uint32_t frameCounter, Rendering::TextureHandle coarseTexture)
    {
        m_Resources.SetCoarseSource(frameCounter, coarseTexture);
    }
    void UploadAtlasRows(uint32_t frameCounter, const void* rows, uint32_t rowCount)
    {
        m_Resources.UploadAtlasRows(frameCounter, rows, rowCount);
    }

    // The paged height resolve (bindings 23 and 22, wide arm only): the frame's page cache texture
    // and the terrain's page-table words. Same frameCounter and ring discipline as the atlas above.
    void SetPageCacheSource(uint32_t frameCounter, Rendering::TextureHandle cacheTexture)
    {
        m_Resources.SetPageCacheSource(frameCounter, cacheTexture);
    }
    bool ProvisionPageTableWords(uint32_t words) { return m_Resources.ProvisionPageTableWords(words); }
    void UploadPageTable(uint32_t frameCounter, std::span<const uint32_t> words)
    {
        m_Resources.UploadPageTable(frameCounter, words);
    }

    // Uploads the sphere sculpt page POOL + page TABLE into this frame's ring slot (planet editing
    // v2). Call with the SAME frameCounter passed to RecordUpdate; a host-visible write that completes
    // before submit (like the params UBO). See CBTResources::UploadSphereSculptPool / PageTable.
    void UploadSphereSculptPool(uint32_t frameCounter, const float* pool, uint32_t texelCount)
    {
        m_Resources.UploadSphereSculptPool(frameCounter, pool, texelCount);
    }
    void UploadSphereSculptPageTable(uint32_t frameCounter, const uint32_t* table, uint32_t entryCount)
    {
        m_Resources.UploadSphereSculptPageTable(frameCounter, table, entryCount);
    }
    uint32_t GetSculptPagePoolCount() const { return m_Resources.GetSculptPagePoolCount(); }

    // Uploads the planet-shading surface params into this frame's ring slot (the fragment
    // reads them on the graphics side — plan §planet-shading). Same host-visible ring
    // discipline as the params UBO. Call with the SAME frameCounter passed to RecordUpdate.
    void UploadSurfaceParams(uint32_t frameCounter, const CBTSurfaceParams& params)
    {
        m_Resources.UploadSurfaceParams(frameCounter, params);
    }

    // Far-field drain (round-9 far-field-drain, design candidate 3): bulk-free the selected root
    // subtrees back to base in one standalone hand-barriered event — CLEAR (free every bisector
    // whose root is in the mask) -> reduce -> SEED (reseed each freed root's base bisector into a
    // decoded free slot) -> LINK (wire the base neighbor links; INVALID toward a retained/visible
    // neighbor) -> reduce. `freeSetMask` bit r frees root r; it MUST be the interior of a wholly-
    // horizon-invisible root region (FarFieldReseedDetector) so the retained one-root ring keeps the
    // base/refined seam out of the visible boundary and the tree stays conforming (Validate green).
    // Manages its own command list + submit like InitializeRoots, barrier-ordered against the
    // frames around it; call BETWEEN frames (never mid-update). No-op for an empty mask, a
    // non-spherical domain, or before InitializeRoots.
    void RegionFreeToBase(uint32_t freeSetMask);

    // Records the debug-counter readback (indirect-draw records + validation)
    // into `cl`. Execute and wait that submission, then use the Read* accessors.
    void RecordReadback(Rendering::CommandList& cl);

    // Self-contained submit+wait reads of the indirect-draw records, each recording its own
    // IndirectDraw -> readback copy. The records are per-frame (Kernel_Reset rewrites them), so
    // these report the LAST completed update's draw. State-restoring.
    uint32_t ReadDrawIndexCount(uint32_t stream) const; // stream in {All,Visible,Modified}
    // One field of a VkDrawIndexedIndirectCommand record (stream in {All,Visible,
    // Modified}, field in {indexCount,instanceCount,firstIndex,vertexOffset,firstInstance}).
    // Lets the draw-input oracle assert the whole record is well-formed, not just the count.
    uint32_t ReadDrawRecordField(uint32_t stream, uint32_t field) const;
    // Self-contained submit+wait reads of the debug validation counters: each records its own
    // Validation -> readback copy, so a caller cannot read counters no readback ever populated.
    // The counters are per-frame (Kernel_Reset zeroes them), so these return the LAST completed
    // update's verdict. State-restoring, so they are safe to call between frames.
    uint32_t ReadValidationErrorCount() const;          // link-reciprocity errors
    uint32_t ReadValidationCounter(uint32_t counterSlot) const; // any kValidation* slot

    // Self-contained submit+wait read of one signed work-queue counter (e.g.
    // kWQFreeCount, kWQOverflowCounter, kWQMultiChildCounter). Diagnostics only.
    int32_t ReadWorkQueueCounter(uint32_t counterSlot);

    // Tessellation-health snapshot (round-8e confirmation stats). One self-contained submit+wait per
    // buffer (WorkQueue counters + the sum-tree root), state-restoring so it is safe to call between
    // frames. The counters are per-frame (Reset zeroes them), so this returns the LAST completed
    // frame's demand/served; LiveCount + OverflowTotal are cumulative/stable. Debug query only — it
    // blocks on its own submissions, so it is NOT for the per-frame path.
    CBTTessellationStats ReadTessellationStats();

    // Incremental vertices VertexEval evaluated on the last gated update (kWQVertexEvalCounter).
    // The perf oracle asserts a quiescent gated frame evaluates ~0, not the whole live pool.
    int32_t ReadVertexEvalCount() { return ReadWorkQueueCounter(kWQVertexEvalCounter); }

    // Debug: copy `wordCount` u32 words starting at `firstWord` of `binding` into a private
    // host-visible buffer and return them (self-contained submit + wait, state-restoring).
    // `wordCount` is clamped to what remains of the source past `firstWord`, so the returned
    // vector may be shorter than asked for (empty if `firstWord` is past the end). Diagnostics
    // only, but unbounded, so the decode golden tests can read every live bisector's geometry.
    std::vector<uint32_t> DebugReadWords(CBTBinding binding, uint32_t wordCount,
                                         uint32_t firstWord = 0);

  private:
    // One word of a render-loop-owned buffer, read through its region of the shared readback
    // buffer with a copy this call records itself.
    uint32_t ReadSharedReadbackWord(CBTBinding binding, uint64_t readbackOffset,
                                    uint64_t regionBytes, uint32_t word) const;

    void DispatchDirect(Rendering::CommandList& cl, CBTKernel kernel,
                        const CBTPushConstants& pc, uint32_t groupsX);
    // `dispatchSlot` selects which uvec3 of the indirect-dispatch buffer supplies the group count:
    // kDispatchSlotStage for the stage width PrepareIndirect just wrote, kDispatchSlotLive for the
    // frame's live-set width Reset seeded.
    void DispatchIndirectKernel(Rendering::CommandList& cl, CBTKernel kernel,
                                const CBTPushConstants& pc, uint32_t dispatchSlot);
    void PrepareIndirect(Rendering::CommandList& cl, CBTPushConstants pc, uint32_t counterSlot);
    void RecordDataBarrier(Rendering::CommandList& cl);
    void RecordReduce(Rendering::CommandList& cl, const CBTPushConstants& pc);
    // Content-aware split: rebuilds the height-range pyramid from this frame's height texture,
    // level by level, when the heights may have changed since the last build (`heightsChanged`:
    // a forced corner refresh or an edit rect) or another texture is bound. No-op when the
    // content-aware split is off for this frame (the threshold is 0, the atlas source, a
    // spherical domain, the narrow-heap arm, a lattice too large for the buffer).
    void RecordHeightRangeBuild(Rendering::CommandList& cl, const CBTClassifyDesc& classify,
                                const CBTFrameParams& params, CBTPushConstants pc,
                                uint32_t frameCounter, bool heightsChanged);
    void EnsureDispatchUAV(Rendering::CommandList& cl);
    void EnsureDispatchIndirect(Rendering::CommandList& cl);

    Rendering::IDevice* m_Device = nullptr;
    const CBTKernelSet* m_KernelSet = nullptr;
    CBTResources m_Resources;
    std::vector<Rendering::ResourceBarrier> m_DataBarriers;
    uint32_t m_PoolSize = 0;
    // Tracks the indirect-dispatch buffer state so the UAV<->IndirectArgs
    // transitions around DispatchIndirect derive the right stage/access masks.
    bool m_DispatchInIndirectState = false;
    // Neighbor ping-pong parity: true = NeighborsA is CURRENT (read) / B is NEXT
    // (written). Flipped at the end of every RecordUpdate (port-notes §8).
    bool m_NeighborsReadIsA = true;
    bool m_Ready = false;
    // Whether RecordUpdate dispatches the whole-pool debug Validate kernel (default on for tests;
    // the shipping render feature disables it — see SetValidateEachUpdate).
    bool m_ValidateEachUpdate = true;
    // Active domain, set by InitializeRoots and fed into every RecordUpdate's push
    // constants. Planar: baseDepth 1, 2 roots. Spherical: baseDepth 5, 24 roots.
    uint32_t m_DomainMode = kDomainPlanar;
    uint32_t m_BaseDepth = kDefaultBaseDepth;
    uint32_t m_RootCount = kRootHalfedgeCount;
    // The identity index + draw-count buffers are content- and domain-independent and
    // settle into their graphics-read states on the first InitializeRoots. A re-seed
    // (domain switch) re-zeroes/re-seeds the SoA buffers but must NOT re-run that
    // one-time transition (they are no longer in CopyDest). Guards it.
    bool m_DrawResourcesInitialized = false;
    bool m_VerticesInitialized = false;
    // The height texture the pyramid was last built from (invalid = never built).
    Rendering::TextureHandle m_HeightRangeSource{};
};

} // namespace GameEngine::CBTTerrain
