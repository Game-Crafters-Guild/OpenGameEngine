// Headless oracle for the round-8 planet flatten-move crack (a persistent sliver-crack + debris
// trail left behind a modifier dragged across a spherical CBT terrain). The crack is a STALE
// VERTEX: a bisector keeps the cached corner height it had while an edit sat on it, even after the
// edit's footprint is restored, because the per-frame region-scoped VertexEval only re-evaluates
// the bisectors this frame's dirty rect covers — and that rect trails the moving modifier, so a
// bisector the modifier already passed is never flagged MODIFIED again.
//
// This drives the REAL GPU update kernels (RecordUpdate) over the sculpt store and reads back the
// CurrentVertex buffer, so it measures actual per-corner heights (byte/ULP), not a CPU model. It
// establishes both halves of the fix contract:
//   * the region-scoped GATED re-eval, handed a dirty rect that misses a just-restored footprint,
//     STRANDS that footprint's bisectors at their stale height (the crack mechanism), and
//   * a full-pool re-eval against the current sculpt heals every stranded bisector so no live
//     bisector differs from a fresh evaluation.
// The shipped fix forces the full-pool re-eval for the frames a planet's sculpt is changing
// (CBTUpdateSystem), which is exactly the second half here — so a regression that lets the stale
// trail survive (the first half without the second) is caught.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"
#include "CBTTestHarness.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24 — S2a 96B layout (sector tail appended; corner/meta word offsets unchanged)

// Sphere frame params carrying the paged-sculpt geometry the way BuildFrameParams feeds it
// (AtlasParams0 = VirtualDim / Cap / PagesPerAxis / PoolPageCount) so VertexEval's CBT_SampleSphereSculpt
// addresses the pool exactly as it does in the editor.
CBTFrameParams SphereParamsWithSculpt(float radius, float reliefAmp, float reliefFreq, uint32_t octaves,
                                      const SphereSculptGeometry& geom)
{
    CBTFrameParams p{};
    p.PlanetParams[0] = radius;
    p.PlanetParams[1] = reliefAmp;
    p.PlanetParams[2] = reliefFreq;
    p.PlanetParams[3] = static_cast<float>(octaves);
    p.AtlasParams0[0] = static_cast<float>(geom.VirtualDim);
    p.AtlasParams0[1] = static_cast<float>(geom.Cap);
    p.AtlasParams0[2] = static_cast<float>(geom.PagesPerAxis);
    p.AtlasParams0[3] = static_cast<float>(geom.PoolPageCount);
    return p;
}
} // namespace

class CBTSphereSculptReEval : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    // Upload the current sculpt store into THIS frame's ring slot (when editing) and record the
    // update — the same per-frame discipline BuildFrameParams + CBTUpdateNode run.
    void RunFrame(CBTInstance& inst, const SphereSculptLayer& layer, bool uploadSculpt,
                  const CBTClassifyDesc& desc, const CBTFrameParams& params, uint32_t frame)
    {
        if (uploadSculpt)
        {
            inst.UploadSphereSculptPool(frame, layer.Pool().data(),
                                        static_cast<uint32_t>(layer.Pool().size()));
            inst.UploadSphereSculptPageTable(frame, layer.PageTable().data(),
                                             static_cast<uint32_t>(layer.PageTable().size()));
        }
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        inst.RecordUpdate(*cl, desc, params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::vector<float> ReadVerts(CBTInstance& inst)
    {
        const auto words =
            inst.DebugReadWords(CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        std::vector<float> f(words.size());
        std::memcpy(f.data(), words.data(), words.size() * sizeof(uint32_t));
        return f;
    }

    std::vector<uint64_t> ReadHeap(CBTInstance& inst)
    {
        const auto w = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        std::vector<uint64_t> h(kDefaultBisectorPoolSize);
        for (uint32_t i = 0; i < kDefaultBisectorPoolSize; ++i)
            h[i] = static_cast<uint64_t>(w[i * 2u]) | (static_cast<uint64_t>(w[i * 2u + 1u]) << 32);
        return h;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// Step a flatten modifier A -> B across a planet and assert the region-scoped gated re-eval strands
// the vacated footprint's bisectors while a full-pool re-eval leaves none stale. Reproduces the
// mechanism and locks the fix at the vertex level (byte compare of the readback CurrentVertex).
TEST_F(CBTSphereSculptReEval, FlattenMoveStrandsVerticesUntilFullReEval)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));

    const float radius = 20000.0f; // planet radius in meters
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    SphereSculptLayer layer;
    layer.Configure(geom);
    const CBTFrameParams p = SphereParamsWithSculpt(radius, 125.0f, 6.0f, 4u, geom);

    // Uniform depth-target refinement (no camera) so bisectors exist at the edited directions and
    // the tessellation is churn-free — isolating the STALE-VERTEX bug from split/merge motion. The
    // sculpt is disabled (no edits yet), so these frames need no pool upload.
    const uint32_t targetDepth = inst.GetBaseDepth() + 8u;
    CBTClassifyDesc c{};
    c.Mode = kClassifyDepthTarget;
    c.FocusRoot = kFocusRootAll;
    c.TargetDepth = targetDepth;
    c.SphereSculptEnabled = 0u;
    for (uint32_t f = 0; f <= 24u; ++f)
    {
        c.GateVertexEval = (f == 0u) ? 0u : 1u;
        RunFrame(inst, layer, /*uploadSculpt=*/false, c, p, f);
    }

    // Two well-separated footprints on the same cube face near the +Z pole. A is the modifier's
    // "old" position, B the "new" one; the angular radius keeps their caps disjoint so B's dirty
    // rect can never cover A.
    const std::array<float, 3> dirA = {0.15f, 0.0f, 1.0f};
    const std::array<float, 3> dirB = {-0.15f, 0.0f, 1.0f};
    const float ang = 0.06f;      // angular cap radius (rad) ~ 3.4 deg
    const float sink = -300.0f;   // flatten offset (m): a large, unambiguous height change

    // A unit-direction cap membership test used by the move bake (sink only inside B's cap).
    auto normalize3 = [](const std::array<float, 3>& d) {
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        return std::array<float, 3>{d[0] / l, d[1] / l, d[2] / l};
    };
    const std::array<float, 3> nB = normalize3(dirB);
    const float capCos = std::cos(ang);

    // ---- Edit lands at A: SET the modifier layer to `sink` inside A's cap, full re-eval so every
    // bisector under A drops. This is the modifier resting at its old position. ----
    c.SphereSculptEnabled = 1u;
    layer.BakeModifierLayer(ClassifySphereCapEdit(dirA[0], dirA[1], dirA[2], ang),
                            [sink](float, float, float) { return sink; });
    c.GateVertexEval = 0u;
    c.DirtyMinU = c.DirtyMaxU = 0.0f; // no rect: a full-pool eval ignores it
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 25u);

    // ---- Modifier MOVES A -> B: re-bake the modifier layer over BOTH footprints (old union new,
    // the sphere analogue of the planar move bake) — restoring A to 0 and sinking B. The published
    // pool now reads A restored. ----
    SphereEditRegions moveRegions = ClassifySphereCapEdit(dirA[0], dirA[1], dirA[2], ang);
    {
        const SphereEditRegions rb = ClassifySphereCapEdit(dirB[0], dirB[1], dirB[2], ang);
        for (uint32_t i = 0; i < rb.Count && moveRegions.Count < moveRegions.Rects.size(); ++i)
            moveRegions.Rects[moveRegions.Count++] = rb.Rects[i];
    }
    layer.BakeModifierLayer(moveRegions, [sink, nB, capCos](float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        if (l <= 0.0f)
            return 0.0f;
        const float d = (x * nB[0] + y * nB[1] + z * nB[2]) / l;
        return d >= capCos ? sink : 0.0f; // sink inside B only; A restored to 0
    });

    // ---- GATED update, dirty rect = B's face rect ONLY (the trailing rect that misses A). A's
    // bisectors are not flagged MODIFIED, so VertexEval leaves them at their sunk height — the
    // stale trail the modifier drags behind it. ----
    const SphereEditRegions rbRect = ClassifySphereCapEdit(dirB[0], dirB[1], dirB[2], ang);
    ASSERT_GT(rbRect.Count, 0u);
    c.GateVertexEval = 1u;
    c.DirtyFace = rbRect.Rects[0].Face;
    c.DirtyMinU = rbRect.Rects[0].MinU;
    c.DirtyMinV = rbRect.Rects[0].MinV;
    c.DirtyMaxU = rbRect.Rects[0].MaxU;
    c.DirtyMaxV = rbRect.Rects[0].MaxV;
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 26u);
    const std::vector<float> gated = ReadVerts(inst);

    // ---- The fix: a full-pool re-eval against the SAME (A-restored) pool. This is the reference
    // "fresh" state — every live bisector re-evaluated against the current sculpt. ----
    c.GateVertexEval = 0u;
    c.DirtyMinU = c.DirtyMaxU = 0.0f;
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 27u);
    const std::vector<float> fresh = ReadVerts(inst);

    const std::vector<uint64_t> heap = ReadHeap(inst);
    // A live bisector is STALE when its cached corner differs from the full re-eval by more than a
    // sub-millimetre tolerance (the sink is 300 m, so a stranded corner is off by ~300 m).
    auto countStale = [&](const std::vector<float>& v) {
        uint32_t n = 0;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            if (heap[s] == 0u)
                continue; // free slot
            for (uint32_t k = 0; k < kVertexWordsPerSlot; ++k)
            {
                if (std::fabs(v[s * kVertexWordsPerSlot + k] - fresh[s * kVertexWordsPerSlot + k]) > 0.001f)
                {
                    ++n;
                    break;
                }
            }
        }
        return n;
    };

    const uint32_t staleGated = countStale(gated);
    std::printf("[sculpt-reeval] staleGated=%u (region-scoped gated re-eval strands the vacated footprint)\n",
                staleGated);
    EXPECT_GT(staleGated, 0u)
        << "a gated re-eval whose dirty rect misses the just-restored footprint must strand its "
           "bisectors — if this is 0 the scenario failed to reproduce the crack mechanism";

    // After the full re-eval, a further quiescent GATED frame (empty rect) must keep every corner
    // matching the fresh eval: no stale trail survives — the shipped fix's guarantee.
    c.GateVertexEval = 1u;
    c.DirtyMinU = c.DirtyMaxU = 0.0f;
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 28u);
    const uint32_t staleAfter = countStale(ReadVerts(inst));
    std::printf("[sculpt-reeval] staleAfter=%u (full re-eval healed every stranded vertex)\n", staleAfter);
    EXPECT_EQ(staleAfter, 0u)
        << "after a full-pool re-eval no live bisector may differ from a fresh evaluation";

    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the update sequence must keep the tree conforming";
}

// PLANET-RESIZE REMAP ORACLE (sculpt-content remap slice), on the REAL update kernels: sculpt a
// mound on a 2 km planet, resize to 6 km (the CPU store remaps; radius + target depth + sculpt
// geometry params all change in one step, exactly the live-edit shape), and keep driving updates.
// Gates: (a) the tree stays conforming through the resize (0 validation errors — the resize never
// touches the bisector pool, so the classifier must simply converge on the rescaled geometry);
// (b) the remapped mound displaces the surface AT ITS DIRECTION at its METRE amplitude on the
// resized planet (VertexEval reads the remapped pool through the new AtlasParams0 geometry);
// (c) the far side stays at the base radius (the remap smeared nothing).
TEST_F(CBTSphereSculptReEval, ResizeRemapKeepsConformityAndAnchorsContent)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));

    const float r1 = 2000.0f;
    const float r2 = 6000.0f;
    const auto geom1 = MakeSphereSculptGeometry(r1, ResolveSculptPagePoolCount());
    SphereSculptLayer layer;
    layer.Configure(geom1);
    // Relief amplitude 0: the surface is radius + sculpt only, so |vertex| - radius probes the
    // sculpt amplitude directly.
    const CBTFrameParams p1 = SphereParamsWithSculpt(r1, 0.0f, 6.0f, 1u, geom1);

    const uint32_t targetDepth = inst.GetBaseDepth() + 11u;
    CBTClassifyDesc c{};
    c.Mode = kClassifyDepthTarget;
    c.FocusRoot = kFocusRootAll;
    c.TargetDepth = targetDepth;
    c.SphereSculptEnabled = 0u;
    for (uint32_t f = 0; f <= 21u; ++f)
    {
        c.GateVertexEval = (f == 0u) ? 0u : 1u;
        RunFrame(inst, layer, /*uploadSculpt=*/false, c, p1, f);
    }

    // A 150 m mound, 0.06 rad cap, centred inside the +X face.
    float dx, dy, dz;
    FaceUVToWorldDir(0u, 0.40f, 0.45f, dx, dy, dz);
    const float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float nx = dx / dlen, ny = dy / dlen, nz = dz / dlen;
    const float ang = 0.06f;
    const float amp = 150.0f;
    layer.ApplyDab(dx, dy, dz, ang, amp, false);
    c.SphereSculptEnabled = 1u;
    c.GateVertexEval = 0u;
    c.DirtyMinU = c.DirtyMaxU = 0.0f;
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p1, 22u);

    // Max |vertex| - radius over live corners inside/outside the cap around the dab direction.
    auto probe = [&](float radius, float& maxInCap, float& maxOutCap) {
        const std::vector<float> v = ReadVerts(inst);
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const float capCos = std::cos(ang);
        maxInCap = -1e9f;
        maxOutCap = -1e9f;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            if (heap[s] == 0u)
                continue;
            for (uint32_t corner = 0; corner < 3u; ++corner)
            {
                const float* p = &v[s * kVertexWordsPerSlot + corner * 4u];
                const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                if (len <= 0.0f)
                    continue;
                const float d = (p[0] * nx + p[1] * ny + p[2] * nz) / len;
                const float h = len - radius;
                if (d >= capCos)
                    maxInCap = std::max(maxInCap, h);
                else if (d < std::cos(3.0f * ang)) // clear of the cap + rim
                    maxOutCap = std::max(maxOutCap, h);
            }
        }
    };
    float inCap1 = 0.0f, outCap1 = 0.0f;
    probe(r1, inCap1, outCap1);
    ASSERT_GT(inCap1, 0.4f * amp) << "the mound must displace vertices before the resize";
    ASSERT_LT(std::fabs(outCap1), 1.0f) << "outside the cap the pre-resize surface is the base sphere";

    // ---- THE RESIZE: remap the store to the 6 km grid and switch every param in one step ----
    const SphereEditRegions remapped = layer.Configure(MakeSphereSculptGeometry(r2, ResolveSculptPagePoolCount()));
    ASSERT_GT(remapped.Count, 0u) << "the resize must remap (dim changes 2 km -> 6 km)";
    const CBTFrameParams p2 = SphereParamsWithSculpt(r2, 0.0f, 6.0f, 1u, layer.Geometry());

    // Drive updates through the change: first a full re-eval frame (the editing-frames forced
    // VertexEval the version advance triggers in production), then settle frames with the gate on.
    // Uploading every frame mirrors the per-ring-slot refresh the production upload gate performs
    // after an edit (each of the 4 slots re-uploads once before going idle).
    for (uint32_t f = 23u; f <= 35u; ++f)
    {
        c.GateVertexEval = (f == 23u) ? 0u : 1u;
        c.DirtyMinU = c.DirtyMaxU = 0.0f;
        RunFrame(inst, layer, /*uploadSculpt=*/true, c, p2, f);
    }

    float inCap2 = 0.0f, outCap2 = 0.0f;
    probe(r2, inCap2, outCap2);
    std::printf("[resize-remap] inCap pre=%.1f post=%.1f m; outCap pre=%.2f post=%.2f m; valErr=%u\n",
                inCap1, inCap2, outCap1, outCap2, inst.ReadValidationErrorCount());
    EXPECT_GT(inCap2, 0.4f * amp)
        << "the remapped mound must still displace the surface at its direction on the resized planet";
    EXPECT_LT(inCap2, 1.2f * amp) << "the remapped amplitude must stay in metres (not scale with radius)";
    EXPECT_LT(std::fabs(outCap2), 1.0f) << "the remap must not smear content outside the cap";
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u)
        << "the tree must stay conforming through a planet resize";
}
