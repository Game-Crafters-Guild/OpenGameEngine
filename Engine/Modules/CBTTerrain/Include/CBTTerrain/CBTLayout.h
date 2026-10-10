#pragma once

// CBTLayout.h — authoritative CPU-side memory contract for the concurrent
// binary tree (CBT/LEB) terrain renderer. Every constant, struct size and
// offset declared here is mirrored bit-for-bit in Shaders/cbt_layout.glsl; the
// two files must be edited in lockstep. The static_asserts at the bottom fail
// the build if a struct drifts from its std430 GPU counterpart.
//
// Reference implementation this layout is ported from:
//   github.com/AnisB/large_cbt (Benyoub & Dupuy 2024)

#include <cstddef>
#include <cstdint>

namespace GameEngine::CBTTerrain
{

// ---------------------------------------------------------------------------
// Pool sizing
// ---------------------------------------------------------------------------
// 128k bisectors == 2^17 physical slots. The pool is indexed by SLOT (physical
// storage index 0..P-1), which is distinct from a bisector's HeapID (its LEB
// subdivision-matrix index). The occupancy bitfield tracks which of the P slots
// are live. P MUST be a power of two and a multiple of 32 (u32 bitfield word).
inline constexpr uint32_t kDefaultBisectorPoolSize = 1024u * 1024u; // 2^20 = 1048576 (1M pool bump)

// Mirror of CBT_POOL_SIZE in cbt_layout.glsl, which hardcodes its own literal. The
// GLSL draw-arg clamp bounds the indirect indexCount at 3*CBT_POOL_SIZE
// (cbt_kernels.comp, kMaxIndexCount) while the identity index buffer is sized
// 3*poolSize on the C++ side (CBTResources rejects any non-default poolSize, so at
// runtime poolSize == kDefaultBisectorPoolSize). If these two pool constants ever
// diverge, the clamp would permit an index fetch past the end of the identity
// buffer -> OOB read. This static_assert keeps the C++ side locked; the GLSL side
// must be kept in sync by hand (there is no compile-time channel from GLSL to C++).
inline constexpr uint32_t kGlslCbtPoolSize = 1048576u;
static_assert(kGlslCbtPoolSize == kDefaultBisectorPoolSize,
              "kDefaultBisectorPoolSize must equal CBT_POOL_SIZE in cbt_layout.glsl — the identity "
              "index buffer (3*poolSize) must cover the shader draw clamp (3*CBT_POOL_SIZE)");

// Unit-square base mesh: two triangles (lower-left + upper-right) that tile the
// [0,1]^2 quad and are twins across their shared hypotenuse (the diagonal from
// (1,0) to (0,1) — each triangle's LONGEST edge, which is the LEB bisection
// edge). The two legs of each triangle are domain boundaries (no neighbor).
// This is the canonical conforming LEB base; C1's 4-slot placeholder ring was
// not a valid conforming triangulation, so the full neighbor fan-out could not
// keep link reciprocity (Validate) green under refinement. baseDepth = log2(2).
inline constexpr uint32_t kRootHalfedgeCount = 2u;
inline constexpr uint32_t kDefaultBaseDepth = 1u; // log2(kRootHalfedgeCount)

// Maximum subdivision the LEB decode is allowed to reach (numSubdiv = depth -
// baseDepth). Kernel_VertexEval walks the HeapID path bits into an integer barycentric
// triple and forms the cube/UV numerator N = bary·rootCorners as an EXACT int64 before a
// single float cast (decode-precision slice 1). Because N is a bit-identical integer from
// both sides of a shared edge, the projection is crack-free by construction to numSubdiv
// ~52 (scale = 2^numSubdiv < 2^63, the int64 walk never overflows). The USEFUL ceiling is
// the fp32 world-corner storage ULP (√3·R·2⁻²³ → degeneracy at ~42, radius-independent);
// 40 leaves ~2× anti-degeneracy margin and lifts the min planet facet edge from ~7 m to
// ~9 mm at R=20 km. The prior cap was 23 — the point below which every bary < 2^24 keeps
// the float sum exact WITHOUT int64; int64 both raises that ceiling and clears the int32
// walk-overflow wall at numSubdiv 31 (the shipped `int scale = 1<<numSubdiv` wrapped).
// Requires shaderInt64, already mandatory for the u64 HeapID: a device without it disables
// CBT terrain wholesale (CBTRenderFeature), so there is no reduced-precision planet path to
// fall back to. Mirrored in cbt_layout.glsl as CBT_MAX_NUM_SUBDIV and locked to it by the
// CBTLayout.GlslMaxDecodeSubdivMatchesCpp test (no compile-time channel from GLSL to C++).
// This is the FP32-WORLD-STORE cap: the spherical deep (sector, local) store lifts its cap
// to DeepDecode::kDeepDecodeSubdiv (50) via the flag-coupled provisioning derivation
// (TerrainProvisioning SubdivCapFor, decode-precision arc S2b) — never lift THIS constant
// without a storage representation that carries the depth (design §1: the store error is
// radius-bound, ~0.25 m at Earth, and crosses 25% of the facet at numSubdiv ~46).
inline constexpr uint32_t kMaxDecodeSubdiv = 40u;

// The cap on the NARROW-HEAP arm (GE_CBT_HEAP32 — targets with no 64-bit integers at all;
// WGSL/WebGPU is the one that forced it). There the heap ID is a u32, so the absolute
// ceiling is depth = baseDepth + numSubdiv <= 31, and the deepest base mesh is the
// cube-sphere's kSphereBaseDepth (5); the barycentric walk's scale = 1 << numSubdiv must
// also stay inside a signed 32-bit integer. 25 clears both with margin. Mirrored in
// cbt_layout.glsl as CBT_MAX_NUM_SUBDIV_EFFECTIVE and locked by the same
// CBTLayout.GlslMaxDecodeSubdivMatchesCpp test as the wide cap. Costs planet detail, not
// planar detail: a planar terrain refines to its heightfield sample spacing, which lands
// well under this, while an Earth-scale sphere's facet floor rises from ~0.25 m to metres.
inline constexpr uint32_t kHeap32DecodeSubdiv = 25u;

// One workgroup width for every per-bisector kernel. Singleton kernels (Reset,
// PrepareIndirect, ReduceSecondPass) dispatch (1,1,1) and guard on the global
// invocation id so only the threads with work execute.
inline constexpr uint32_t kComputeWorkgroupSize = 64u;

// ---------------------------------------------------------------------------
// Sentinels / enums (from reference bisector.hlsl)
// ---------------------------------------------------------------------------
inline constexpr uint32_t kInvalidPointer = 0xFFFFFFFFu;
inline constexpr uint64_t kFreeSlotHeapID = 0u; // HeapID 0 == free slot (port-notes §10 Q6)

// Classify focus sentinel: FocusRoot == kFocusRootAll drives a uniform target
// depth over the whole tree (see CBTPushConstants::FocusRoot).
inline constexpr uint32_t kFocusRootAll = 0xFFFFFFFFu;

// Subdivision-pattern bitmask (BisectorData.SubdivisionPattern). 3 refinement
// bits: CENTER(0x1) | RIGHT(0x2) | LEFT(0x4). Compositions are DOUBLE/TRIPLE.
inline constexpr uint32_t kNoSplit = 0x0u;
inline constexpr uint32_t kCenterSplit = 0x1u;
inline constexpr uint32_t kRightSplit = 0x2u;
inline constexpr uint32_t kRightDoubleSplit = 0x3u; // CENTER | RIGHT
inline constexpr uint32_t kLeftSplit = 0x4u;
inline constexpr uint32_t kLeftDoubleSplit = 0x5u; // CENTER | LEFT
inline constexpr uint32_t kTripleSplit = 0x7u;      // CENTER | RIGHT | LEFT

// Bisector classification state (BisectorData.BisectorState). These are ENUM
// values, NOT bitfield positions — never OR them together (port-notes §5).
inline constexpr uint32_t kStateUnchanged = 0u;
inline constexpr uint32_t kStateBisect = 1u;
inline constexpr uint32_t kStateSimplify = 2u;
inline constexpr uint32_t kStateMerged = 3u;

// Draw-stream binning flags (BisectorData.Flags).
inline constexpr uint32_t kFlagVisible = 0x1u;
inline constexpr uint32_t kFlagModified = 0x2u;

// Classify metric selector (CBTPushConstants::ClassifyMode). DepthTarget is the
// deterministic C2/C3 depth-band metric (no camera) kept for the unit tests and
// bring-up; ScreenSpace is the C4 production metric (projected edge length vs a
// target pixel error). Mirrors CBT_CLASSIFY_* in cbt_layout.glsl. Plan §8 C4.
inline constexpr uint32_t kClassifyDepthTarget = 0u;
inline constexpr uint32_t kClassifyScreenSpace = 1u;

// Domain selector (CBTPushConstants::DomainMode). Planar is the pre-C7 unit-square +
// heightmap terrain; Spherical is the C7 cube-sphere (6 faces x 4 pie-slice roots).
// Mirrors CBT_DOMAIN_* in cbt_domain.glsl. Plan §8 C7.
inline constexpr uint32_t kDomainPlanar = 0u;
inline constexpr uint32_t kDomainSpherical = 1u;

// Per-frame params UBO ring depth. Sized to Rendering::IDevice::kMaxSupportedFramesInFlight,
// the backend-independent upper bound on frames in flight, so the ring is valid for the
// deepest-pacing backend. On a backend that paces fewer frames than that (Vulkan: 3) the
// surplus element is the write-after-fence margin stated at CBTFrameRingSlot. Mirrors
// CBT_FRAME_RING in cbt_layout.glsl.
inline constexpr uint32_t kCBTFrameParamsRing = 4u;

// The ring depth of the three heightmap TEXTURE bindings (15/18/19), which is not always
// the params ring depth. A descriptor array needs a backend that has them: WebGPU exposes
// no binding arrays, so there the bindings are single and the CPU rewrites element 0 every
// frame. That is safe precisely where the array is impossible — a bind group is rebuilt
// on every descriptor write, so no in-flight frame is left reading the element being
// rewritten, which is the whole reason the ring exists elsewhere. Mirrors
// CBT_TEX_RING_DECL / CBT_RING in cbt_layout.glsl.
inline constexpr uint32_t kCBTTextureRingMax = kCBTFrameParamsRing;
// The narrow-heap arm (CBT_DECODE_INT32 in cbt_layout.glsl) declares each texture as
// one element; the wide arm declares the ring. CBTResources sizes its bound-texture
// bookkeeping to the maximum and walks only the elements the loaded kernels declare.
constexpr uint32_t CBTTextureRingFor(bool narrowHeap)
{
    return narrowHeap ? 1u : kCBTFrameParamsRing;
}

// The device runs the narrow-heap arm when it compiles WGSL (per-kernel programs) or has no
// 64-bit shader integers. CBTKernelSet::Initialize loads that arm; the height pages read it to
// stream nothing on an arm that binds no page table.
constexpr bool CBTDeviceRunsNarrowArm(bool wgslSource, bool supportsShaderInt64)
{
    return wgslSource || !supportsShaderInt64;
}

// The ring element frame `frameCounter` reads and writes — the single CPU-side reduction;
// the GPU mirrors it as `pc.frameIndex % CBT_FRAME_RING` on the value CBTInstance pushes
// (locked by CBTLayout.GlslFrameRingMirrorsCpp), so both sides land on the same element.
//
// `frameCounter` is an UNWRAPPED, monotonically increasing frame number — see
// CBTResources::AdvanceFrameCounter, which derives it from Rendering::DeviceFrameCounter.
// It is NEVER IDevice::GetFrameIndex(): that reports a slot in [0, framesInFlight), a
// domain too narrow to address this ring's last element, which would collapse the
// effective rotation onto the device's pacing and spend the margin below.
//
// ORDERING. The CPU write to an element happens during graph record, after the device's
// BeginFrame has waited the fence for frame F-framesInFlight. Rotating one element per
// frame over R elements, the write at frame F is the next touch of an element last used at
// F-R, so the margin over the fenced frame is R - framesInFlight:
//   R == framesInFlight (what reducing the device's wrapped index gives): margin ZERO. The
//     write lands on the element the JUST-retired frame used. Correct only while the
//     fence-before-write ordering is exact and no consumer reads an earlier frame's element
//     — CBT's kernels read only pc.frameIndex's own element today, so this held, which is
//     why restoring the rotation is hardening and not a bug fix.
//   R == framesInFlight + 1 (this ring on Vulkan): the element's last reader retired a full
//     frame before the write, so a consumer that reads the previous frame's element, or a
//     write that escapes the fence, still has a frame of headroom.
inline constexpr uint32_t CBTFrameRingSlot(uint32_t frameCounter)
{
    return frameCounter % kCBTFrameParamsRing;
}

// A power-of-two ring divides 2^32 evenly, so the counter's own wrap keeps the rotation
// seamless rather than replaying an element early.
static_assert((kCBTFrameParamsRing & (kCBTFrameParamsRing - 1u)) == 0u,
              "kCBTFrameParamsRing must be a power of two for CBTFrameRingSlot to survive "
              "the frame counter's 2^32 wrap");

// The same reduction for the TEXTURE bindings, which do not always ring (CBTTextureRingFor):
// a collapsed ring has one element and every frame rewrites it.
inline constexpr uint32_t CBTTextureRingSlot(uint32_t frameCounter, uint32_t textureRing)
{
    return frameCounter % textureRing;
}

// Analytic sphere modifiers (sculpt shape-accuracy S2): the bounded closed-form placement set
// uploaded per frame in CBTFrameParams (compute) and CBTSurfaceParams (fragment). Closed-form
// primitives only (circular flattens today; noise/stamp/zone stay store-baked), so a planet's
// analytic count stays small; overflow beyond the cap falls back to the store bake. Mirrors
// CBT_MAX_SPHERE_ANALYTIC in cbt_analytic.glsl (locked by the GLSL parse test). Each entry packs
// as 3 vec4: [N.xyz | Radius], [E1.xyz | Falloff], [E2.xyz | TargetRadius].
inline constexpr uint32_t kMaxSphereAnalyticModifiers = 16u;
inline constexpr uint32_t kSphereAnalyticFloatsPerModifier = 12u;

// Per-cell placement culling (sculpt shape-accuracy S3): each cube face carries an 8x8 cell grid;
// the CPU publishes a 16-bit mask per cell of which placements (flattens then transient dabs, the
// shared slot order) have support intersecting that cell. The GPU chokepoints read one mask word
// for their sample point's cell and loop only the set bits — kills the all-N covers/eval loop per
// facet (the measured N=1 -> 8 CBT.Update driver). 6 faces x 64 cells x 16 bits, 2 cells/uint32 =
// 192 words (uvec4[48] std140, 768 B appended to CBTFrameParams). Mirrors CBT_ANALYTIC_CELL_GRID
// in cbt_layout.glsl (locked by the GLSL parse test).
inline constexpr uint32_t kSphereAnalyticCellGrid = 8u;
inline constexpr uint32_t kSphereAnalyticCellCount = 6u * kSphereAnalyticCellGrid * kSphereAnalyticCellGrid;
inline constexpr uint32_t kSphereAnalyticCellMaskWords = kSphereAnalyticCellCount / 2u;

// ---------------------------------------------------------------------------
// SoA GPU structs (std430)
// ---------------------------------------------------------------------------
// Neighbors are stored as uvec4 rather than uvec3 because std430 pads a uvec3
// to 16 bytes anyway — declaring the padding explicitly keeps host/GPU stride
// in agreement (port-notes §9). Order is (neighbor0, neighbor1, twin, unused)
// with twin == .z, matching the reference exactly (port-notes §10 Q2).
struct CBTNeighbors
{
    uint32_t Neighbor0 = kInvalidPointer;
    uint32_t Neighbor1 = kInvalidPointer;
    uint32_t Twin = kInvalidPointer;
    uint32_t Unused = 0u;
};

// Per-bisector scalar payload. Field order is hot-fields-front (pattern/state/
// flags grouped) rather than the reference's (pattern, indices[3], ...) so
// Classify touches one cache line; functionally equivalent because the SSBO is
// RW-shared across all kernels (port-notes §10 Q12). 8 x u32 = 32 bytes.
struct CBTBisectorData
{
    uint32_t SubdivisionPattern = kNoSplit;
    uint32_t BisectorState = kStateUnchanged;
    uint32_t Flags = 0u;
    uint32_t ProblematicNeighbor = kInvalidPointer;
    uint32_t PropagationID = kInvalidPointer;
    uint32_t Indices[3] = {kInvalidPointer, kInvalidPointer, kInvalidPointer};
};

// Per-bisector triangle corners, written by the vertex-eval kernel and read by
// Classify (screen-space metric) and the graphics vertex modifier. C4: each corner
// carries the evaluated position (xyz, height sampled from the terrain heightmap
// baked into y) + uv.x in .w; Meta holds the three uv.y + the terrain normalmap
// bindless index (the graphics surface samples that normalmap per-pixel for a
// smooth normal, matching CDLOD).
//
// Earth-scale (sector, local) storage (decode-precision arc S2a): fp32
// WORLD corners quantize on a ~0.5 m grid at Earth magnitude AT STORE TIME, before
// any camera-relative consumption can help — the measured wall the S1 probes
// pinned. The cure appends a per-corner integer SECTOR anchor on the engine's
// 1024 m world-sector grid (the same representation instanced meshes use —
// camera_relative.glsl GE_ClipFromSectorLocal / Components::WorldSectorCoord):
//   DeepTag[0] == 0 (legacy, the dark-ship default): Corner*.xyz is the fp32
//     WORLD position exactly as before; the sector tail is written zero and never
//     read. Byte-identical to the pre-S2a pipeline.
//   DeepTag[0] == 1 (GE_CBT_DEEP_DECODE, spherical only): Corner*.xyz is the fp32
//     SECTOR-LOCAL offset (|component| <= 512 + relief spill, ULP ~6e-5 m at any
//     radius) and Sector<k> packs corner k's ivec3 sector as 3 x 16-bit
//     two's-complement lanes (range +/-32767 sectors ~ +/-3.36e7 m — 5x Earth
//     radius; locked by CBTLayoutTests.DeepSectorPackingContract). Readers
//     reconstruct rel = float(sector - originSector) * 1024 + local (exact
//     integer delta — the GE_ClipFromSectorLocal math verbatim).
// Corner*.w / Meta keep their meaning in BOTH modes (uv + normalmap index), so
// every UV consumer is mode-blind. 4 x vec4 + 4 x uvec2 = 96 bytes std430
// (was 64 — +32 MiB at the 1M pool, paid in both modes: the struct is compiled
// into every kernel, so a flag-conditional stride would need dual pipelines).
struct CBTVertexData
{
    float Corner0[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = world position (m) or sector-local offset (DeepTag), w = uv.x
    float Corner1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float Corner2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float Meta[4] = {0.0f, 0.0f, 0.0f, 0.0f};    // (uv0.y, uv1.y, uv2.y, normalmapBindlessIndex)
    uint32_t Sector0[2] = {0u, 0u}; // corner 0 sector: x = (sx & 0xFFFF) | (sy << 16), y = (sz & 0xFFFF)
    uint32_t Sector1[2] = {0u, 0u};
    uint32_t Sector2[2] = {0u, 0u};
    uint32_t DeepTag[2] = {0u, 0u}; // x: 0 = world corners (legacy), 1 = (sector, local); y reserved 0
};

// Pack one signed sector component into a 16-bit two's-complement lane / recover it.
// CPU twin of CBT_PackCornerSector / CBT_UnpackCornerSector in cbt_layout.glsl (the
// GPU packers are the authority; these exist for the readback tests + physics mirror).
inline uint32_t PackSectorPair(int32_t lo, int32_t hi)
{
    return (static_cast<uint32_t>(lo) & 0xFFFFu) | (static_cast<uint32_t>(hi) << 16);
}
inline int32_t UnpackSectorLo(uint32_t v) { return static_cast<int32_t>(v << 16) >> 16; }
inline int32_t UnpackSectorHi(uint32_t v) { return static_cast<int32_t>(v) >> 16; }

// Per-frame camera + terrain params consumed by Classify (screen-space error) and
// VertexEval (world-space height displacement). std140 mirror of CBTFrameParams in
// cbt_layout.glsl — mat4 + 13 vec4 + the 16 x 3 vec4 analytic modifier set = 1040
// bytes, ring-buffered on the GPU. All floats (GPU-upload-ready, column-major
// matrix). Plan §8 C4; Phase E atlas params + Earth-scale render origin +
// view-priority near-bias + walking-headroom demand tuning + round-8c screen-area
// priority ordering + water plane + S2 analytic sphere modifiers appended.
struct CBTFrameParams
{
    // Rebased (render-origin-relative) world -> clip (reverse-Z LH). Classify's frustum
    // + screen-space metric subtract RenderOriginSector from each world corner and project
    // the small camera-relative result through this, so the big·big cancellation that
    // swims a planetary surface never happens (Earth-scale slice 1b). Equals the full-world
    // viewProj when the render origin is inactive (sector 0) — the dark-ship path. Filled
    // from CameraData::viewProjRel, the SAME source the CameraUBO draw path reads, so
    // terrain and meshes agree on where the camera is (cross-pass agreement).
    float ViewProjRel[16] = {};
    float CameraPos[4] = {}; // xyz = camera world position (PLANET-CENTER space — the horizon cull needs true radii, NOT rebased)
    float Screen[4] = {};    // x=width px, y=height px, z=split threshold px, w=merge threshold px
    float TerrainSize[4] = {}; // x=sizeX, y=sizeZ, z=heightScale, w=originY
    float TerrainOrigin[4] = {}; // x=originX, y=originZ, z=maxDepth (float), w=normalmap bindless index
    float PlanetParams[4] = {};  // C7 spherical: x=radius, y=reliefAmplitude, z=reliefFrequency, w=unused
    // Phase E resident-window atlas geometry (design §3). AtlasParams1.z picks the planar
    // height source CBT_SampleHeight reads: 0 the single unified height texture (binding 15);
    // 1 the atlas, terrain UV -> tile -> slot through the indirection SSBO (binding 17) +
    // atlas height texture (binding 18) via cbt_atlas.glsl; 2 the paged resolve, the page
    // table (binding 22) + page cache (binding 23) via cbt_page.glsl. Integer geometry rides
    // as float (all < 2^23 -> exact).
    float AtlasParams0[4] = {}; // x=AtlasDim, y=SlotStride, z=SlotsPerRow, w=TileRes
    float AtlasParams1[4] = {}; // x=TilesPerAxisX, y=TilesPerAxisZ, z=height source (0 unified, 1 atlas, 2 paged), w=coarseDim (coarse field texels/axis)
    // Earth-scale render origin (slice 1b). xyz = the render origin sector (as float; the
    // camera's nearest kWorldSectorSize cell, exact integer < 2^23), w = sector size (m,
    // informational — the shader uses the fixed CBT_SECTOR_SIZE constant, never .w, so a
    // pass that leaves this zero collapses cleanly to the world path). Classify reconstructs
    // originWorld = xyz * CBT_SECTOR_SIZE (== camera_relative.glsl GE_RenderOriginWorld) and
    // subtracts it from each world corner before the rebased projection. Zero => inactive =>
    // the subtraction is an IEEE-exact no-op and every metric is byte-identical to today.
    float RenderOriginSector[4] = {};
    // View-prioritised split metric. Redistributes the fixed bisector
    // pool toward the camera when the pool is contended: near-camera bisectors keep the base
    // pixel-error threshold (fine) while the far field's threshold is scaled up (coarse), so a
    // saturated frame spends its budget where the camera is looking instead of uniformly across
    // the visible hemisphere. Classify shapes only the split/merge DEMAND (the Split kernel's
    // conformity propagation is untouched — forced splits still happen), so the CBT stays a
    // conforming triangulation. x = enabled (0/1 — the A/B toggle; 0 => byte-identical to the
    // pre-slice metric, no extra work); y = near radius (m, full detail inside); z = far radius
    // (m, max coarsening reached by here); w = max coarsen factor (the far-field threshold
    // multiplier, <= 1 => no coarsening). Gated in-shader by pool occupancy so an UNSATURATED
    // frame classifies exactly as today regardless of the toggle (the neutrality oracle).
    float NearBias[4] = {};
    // Walking-headroom demand shaping (walking-headroom slice — mirror cbt_layout.glsl demandTuning).
    // Consumed by Kernel_Classify only; every field 0 => byte-identical to the pre-slice metric.
    // x = near-field world facet target (m): bounds the behind-eye/straddle force-split so a facet at
    //     the eye plane stops at ~target metres instead of the decode cap (cap-depth near-plane detail
    //     is invisible over-refinement that saturates the walking pose). 0 = off.
    // y = off-frustum keep-depth occupancy ceiling [0,1]: an off-frustum bisector keeps its depth while
    //     occupancy is below this (no yaw re-refine churn) and coarsens to base only under pool pressure.
    //     0 = off (always coarsen-to-base, the pre-slice frustum gate).
    // z = flat-disc edge-rescue projected-edge threshold (px): a near facet the AREA metric under-refines
    //     (grazing/flattened, projected area collapsed) still splits while its projected LEB edge exceeds
    //     this, and a rescued parent's children hold their merge while the parent's projected split edge
    //     still exceeds it (the rescue's merge counterpart: a diamond never both re-rescues and merges).
    //     Self-limiting. 0 = off.
    // w = edge-rescue occupancy ceiling [0,1]: the rescue and its merge hold fire only while occupancy is
    //     below this. 0 = off.
    float DemandTuning[4] = {};
    // Screen-area priority ordering (round-8c look-back — mirror cbt_layout.glsl priorityParams).
    // Orders the fixed pool's split budget by ON-SCREEN facet area under saturation so the largest
    // (most jarring) facets refine FIRST — the fix for "look away, look back, the plateau rebuilds
    // slowly with artifacts" (tiny far facets were winning slots as often as the huge near blob).
    // Kernel_Classify raises a projected-area floor on split demand as live occupancy climbs a wide
    // contention ramp (the same low-gain, no-oscillation shaping the near-bias uses), so an
    // unsaturated pool imposes no floor. Spherical-domain only. x=0 => byte-identical to the metric.
    //   x = enabled (0/1 — the A/B toggle).
    //   y = contention ramp start occupancy [0,1] (below it the floor is 0 — neutrality).
    //   z = max area floor (px^2) reached at full saturation (bounds how aggressively small facets are
    //       shed; the ramp scales the floor from 0 to this over [y, ~1.0]).
    //   w = keep-large off-frustum band (NDC): a large just-off-frustum facet (recently in view — the
    //       plateau you glanced away from) retains its depth even under saturation while it sits within
    //       this NDC annulus of the frustum edge, so a quick glance away does not demote it. Bounded by
    //       the annulus width AND the rising area floor (deep-off-frustum / small facets still coarsen).
    //       0 = off (the pre-slice frustum gate, coarsen-under-pressure).
    float PriorityParams[4] = {};
    // Water plane (mirror cbt_layout.glsl waterPlane): the calm water surface a planar terrain sits
    // under. Seen from a camera above that surface, a bisector whose three corners all lie more than
    // the margin below it is hidden by the water, so Kernel_Classify scales its split AND merge pixel
    // thresholds by the coarsen factor (the near-bias mechanism — both thresholds, so the hysteresis
    // band is preserved) instead of refining the seabed to TargetPixelError. A corner within the
    // margin refines normally, which keeps the shoreline and the seabed still visible through
    // shallow water at full detail. Planar only; every field 0 => byte-identical to the metric.
    //   x = water surface height (world Y).
    //   y = submerged margin (m): the seabed within this depth of the surface counts as visible.
    //   z = coarsen factor applied to submerged bisectors (> 1 enables; 0 = off).
    //   w = the content-aware split threshold (px), the other planar Classify term riding this
    //       vector: a facet splits only while its height range projects above it, a pair merges
    //       once its parent's projects below half (cbt_kernels.comp). 0 = off.
    float WaterPlane[4] = {};
    // Analytic sphere modifiers (sculpt shape-accuracy S2 — mirror cbt_layout.glsl analyticParams /
    // sphereAnalytic). The bounded closed-form placement set evaluated AT SAMPLE TIME inside the
    // sculpt-sampling chokepoints, so a flatten renders as an exact circle at any planet radius
    // instead of the store-texel staircase (design §3.a). x = flatten count, y = transient brush
    // dab count (S3 — dabs occupy slots [x, x+y); integers as float, x+y <=
    // kMaxSphereAnalyticModifiers — exact under 2^23); z = the page-table ring's words per slot
    // (kCBTPageRingWordsLane, wide arm, exact under 2^24; written by CBTResources::UploadFrameParams,
    // not by the caller); w reserved 0. Both 0 (the
    // GE_TERRAIN_ANALYTIC_MODIFIERS-off dark-ship) skips every analytic branch, byte-identical.
    float AnalyticParams[4] = {};
    // kMaxSphereAnalyticModifiers entries x 3 vec4 each: flattens packed by
    // PackSphereAnalyticFlatten ([N.xyz | Radius], [E1.xyz | Falloff], [E2.xyz | TargetRadius]),
    // transient dabs by PackSphereAnalyticDab ([N.xyz | AngularRadius], [Amplitude, CosThreshold,
    // 0, 0], zero) in the slots after the flattens. PlanetRadius rides PlanetParams.x.
    float SphereAnalytic[kMaxSphereAnalyticModifiers * kSphereAnalyticFloatsPerModifier] = {};
    // S3 per-cell placement culling: 16-bit placement mask per face cell (2 cells per word, low
    // half = even cell), built by BuildSphereAnalyticCellMasks over the SAME slot order as
    // SphereAnalytic. On the GLSL side this tail is uvec4[48] in CBTFrameSlot BESIDE the frame
    // struct (std140-tight; same slot bytes) — NOT a CBTFrameParams member, because CBT_Frame()
    // copies the params by value per thread and folding the 768 B mask into that copy measured
    // ~7x on CBT.Update. All-zero when no placements exist (the dark-ship bytes).
    uint32_t AnalyticCellMask[kSphereAnalyticCellMaskWords] = {};
};

// ---------------------------------------------------------------------------
// Work-queue SSBO (single buffer, positional offset constants)
// ---------------------------------------------------------------------------
// Collapses the reference's _MemoryBuffer / _ClassificationBuffer /
// _AllocateBuffer / _PropagateBuffer / _SimplificationBuffer into one SSBO of
// signed int (port-notes §9: overflow of the memory budget is detected via a
// `< 0` test, so the element type is signed). Counters live in the first
// kWQCounterSlots elements; each work queue that follows holds up to P entries.
inline constexpr uint32_t kWQAllocCursor = 0u;              // MemoryBuffer[0]: bump alloc cursor
inline constexpr uint32_t kWQFreeCount = 1u;                // MemoryBuffer[1]: free-slot budget (signed)
inline constexpr uint32_t kWQSplitCounter = 2u;            // split candidates enqueued by Classify
inline constexpr uint32_t kWQSimplifyClassCounter = 3u;    // simplify candidates enqueued by Classify
inline constexpr uint32_t kWQAllocateCounter = 4u;         // allocations enqueued by Split
inline constexpr uint32_t kWQPropagateBisectCounter = 5u;  // bisect-propagate work enqueued by Bisect
inline constexpr uint32_t kWQPropagateSimplifyCounter = 6u; // simplify-propagate work enqueued by Simplify
inline constexpr uint32_t kWQSimplifyCounter = 7u;         // dense simplify queue built by PrepareSimplify
inline constexpr uint32_t kWQOverflowCounter = 8u;         // telemetry: Split reserve rollbacks (pool exhaustion)
// Per-pattern fan-out telemetry (which double/triple variants the frame exercised).
inline constexpr uint32_t kWQRightDoubleCounter = 9u;      // RIGHT_DOUBLE allocations
inline constexpr uint32_t kWQLeftDoubleCounter = 10u;      // LEFT_DOUBLE allocations
inline constexpr uint32_t kWQTripleCounter = 11u;          // TRIPLE allocations
// Incremental vertices VertexEval evaluated this frame (only when GateVertexEval is on) —
// the perf oracle asserts a quiescent frame evaluates ~0, not the whole live pool (plan §planet-shading).
inline constexpr uint32_t kWQVertexEvalCounter = 12u;
// Since-init telemetry (Reset does NOT clear it, same as the overflow/fan-out slots): the
// GPU-side dispatch-arg writers (Kernel_Reset, Kernel_PrepareIndirect) count here when a
// GPU-written element count exceeded the pool size and the dispatch width was clamped to
// ceil(pool / kComputeWorkgroupSize). No healthy frame can exceed that bound (every work
// queue holds at most pool entries and the sum-tree root is at most pool), so a non-zero
// value means a corrupted count was caught before it became an unbounded indirect dispatch
// (a multi-second GPU packet — the TDR / device-loss class). Read via ReadWorkQueueCounter.
inline constexpr uint32_t kWQDispatchClampCounter = 13u;
// Planar pool-pressure scale: current threshold-scale step (cbt_layout.glsl CBT_WQ_PRESSURE_STEP);
// scale = 2^(step / kPressureStepsPerOctave).
inline constexpr uint32_t kWQPressureStep = 14u;
inline constexpr uint32_t kPressureStepsPerOctave = 16u;
// Planar off-frustum keep step (cbt_layout.glsl CBT_WQ_OFF_FRUSTUM_KEEP_STEP): how far Kernel_Reset
// has collapsed the off-frustum keep band, 0 to kOffFrustumKeepSteps (CBTDemandTuning.h).
inline constexpr uint32_t kWQOffFrustumKeepStep = 15u;
inline constexpr uint32_t kWQCounterSlots = 16u;           // reserve 16 counter slots (padding headroom)

// Payload lanes — 3 ALIASED physical lanes over the 6 logical queues (slot-diet r2).
// The six work queues never all hold live data at once: each is written by one kernel
// and drained by the next, with a full WorkQueue UAV barrier between EVERY dispatch
// (CBTInstance::RecordDataBarrier). Their live intervals over the fixed per-frame kernel
// sequence (CBTInstance::RecordUpdate; stage = ordinal of the payload-touching dispatch,
// a barrier between each) are:
//
//   logical queue        written by (stage)     drained by (stage)      live span
//   Split                Classify   (1)          Split           (2)     [1,2]
//   SimplifyClass        Classify   (1)          PrepareSimplify (6)     [1,6]
//   Allocate             Split      (2)          Allocate/Bisect (3,4)   [2,4]
//   PropagateBisect      Bisect     (4)          PropagateBisect (5)     [4,5]
//   Simplify             PrepareSimplify (6)      Simplify        (7)     [6,7]
//   PropagateSimplify    Simplify   (7)          PropagateSimplify (8)   [7,8]
//
// Two queues may share one physical P-slot lane IFF their live spans are DISJOINT: the
// earlier queue's last read is strictly before the later queue's first write, so the
// barrier between them orders every read of the old occupant ahead of the first write of
// the new one (no RAW/WAR hazard). This is an interval-graph colouring; its max clique is
// 3 — at stage 2 {Split draining, Allocate filling, SimplifyClass held} and stage 4
// {Allocate draining, PropagateBisect filling, SimplifyClass held} — so 3 lanes is the
// PROVABLE minimum (fewer would force two overlapping spans onto one lane). The colouring:
//
//   LANE 0 : Split [1,2]  ->  PropagateBisect [4,5]  ->  Simplify [6,7]   (chain, pairwise disjoint)
//   LANE 1 : SimplifyClass [1,6]  ->  PropagateSimplify [7,8]             (disjoint: 6 < 7)
//   LANE 2 : Allocate [2,4]                                               (spans the bisect fill alone)
//
// Every queue keeps its OWN counter slot (kWQ*Counter above; Reset zeroes all counters each
// frame), so a lane's next occupant always fills from index 0 over the drained-and-barriered
// old payload — the aliasing lives purely at the payload offset. Each kernel that reads queue
// X and writes queue Y in the same dispatch is on DIFFERENT lanes here (verified above), so it
// never clobbers its own input. A mis-colouring that overlaps two live spans on one lane
// corrupts the split or merge payload and mis-drives Bisect/Simplify into wrong-slot topology
// edits — caught by the Validate link-reciprocity / zombie / budget oracle
// (CBTInstanceTest.WorkQueueLaneAliasingChurnStaysValid). Was 6 contiguous lanes (24 B/slot);
// 3 lanes is 12 B/slot — halves the work-queue footprint (−12 MiB at the 1M pool).
//
// Offsets are parameterised on the pool size P so a future variant pool can reuse them.
inline constexpr uint32_t kWQLaneCount = 3u;
constexpr uint32_t WQSplitQueueOffset(uint32_t p) { return kWQCounterSlots + 0u * p; }             // lane 0
constexpr uint32_t WQPropagateBisectQueueOffset(uint32_t p) { return kWQCounterSlots + 0u * p; }   // lane 0
constexpr uint32_t WQSimplifyQueueOffset(uint32_t p) { return kWQCounterSlots + 0u * p; }          // lane 0
constexpr uint32_t WQSimplifyClassQueueOffset(uint32_t p) { return kWQCounterSlots + 1u * p; }     // lane 1
constexpr uint32_t WQPropagateSimplifyQueueOffset(uint32_t p) { return kWQCounterSlots + 1u * p; } // lane 1
constexpr uint32_t WQAllocateQueueOffset(uint32_t p) { return kWQCounterSlots + 2u * p; }          // lane 2
constexpr uint32_t WQElementCount(uint32_t p) { return kWQCounterSlots + kWQLaneCount * p; }

// ---------------------------------------------------------------------------
// OCBT bitfield + sum tree — u32 leaf-word re-derivation (plan §6)
// ---------------------------------------------------------------------------
// The reference stores the occupancy bitfield as uint64_t[] and bakes the
// ocbt_128k level-offset constants for 64-bit leaf words. We store it as u32[]
// so every atomic is a core-GLSL u32 op (Metal/MoltenVK has no buffer int64
// atomics). That forces re-derivation of the sum-tree geometry for 32-bit leaf
// words. Derivation, for pool P (power of two, multiple of 32):
//
//   bitfield words        W  = P / 32                (reference used P / 64)
//   leaf sum nodes        L  = W = P / 32            (one popcount per 32-bit word)
//   leaf packed words     LP = L / 4                 (4 byte-counts packed per u32;
//                                                     popcount(u32) <= 32 fits a byte)
//   sum-tree node count   N  = 2*L - 1               (full binary heap over L leaves)
//   leaf level offset     LO = L - 1                 (binary heap: level d at 2^d - 1)
//   sum-tree depth        D  = log2(L)
//
// The reduction is three kernels (matching the reference's split-for-perf
// structure):
//   ReducePrePass   Dispatch(ceil(LP / WG))  bitfield u32 -> packed popcount bytes
//   ReduceFirstPass Dispatch(ceil(L  / WG))  unpack packed bytes into leaf level, then each
//                                            workgroup sums its WG leaves up to level 11
//   ReduceSecondPass Dispatch(1)             single-workgroup up-sweep level 11 -> root in
//                                            workgroup memory; root == live count
//
// vs the reference's u64 numbers: W and L double (P/32 not P/64), so ReducePrePass
// issues 2x the bitCount() calls over the same bit span — measured-irrelevant next
// to the memory traffic (plan §6). Leaf-level nodes are stored packed (4/u32); the
// up-sweep levels above are full u32 because their sums exceed a byte immediately.
constexpr uint32_t CBTBitfieldWords(uint32_t p) { return p / 32u; }
constexpr uint32_t CBTLeafNodeCount(uint32_t p) { return p / 32u; }
constexpr uint32_t CBTLeafPackedWords(uint32_t p) { return (p / 32u) / 4u; }
constexpr uint32_t CBTSumTreeNodeCount(uint32_t p) { return 2u * (p / 32u) - 1u; }
constexpr uint32_t CBTLeafLevelOffset(uint32_t p) { return (p / 32u) - 1u; }

// log2 for compile-time depth constants (p is a power of two).
constexpr uint32_t CBTLog2(uint32_t v)
{
    uint32_t r = 0u;
    while ((v >>= 1u) != 0u)
        ++r;
    return r;
}
constexpr uint32_t CBTSumTreeDepth(uint32_t p) { return CBTLog2(CBTLeafNodeCount(p)); }

// The sum-tree buffer packs the packed-leaf words first, then the full-u32
// heap. Kernels index the heap via kSumTreeHeapBase.
constexpr uint32_t CBTSumTreeHeapBase(uint32_t p) { return CBTLeafPackedWords(p); }
constexpr uint32_t CBTSumTreeWords(uint32_t p) { return CBTLeafPackedWords(p) + CBTSumTreeNodeCount(p); }

// ---------------------------------------------------------------------------
// Indirect args / draw streams
// ---------------------------------------------------------------------------
// Indirect-dispatch args: 3 uvec3 slots reused across the frame (port-notes §7).
inline constexpr uint32_t kIndirectDispatchSlotCount = 3u;
// Slot 0 — the CURRENT stage's element width. Rewritten several times per frame: by Reset for
// Classify, then by each PrepareIndirect immediately before the stage that consumes it.
inline constexpr uint32_t kDispatchSlotStage = 0u;
// Slot 1 — the frame's LIVE-set width, seeded once by Reset from the sum-tree root and left
// alone for the rest of the frame. The neighbor carry-over dispatches from it because it runs
// after the split path has already rewritten slot 0 with the split/allocate widths.
inline constexpr uint32_t kDispatchSlotLive = 1u;
// Slot 2 — modified-vertex regeneration width, written by PrepareBisectorIndirect and consumed by
// gated VertexEval. A quiescent tree has zero workgroups here.
inline constexpr uint32_t kDispatchSlotModified = 2u;
inline constexpr uint32_t kIndirectDispatchWords = kIndirectDispatchSlotCount * 3u; // 9 u32
constexpr uint32_t IndirectDispatchSlotByteOffset(uint32_t slot) { return slot * 3u * 4u; }

// Indirect-draw: three 5-word VkDrawIndexedIndirectCommand records (all /
// visible / modified). Layout per record: {indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance}. BisectorIndexation atomicAdds 3 to
// indexCount per live bisector (3 indices/triangle); Reset seeds instanceCount=1.
inline constexpr uint32_t kDrawStreamCount = 3u;
inline constexpr uint32_t kDrawStreamStride = 5u; // u32 per VkDrawIndexedIndirectCommand
inline constexpr uint32_t kDrawStreamAll = 0u;
inline constexpr uint32_t kDrawStreamVisible = 1u;
inline constexpr uint32_t kDrawStreamModified = 2u;
inline constexpr uint32_t kIndirectDrawWords = kDrawStreamCount * kDrawStreamStride; // 15 u32
inline constexpr uint32_t kDrawIndexCountField = 0u;   // offset within a record
inline constexpr uint32_t kDrawInstanceCountField = 1u;
constexpr uint32_t DrawStreamWordOffset(uint32_t stream) { return stream * kDrawStreamStride; }

// C3 indexed-indirect draw contract. The world/forward pass consumption sites
// only record DrawIndexedIndirectCount (indexed draws), so the CBT draw rides
// that path with (1) the ALL-stream record at byte offset 0 as the indirect
// command source, (2) a one-element count buffer holding 1 (draw exactly the
// ALL record), and (3) an identity index buffer (index[i] = i) so gl_VertexIndex
// walks 0..indexCount-1 — the vertex modifier maps vertexID -> (triangle, corner)
// -> bisector slot via IndicesAll. Plan §8 C3, open-question #2.
inline constexpr uint32_t kIndirectDrawStrideBytes = kDrawStreamStride * 4u; // 20 = sizeof(VkDrawIndexedIndirectCommand)
inline constexpr uint32_t kDrawStreamAllByteOffset = kDrawStreamAll * kIndirectDrawStrideBytes; // 0
inline constexpr uint32_t kDrawCountValue = 1u;        // one indirect draw command
constexpr uint32_t IdentityIndexCount(uint32_t p) { return 3u * p; } // 3 indices per bisector triangle

// Validation SSBO: the debug validation kernels write independent error counters
// so a failing test can tell WHICH invariant broke. Mirror of the CBT_VALIDATION_*
// slots in cbt_layout.glsl (locked by CBTLayoutTests.GlslValidationCountersMatchCpp).
inline constexpr uint32_t kValidationErrorCounter = 0u;  // neighbor link non-reciprocity
inline constexpr uint32_t kValidationBudgetCounter = 1u; // FreeCount went negative
inline constexpr uint32_t kValidationZombieCounter = 2u; // bitfield bit set XOR HeapID != 0
inline constexpr uint32_t kValidationCompactCounter = 3u; // IndicesAll[0..liveCount) != the live set
inline constexpr uint32_t kValidationWords = 16u;

// ---------------------------------------------------------------------------
// Descriptor bindings (compute set 0)
// ---------------------------------------------------------------------------
enum class CBTBinding : uint32_t
{
    HeapID = 0,          // uint64_t[P]
    NeighborsA = 1,      // uvec4[P] (ping-pong pair with NeighborsB)
    NeighborsB = 2,      // uvec4[P]
    BisectorData = 3,    // CBTBisectorData[P]
    Bitfield = 4,        // uint[P/32] occupancy bits
    SumTree = 5,         // uint[CBTSumTreeWords(P)] packed leaves + heap
    WorkQueue = 6,       // int[WQElementCount(P)] counters + queues
    IndirectDispatch = 7, // uint[9] uvec3 x3 dispatch args
    IndirectDraw = 8,    // uint[15] VkDrawIndexedIndirectCommand x3
    IndicesAll = 9,      // uint[P] all-bisector index stream
    IndicesVisible = 10, // uint[P] visible-only index stream
    IndicesModified = 11, // uint[P] modified-only index stream
    CurrentVertex = 12,  // CBTVertexData[P]
    Validation = 13,     // uint[16]
    Count = 14
};
// The 14 CBTBinding entries are the SoA storage buffers (per-frame UAV-barriered as
// a block, GPU-zero-filled at init). kCBTBindingCount bounds those loops.
inline constexpr uint32_t kCBTBindingCount = static_cast<uint32_t>(CBTBinding::Count);

// C4 adds two non-storage-buffer bindings after the 14 SoA buffers: a per-frame
// params UBO and the terrain height texture (combined image sampler). They are
// kept OUT of the CBTBinding enum so the storage-buffer loops (zero-fill, UAV
// barriers) never touch them; the descriptor-set layout appends them explicitly.
inline constexpr uint32_t kCBTFrameParamsBinding = 14;    // UniformBuffer: CBTFrameParams[ring]
inline constexpr uint32_t kCBTHeightTextureBinding = 15;  // CombinedImageSampler: terrain heightmap

// Planet-editing v2 — SPARSE VIRTUAL SCULPT PAGE-TABLE. The editable sphere height source decouples
// VIRTUAL resolution (radius-scaled, sets quality) from PHYSICAL storage (content-scaled,
// sets memory). A per-face virtual grid of runtime dim Dv = pagesPerAxis * kSculptPageDim
// (derived from a meters-per-texel budget at the planet radius — DeriveSculptVirtualDim,
// SphereSculptPaging.h) is addressed through a per-face PAGE TABLE (binding 20) into a fixed
// PHYSICAL PAGE POOL (binding 16). Pages materialize on first non-zero write; an unallocated
// page reads additive 0 (the "no edits" fast path). Physical cost is radius-INDEPENDENT: the
// pool is sized to a working-set budget, not to Dv^2, so a 50 km planet costs the same as a
// 2 km one. The flat 256^2/face atlas grew linearly with radius (~123 m/texel at 20 km — the
// sub-texel invisible-brush case) and a fixed-quality flat scale-up needed ~9.25 GB at 50 km.
// Both SSBOs keep the host-visible write-once-per-frame ring discipline (no staging/barrier —
// the class of bug that caused the tiled-upload device-losts). Sphere VertexEval samples the
// pool through the table (paged bilinear) only when SphereSculptEnabled; planar never touches
// them. Kept OUT of CBTBinding for the same reason as 14/15 (no zero-fill/UAV-barrier).
inline constexpr uint32_t kCubeFaceCount6 = 6u; // 6 cube faces (matches CBTSphereRoots kCubeFaceCount)

// A physical page is kSculptPageDim^2 texels (128^2 R32F = 64 KiB). The page table maps up to
// kSculptMaxPagesPerFaceAxis pages/axis per face (128 -> Dv up to 16384 -> R up to ~83 km at
// 8 m/texel); a face's table region is a fixed cap*cap grid (row stride = cap) so the shader
// indexes it without knowing the per-planet pagesPerAxis. kSculptNoPage marks an unallocated
// virtual page (reads 0). Mirrored in cbt_layout.glsl as CBT_SCULPT_* and locked by
// CBTLayoutTests.GlslSculptPageConstantsMatchCpp.
inline constexpr uint32_t kSculptPageDim = 128u;
inline constexpr uint32_t kSculptPageTexels = kSculptPageDim * kSculptPageDim; // 16384
// Words per ring slot of the placeholder sculpt rings a planar tree binds (CBTResources): 256 B, a
// whole minStorageBufferOffsetAlignment step on every supported device, so the per-slot offset bind
// is valid. Nothing reads them.
inline constexpr uint32_t kSculptPlaceholderSlotWords = 64u;
inline constexpr uint32_t kSculptMaxPagesPerFaceAxis = 128u;
inline constexpr uint32_t kSculptPageTableFaceStride =
    kSculptMaxPagesPerFaceAxis * kSculptMaxPagesPerFaceAxis; // entries reserved per face (cap^2)
inline constexpr uint32_t kSculptPageTableEntries = kSculptPageTableFaceStride * kCubeFaceCount6;
inline constexpr uint32_t kSculptNoPage = 0xFFFFFFFFu;

// Adaptive page LEVELS (sculpt shape-accuracy S4, design §3.b): a virtual page may host its
// content at 2^level x the base texel density over the SAME angular rect, so a brush whose
// footprint is sub-representable at the base resolution still commits its shape (the S3
// release-pop cure). The page-table entry packs the level into its top byte (pool ids stay
// below 2^24 — ResolveSculptPagePoolCount caps the pool): id = entry & kSculptPageIdMask,
// level = entry >> kSculptPageLevelShift; kSculptNoPage is tested for FIRST, so its 0xFF top
// byte never decodes as a level. A level-L page owns 4^L CONTIGUOUS pool slots (base id +
// followers) holding a (127*2^L + 1)^2 endpoint-aligned fine grid — 255^2 <= 4 slots and
// 509^2 <= 16 slots, so the block never outgrows its slots. Max level 2 (4x density) is the
// honest VRAM ceiling: one level-2 page costs 16 of the pool's slots (1 MiB of the default
// 256-slot budget per page). Mirrored in cbt_sculpt.glsl (CBT_SCULPT_PAGE_ID_MASK et al) and
// locked by CBTLayoutTests.GlslSculptPageConstantsMatchCpp.
inline constexpr uint32_t kSculptPageIdMask = 0x00FFFFFFu;
inline constexpr uint32_t kSculptPageLevelShift = 24u;
inline constexpr uint32_t kSculptMaxPageLevel = 2u;
inline constexpr uint32_t kCBTSphereSculptBinding = 16;          // StorageBuffer: float[ring][poolPages][pageTexels]
inline constexpr uint32_t kCBTSphereSculptPageTableBinding = 20; // StorageBuffer: uint[ring][6*cap^2] page table

// ---------------------------------------------------------------------------
// Phase E resident-window atlas bindings. Kept OUT of CBTBinding for the
// same reason as 14/15/16 (no zero-fill / UAV barrier). Both are dark-shipped: a
// planar terrain with atlas disabled (AtlasParams1.z == 0) never touches them, and
// the driver DCEs the resolve out of every kernel but VertexEval.
//
// Binding 17: the per-tile indirection table (TileAtlasSlot, 16 B std430 — mirror
// TerrainECS::TileAtlasSlot / cbt_atlas.glsl CBTTileAtlasSlot). Host-visible ring:
// the CPU writes ring slot (frameIndex % ring) each frame from the residency
// controller's table, so the slot the shader reads this frame was written before
// this frame's GPU work — the SAME write-once-per-frame / ring discipline as the
// FrameParams UBO, and the reason the shader-side generation check is redundant
// (each frame reads its own ring slot; an evicted texture slot is quarantined for
// >= framesInFlight frames — see AtlasSlotPool). One row per tile; sized to a fixed
// tile-capacity window per ring slot so a huge terrain's small table fits trivially.
inline constexpr uint32_t kAtlasMaxTiles = 16384u; // 128x128 tile cap (128 km at 1 km tiles); 256 KiB/slot
inline constexpr uint32_t kAtlasRowBytes = 16u;    // sizeof(TileAtlasSlot) — asserted at the writer
inline constexpr uint32_t kCBTAtlasRowsBinding = 17;    // StorageBuffer: TileAtlasSlot[ring * kAtlasMaxTiles]
// Binding 18: the atlas height texture ring (R32_FLOAT, AtlasDim^2). Rebound per
// frame slot exactly like binding 15, so a mid-session atlas resize / rebind never
// touches an in-flight ring element. VertexEval samples it with a linear-clamp
// sampler; the 1-texel apron absorbs the bilinear straddle at a slot's edge.
inline constexpr uint32_t kCBTAtlasHeightBinding = 18;  // CombinedImageSampler ring: atlas height texture
// Binding 19: the per-terrain coarse height field (R32_FLOAT, coarseDim^2) an out-of-window
// tile resolves to — height-continuous with the resident relief at the window edge (Risk 3).
// Always resident, a few KiB; rebound per frame slot like binding 18.
inline constexpr uint32_t kCBTAtlasCoarseBinding = 19; // CombinedImageSampler ring: coarse height field
// Binding 20: the sphere sculpt PAGE TABLE SSBO (planet editing v2). Host-visible ring, same
// discipline as binding 16; only VertexEval reads it (gated on SphereSculptEnabled). Declared
// after the atlas bindings so the SoA storage-buffer loops (0..13) never touch it.
// Binding 21: the height-range pyramid the content-aware split reads (wide arm only; the
// narrow-heap kernels do not declare it). Device-local; Kernel_HeightRangeBuild writes it from
// the bound height texture whenever the heights may have changed, Classify reads it.
inline constexpr uint32_t kCBTHeightRangeBinding = 21; // StorageBuffer: uint[kCBTHeightRangeEntries]
// Binding 22: the paged height resolve's page table (cbt_page.glsl), a host-visible ring of equal
// slots, one per frame slot, written whole from the terrain's table when its version changes (the
// binding-17 ring discipline). A slot holds at least kCBTPageTableMinRingWords words and grows to
// the bound terrain's table (CBTResources::ProvisionPageTableWords). Words, mirror cbt_layout.glsl
// CBT_PAGE_*:
//   [0, 4)   CBTPageField (level-0 samples X, Z, level count, cache slots per row)
//   [4]      the cache's slot count; [5, 8) zero
//   [8, 8 + 4 * kCBTPageMaxLevels) CBTPageLevel per level (first entry, pages X, Z, 0)
//   then one arrival fade per cache slot (float bits), then the entries (level-major, row-major).
// The ring's words per slot are not in the table: they ride the frame's own params
// (CBTFrameParams::AnalyticParams[kCBTPageRingWordsLane], written by UploadFrameParams), so a frame
// finds its slot without reading a slot another frame's upload may be rewriting.
// Binding 23: the page cache texture ring (R32_FLOAT, one per field shared by every terrain),
// rebound per frame slot like binding 18. Both are wide-arm only: the narrow arm's VertexEval
// already binds the WebGPU per-stage storage-buffer limit, so it keeps the unified and atlas
// heights (the narrow-heap kernels do not declare them).
inline constexpr uint32_t kCBTPageTableBinding = 22; // StorageBuffer: uint[ring * slot words]
inline constexpr uint32_t kCBTPageCacheBinding = 23; // CombinedImageSampler ring: the height page cache
inline constexpr uint32_t kCBTPageMaxLevels = 32u;   // mirror PageStreaming::kPageStoreMaxLevels
inline constexpr uint32_t kCBTPageHeaderWords = 8u;
inline constexpr uint32_t kCBTPageFadeOffsetWords = kCBTPageHeaderWords + 4u * kCBTPageMaxLevels;
inline constexpr uint32_t kCBTPageRingWordsLane = 2u; // AnalyticParams lane carrying the ring's words per slot
inline constexpr uint32_t kCBTPageTableMinRingWords = 1u << 18; // 1 MiB per frame slot; slots grow in these steps
inline constexpr uint32_t kCBTDescriptorBindingCount = 24;

// The height-range pyramid (content-aware split). One entry per square block of height-lattice
// cells at each stored level: the lowest and highest normalized height over the block, as two raw
// floats (the heights a terrain holds are not bounded to [0, 1], so nothing is quantized or
// clamped). Level k blocks are 2^k cells wide; the first stored level is the finest of 1..4 whose
// levels up to 1 x 1 fit kCBTHeightRangeEntries (finer queries read the height texture directly).
// 2^20 entries (8 MiB) store from level 1 up to a 1024 x 1024-cell lattice, from level 2 at 2048
// and from level 3 at 4096.
inline constexpr uint32_t kCBTHeightRangeEntries = 1u << 20;
inline constexpr uint32_t kCBTHeightRangeMaxFirstLevel = 4u;
// Words ahead of the entries: [0] the lattice cells (x | y << 16) the pyramid was built from, [1] the
// first and top stored levels (first | top << 8), [2 + k] the first entry of level k. Classify reads
// the level table here instead of recomputing it for every facet. Entry e sits at words
// kCBTHeightRangeHeaderWords + 2e (low) and + 2e + 1 (high).
inline constexpr uint32_t kCBTHeightRangeHeaderWords = 32u;
inline constexpr uint32_t kCBTHeightRangeBufferWords = kCBTHeightRangeHeaderWords + 2u * kCBTHeightRangeEntries;

// Blocks per axis at pyramid level `level` over an axis of `cells` lattice cells. Mirror
// cbt_kernels.comp CBT_HeightRangeDim (and the three below): the CPU sizes the build dispatches
// with this arithmetic, the kernels address the buffer with theirs.
constexpr uint32_t CBTHeightRangeDim(uint32_t cells, uint32_t level)
{
    return (cells + (1u << level) - 1u) >> level;
}

// The level at which both axes are one block.
constexpr uint32_t CBTHeightRangeTopLevel(uint32_t cellsX, uint32_t cellsY)
{
    uint32_t level = 0;
    while (CBTHeightRangeDim(cellsX, level) > 1u || CBTHeightRangeDim(cellsY, level) > 1u)
        ++level;
    return level;
}

// The first entry of level `level` when the stored levels begin at `first`.
constexpr uint32_t CBTHeightRangeOffset(uint32_t cellsX, uint32_t cellsY, uint32_t first,
                                        uint32_t level)
{
    uint32_t offset = 0;
    for (uint32_t k = first; k < level; ++k)
        offset += CBTHeightRangeDim(cellsX, k) * CBTHeightRangeDim(cellsY, k);
    return offset;
}

// The first stored level: the finest of 1..kCBTHeightRangeMaxFirstLevel whose levels up to the
// top fit the buffer; 0 when none does (the content-aware split is off for that lattice).
constexpr uint32_t CBTHeightRangeFirstLevel(uint32_t cellsX, uint32_t cellsY)
{
    const uint32_t top = CBTHeightRangeTopLevel(cellsX, cellsY);
    for (uint32_t first = 1; first <= kCBTHeightRangeMaxFirstLevel; ++first)
    {
        const uint32_t last = (top > first ? top : first) + 1u;
        if (CBTHeightRangeOffset(cellsX, cellsY, first, last) <= kCBTHeightRangeEntries)
            return first;
    }
    return 0;
}
static_assert(CBTHeightRangeFirstLevel(1024, 1024) == 1, "a 1025^2 heightfield stores from level 1");
static_assert(CBTHeightRangeFirstLevel(2048, 2048) == 2, "a 2049^2 unified set stores from level 2");
static_assert(CBTHeightRangeFirstLevel(4096, 4096) == 3, "a 4097^2 unified set stores from level 3");

// ---------------------------------------------------------------------------
// Planet-shading surface params (plan §planet-shading)
// ---------------------------------------------------------------------------
// A small GRAPHICS-only buffer the fragment reads to shade the sphere with per-pixel
// ANALYTIC normals (the multi-octave relief gradient + a direction-sampled sculpt gradient)
// and a radial slope+altitude material blend. It rides set 2 via DrawBindings (resolved by
// the reflected instance name gCbtSurf), NOT the compute descriptor set, so the kernel
// layout is untouched. Host-visible ring: the render feature writes slot (frameIndex % ring)
// each frame, and the fragment offset-binds that one slot (kCBTSurfaceParamsSlotStride) so it
// always reads element [0]. Mirror of CBTSurfaceParamsData in cbt_surface.glsl.
struct CBTSurfaceParams
{
    float Radius = 0.0f;
    float ReliefAmplitude = 0.0f;
    float ReliefFrequency = 0.0f;
    uint32_t ReliefOctaves = 0u;
    uint32_t SphereSculptEnabled = 0u; // 1 -> the fragment adds the sculpt gradient to the normal
    // Paged sculpt geometry (mirror SphereSculptGeometry) — the fragment's page addressing (no
    // hardcode). VirtualDim is the radius-scaled per-face dim; Cap the fixed page-table row stride;
    // PoolPageCount the physical pool size (0 -> unconfigured, sample 0).
    uint32_t SphereSculptVirtualDim = 0u;
    uint32_t SphereSculptCap = 0u;
    uint32_t SphereSculptPagesPerAxis = 0u;
    uint32_t SphereSculptPoolPageCount = 0u;
    uint32_t DebugMode = 0u; // TerrainDebugView: 0 = off (normal shading), 1 = facet tint (CBT triangulation)
    // Phase E: 1 when the planar terrain is resident-window-atlas-backed. Such a terrain has no
    // unified normalmap; the surface resolves splat + normal per-pixel through the atlas below.
    uint32_t AtlasBacked = 0u;
    // Quality-sweep slice 1: the atlas geometry (mirror AtlasGeometry) + the four bindless texture
    // indices the surface taps. The fragment resolves terrain UV -> tile -> slot UV via cbt_atlas.glsl
    // (the SAME resolve VertexEval runs for height) using the per-tile indirection rows bound on set 2
    // (gAtlasRows), then samples AtlasSplat/AtlasNormal at that slot UV for a resident tile, or
    // AtlasSplatCoarse/AtlasNormalCoarse at terrain UV out of the resident window. All integer geometry
    // rides as u32 (< 2^23 -> exact). Zero-defaulted so a non-atlas terrain leaves the path untouched.
    uint32_t AtlasDim = 0u;
    uint32_t AtlasSlotStride = 0u;
    uint32_t AtlasSlotsPerRow = 0u;
    uint32_t AtlasTileRes = 0u;
    uint32_t AtlasTilesPerAxisX = 0u;
    uint32_t AtlasTilesPerAxisZ = 0u;
    uint32_t AtlasCoarseDim = 0u;
    uint32_t AtlasSplatBindless = 0u;
    uint32_t AtlasNormalBindless = 0u;
    uint32_t AtlasSplatCoarseBindless = 0u;
    uint32_t AtlasNormalCoarseBindless = 0u;
    // Analytic sphere modifiers (sculpt shape-accuracy S2 — mirror the tail of CBTSurfaceParamsData
    // in cbt_surface.glsl). The same placement set CBTFrameParams carries, here for the fragment's
    // per-pixel composed sculpt (centre height + gradient taps + tap gate). DabCount (S3, the old
    // pad slot) keeps the vec4-struct array 16-aligned in std430 (88 + 4 + 4 = 96); transient
    // dabs occupy slots [Count, Count+DabCount). Both 0 = every analytic branch skipped.
    uint32_t SphereAnalyticCount = 0u;
    uint32_t SphereAnalyticDabCount = 0u;
    float SphereAnalytic[kMaxSphereAnalyticModifiers * kSphereAnalyticFloatsPerModifier] = {};
};
static_assert(sizeof(CBTSurfaceParams) == 864,
              "CBTSurfaceParams must be 864B (22 scalars + count/pad + 16 x 3 vec4 analytic set, "
              "std430 mirror)");
static_assert(offsetof(CBTSurfaceParams, SphereAnalyticCount) == 88 &&
                  offsetof(CBTSurfaceParams, SphereAnalytic) == 96,
              "analytic tail is appended after the leading 88B block, array 16-aligned (std430)");

// Per-slot stride, padded well past sizeof so slot * stride satisfies every device's
// minStorageBufferOffsetAlignment when the fragment offset-binds a single ring slot.
inline constexpr uint32_t kCBTSurfaceParamsSlotStride = 1280u;
static_assert(kCBTSurfaceParamsSlotStride % 256u == 0u && kCBTSurfaceParamsSlotStride >=
                  sizeof(CBTSurfaceParams),
              "surface-params slot stride must be 256-aligned and hold one struct");

// ---------------------------------------------------------------------------
// Push constants (<= 128 bytes, single shared block across all kernels)
// ---------------------------------------------------------------------------
// Small per-dispatch state shared by every kernel. Camera + terrain params live in
// the CBTFrameParams UBO instead (a mat4 alone is 64 B and would blow the 128-byte
// push-constant budget); the push block only carries per-dispatch selectors.
// ClassifyMode picks the metric: kClassifyScreenSpace (C4 production — projected
// edge length vs a target pixel error, camera from the UBO) or kClassifyDepthTarget
// (the deterministic depth-band metric used by the unit tests / bring-up, where
// FocusRoot's subtree is driven to TargetDepth and the rest toward BaseDepth). Under
// screen-space, TargetDepth is reused as the max-subdivision cap. 56 B, under 128.
struct CBTPushConstants
{
    uint32_t PassIndex = 0u;         // selects which counter PrepareIndirect promotes
    uint32_t BaseDepth = kDefaultBaseDepth;
    uint32_t PoolSize = kDefaultBisectorPoolSize;
    uint32_t TotalElements = 0u;     // whole-pool bound for the per-slot kernels
    uint32_t NeighborsReadIsA = 1u;  // ping-pong parity: 1 = current is A/next is B, 0 = swapped
    uint32_t FocusRoot = kFocusRootAll; // Classify: which root subtree to drive to TargetDepth
    uint32_t TargetDepth = kDefaultBaseDepth; // Classify: desired depth inside the focus subtree
    uint32_t FrameIndex = 0u;
    uint32_t ClassifyMode = kClassifyDepthTarget; // kClassify* — depth-target (tests) or screen-space (C4)
    uint32_t DomainMode = kDomainPlanar; // kDomain* — planar (default) or spherical cube-sphere (C7)
    // C5 region-dirty reclassification (plan §8 C5): this frame's edited terrain
    // footprint in UV [0,1] (the region-dirty log, mapped from texels). Classify
    // flags any bisector whose UV AABB overlaps it MODIFIED and re-evaluates it
    // without the hysteresis dead-band, so an edit reaches the right density the
    // update it lands instead of waiting for camera motion. Empty (DirtyMaxU <=
    // DirtyMinU) => no edit this frame => Classify's dirty branch is skipped and
    // work quiescence holds. Only Classify reads these; the other kernels ignore
    // them (one shared push-constant block).
    float DirtyMinU = 0.0f;
    float DirtyMinV = 0.0f;
    float DirtyMaxU = 0.0f;
    float DirtyMaxV = 0.0f;
    // Planet-editing v1 (plan §planet-editing): the (face,rect) region identity. On the
    // SPHERE, the dirty rect above is a FACE-LOCAL UV rect and DirtyFace selects which
    // cube face it lies on — Classify flags MODIFIED only for bisectors on that face whose
    // face-local UV AABB overlaps it (region-proportional per face; a cross-face edit is
    // handed one face per update, the others re-refine via the normal metric). Ignored in
    // planar mode (DirtyFace unused; the rect is a whole-terrain UV rect). SphereSculptEnabled
    // gates the VertexEval additive sculpt sample (0 = pure procedural relief, the C7 default
    // — zero overhead and byte-identical to pre-edit).
    uint32_t DirtyFace = 0u;
    uint32_t SphereSculptEnabled = 0u;
    // Quiescence gate for VertexEval (plan §planet-shading perf). 0 = evaluate the whole live
    // pool (pre-slice behavior; the force pass after init / a param change). 1 = evaluate only
    // bisectors flagged CBT_FLAG_MODIFIED this frame, so a quiescent frame re-evaluates ~0.
    uint32_t GateVertexEval = 0u;
    // Edit-driven retessellation A/B (round-8b). 1 = Classify carries the geometric-error crease
    // term (refines a facet straddling a live sculpt cliff the area metric cannot see); 0 = the
    // pre-slice area-only metric. Default on; GE_CBT_EDIT_RETESS=0 forces it off for an A/B on the
    // same build. Only Classify reads it, only on the sphere with SphereSculptEnabled.
    uint32_t EditRetessEnabled = 1u;
    // Near-field force-split occupancy gate (round-8e saturation-deadlock fix). 1 = under pool
    // pressure, stop force-splitting invisible near-plane/behind-eye grazing geometry (the treadmill
    // that pins the pool at 100% and freezes the topology, so a static-camera edit / TargetPixelError
    // change can never re-tessellate); 0 = the pre-slice unconditional force-split (the fails-before
    // behavior). Default on; GE_CBT_NEARFIELD_GATE=0 forces it off for an A/B on the same build. Only
    // Classify reads it, only on the sphere with the screen-space metric.
    uint32_t NearFieldGate = 1u;
    // Far-field region reseed mask (round-9 far-field-drain). A bitmask of ROOT indices; bit r set =
    // free root r's subtree back to base this dispatch. Read ONLY by the K_RegionFree* kernels (which
    // never run in the per-frame update sequence), so every other kernel ignores it. Chosen CPU-side
    // as the interior of a wholly-horizon-invisible root region (CBTInstance::RegionFreeToBase /
    // FarFieldReseedDetector). 0 in the normal update.
    uint32_t RegionFreeMask = 0u;
    // Earth-scale (sector, local) storage A/B (decode-precision arc S2a). 1 = spherical VertexEval
    // decodes each corner via the df64 path (cbt_deep_decode.glsl, the GLSL twin of CBTDeepDecode.h)
    // and stores (sector anchor, fp32 local offset) with DeepTag = 1; 0 (default) = the fp32 world
    // store, byte-identical to the pre-S2a pipeline. Only VertexEval reads it — every CONSUMER
    // branches on the per-slot DeepTag instead, so mixed-mode pools degrade gracefully. Driven from
    // GE_CBT_DEEP_DECODE by CBTUpdateSystem (default OFF — the dark-ship).
    uint32_t DeepDecode = 0u;
    // Planar pool-pressure scale (Kernel_Reset / Kernel_Classify). 1 = on; 0 holds the scale at 1x —
    // the A/B arm the pinned-regime oracles use. Mirrors CBTClassifyDesc::PoolPressure.
    uint32_t PoolPressure = 1u;
};

// ---------------------------------------------------------------------------
// Per-slice pool geometry, resolved once for the compile-time default pool.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kDefaultBitfieldWords = CBTBitfieldWords(kDefaultBisectorPoolSize);
inline constexpr uint32_t kDefaultLeafNodeCount = CBTLeafNodeCount(kDefaultBisectorPoolSize);
inline constexpr uint32_t kDefaultLeafPackedWords = CBTLeafPackedWords(kDefaultBisectorPoolSize);
inline constexpr uint32_t kDefaultSumTreeNodeCount = CBTSumTreeNodeCount(kDefaultBisectorPoolSize);
inline constexpr uint32_t kDefaultSumTreeDepth = CBTSumTreeDepth(kDefaultBisectorPoolSize);
inline constexpr uint32_t kDefaultSumTreeWords = CBTSumTreeWords(kDefaultBisectorPoolSize);
inline constexpr uint32_t kDefaultWorkQueueWords = WQElementCount(kDefaultBisectorPoolSize);

// ---------------------------------------------------------------------------
// static_asserts — the C++/GLSL/std430 contract
// ---------------------------------------------------------------------------
static_assert(sizeof(CBTNeighbors) == 16, "CBTNeighbors must be uvec4 (16B std430)");
static_assert(sizeof(CBTBisectorData) == 32, "CBTBisectorData must be 8 x u32 (32B std430)");
static_assert(offsetof(CBTBisectorData, SubdivisionPattern) == 0, "hot-field-front layout");
static_assert(offsetof(CBTBisectorData, BisectorState) == 4, "hot-field-front layout");
static_assert(offsetof(CBTBisectorData, Flags) == 8, "hot-field-front layout");
static_assert(offsetof(CBTBisectorData, ProblematicNeighbor) == 12, "hot-field-front layout");
static_assert(offsetof(CBTBisectorData, PropagationID) == 16, "hot-field-front layout");
static_assert(offsetof(CBTBisectorData, Indices) == 20, "indices[3] tail");
static_assert(sizeof(CBTVertexData) == 96,
              "CBTVertexData must be 4 x vec4 + 4 x uvec2 (96B std430 — S2a (sector, local) tail)");
static_assert(offsetof(CBTVertexData, Meta) == 48, "legacy corner/meta offsets must not move (dark-ship)");
static_assert(offsetof(CBTVertexData, Sector0) == 64 && offsetof(CBTVertexData, DeepTag) == 88,
              "sector tail is appended after the legacy 64B block (uvec2-aligned)");
static_assert(sizeof(CBTFrameParams) == 1808,
              "CBTFrameParams must be mat4 + 13 vec4 + 16 x 3 vec4 analytic set + 48 uvec4 cell "
              "masks (1808B std140 — S3 appends the mask tail)");
static_assert(offsetof(CBTFrameParams, WaterPlane) == 240 &&
                  offsetof(CBTFrameParams, AnalyticParams) == 256 &&
                  offsetof(CBTFrameParams, SphereAnalytic) == 272,
              "analytic tail is appended after the 256B scalar block (vec4-aligned, std140)");
static_assert(offsetof(CBTFrameParams, AnalyticCellMask) == 1040,
              "the S3 cell-mask tail is appended after the 1040B frame block (uvec4-aligned, std140)");

static_assert((kDefaultBisectorPoolSize & (kDefaultBisectorPoolSize - 1u)) == 0u,
              "pool size must be a power of two");
static_assert(kDefaultBisectorPoolSize % 32u == 0u, "pool size must be a multiple of 32 (u32 bitfield word)");
static_assert(kDefaultBaseDepth == CBTLog2(kRootHalfedgeCount), "baseDepth = log2(halfedges)");
static_assert(kRootHalfedgeCount == 2u, "C2 conforming base = 2 twin triangles");
static_assert(kDefaultBaseDepth == 1u, "log2(2) = 1");
static_assert(sizeof(CBTPushConstants) == 88,
              "push constants must stay 88B (10 u32 + 4 f32 dirty-rect + 2 u32 sphere face/enable + "
              "1 u32 vertex-eval gate + 1 u32 edit-retess enable + 1 u32 near-field gate + 1 u32 "
              "region-free mask + 1 u32 deep-decode (S2a) + 1 u32 pool-pressure; GLSL mirror; "
              "<= 128B policy)");

// u32 sum-tree derivation sanity for the default 2^20 pool (1M pool bump).
static_assert(kDefaultBitfieldWords == 32768u, "1M / 32 = 32768 u32 bitfield words");
static_assert(kDefaultLeafNodeCount == 32768u, "one popcount leaf per 32-bit word");
static_assert(kDefaultLeafPackedWords == 8192u, "4 packed byte-counts per u32");
static_assert(kDefaultSumTreeNodeCount == 65535u, "2 * 32768 - 1 full binary heap");
static_assert(CBTLeafLevelOffset(kDefaultBisectorPoolSize) == 32767u, "leaf level offset = L - 1");
static_assert(kDefaultSumTreeDepth == 15u, "sum-tree depth = log2(L)");

// The reduce's split level (CBT_SUMTREE_SHARED_DEPTH in cbt_kernels.comp): ReduceSecondPass
// reduces levels 0..kSumTreeSharedDepth in one workgroup's memory, and each ReduceFirstPass
// workgroup sums its kComputeWorkgroupSize leaves up to level kSumTreeSharedDepth + 1. That
// block holds whole subtrees of those levels only while the levels below the split fit in it.
inline constexpr uint32_t kSumTreeSharedDepth = 10u;
static_assert(kDefaultSumTreeDepth - kSumTreeSharedDepth - 1u <= CBTLog2(kComputeWorkgroupSize),
              "a ReduceFirstPass workgroup no longer covers a whole subtree below the split: raise kSumTreeSharedDepth "
              "and CBT_SUMTREE_SHARED_DEPTH together (the shared array grows with it)");

} // namespace GameEngine::CBTTerrain
