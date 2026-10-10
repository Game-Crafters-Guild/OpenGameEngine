/**
 * @file CascadeCullingUnifiedParityTests.cpp
 * @brief Regression guard for the fused SubmitCascadeGroup cascade culling
 *        path. Cross-checks one fused N=4 dispatch against four independent
 *        N=1 SubmitView dispatches that simulate the historical per-cascade
 *        path. For the same candidate set and cascade frusta, every
 *        per-cascade visibility slice must be byte-identical between the two
 *        — the SPIR-V spec-const unrolls the cascade loop, so each cascade's
 *        test inside the fused dispatch is the same op as a free-standing
 *        N=1 dispatch.
 *
 * Production has only one cascade culling code path (SubmitCascadeGroup);
 * the legacy SubmitView-per-cascade pattern is reconstructed here purely as
 * the test baseline.
 *
 * The pipeline never reads visibility back to the CPU (it is consumed
 * GPU-side by the draw-stream scatter), so this test owns its readback: it
 * declares a copy pass from the frame's visibility buffer into a
 * test-created readback buffer and compares the RAW per-instance flags of
 * every slice across the two paths.
 *
 * This is a standalone (non-gtest) executable mirroring the GPUCullingTests
 * harness; returns 0 on success, 1 on failure, 77 if the GPU is unavailable.
 */

#include <Rendering/Common/Math.h>
#include <Rendering/Common/Utils.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Core/GPUCulling.h>
#include <Rendering/Core/GPUScene.h>
#include <Rendering/Core/RenderGraph/RGFrame.h>
#include <Mathematics/MatrixOps.h>

#include "TestUtils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GameEngine::Rendering;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{
// ctest maps this to "skipped" via SKIP_RETURN_CODE.
constexpr int kSkipExitCode = 77;

// Search a few well-known locations for the staged shaderpkg. Tests run with
// WORKING_DIRECTORY = ${CMAKE_BINARY_DIR}, so paths relative to that root are
// expected to resolve.
fs::path FindStagedShaderPkg(const char* name)
{
    const fs::path candidates[] = {
        fs::current_path() / "bin" / "Debug" / "Apps" / "Editor" / "Assets" / "Shaders" / name,
        fs::current_path() / "bin" / "DebugFast" / "Apps" / "Editor" / "Assets" / "Shaders" / name,
        fs::current_path() / "bin" / "RelWithDebInfo" / "Apps" / "Editor" / "Assets" / "Shaders" / name,
        fs::current_path() / "bin" / "Release" / "Apps" / "Editor" / "Assets" / "Shaders" / name,
        // Fallback: alongside the test executable.
        fs::current_path() / "Apps" / "Editor" / "Assets" / "Shaders" / name,
    };
    std::error_code ec;
    for (const auto& p : candidates)
    {
        if (fs::exists(p, ec))
            return p;
    }
    return {};
}

static fs::path g_ShaderPkgPath;

std::vector<uint8_t> CullingShaderPkgLoader(const char* name)
{
    using Utils::ReadFile;
    if (!name || !name[0])
        return {};
#ifdef RENDERING_SHADER_OUTPUT_DIR
    // Compiled .shaderpkg artifacts live under CMAKE_BINARY_DIR (the shader
    // output dir's parent) when built by this tree's shader targets.
    const auto built = std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / name;
    std::error_code ec;
    if (std::filesystem::exists(built, ec))
    {
        g_ShaderPkgPath = built;
        return ReadFile(built.string());
    }
#endif
    fs::path full = FindStagedShaderPkg(fs::path(name).filename().string().c_str());
    if (full.empty())
        return {};
    g_ShaderPkgPath = full;
    return ReadFile(full.string());
}

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

struct CapturedSlice
{
    ViewId viewId = 0;
    uint8_t cascadeIndex = kCullingCascadeIndexNone;
    uint32_t offset = 0;
    uint32_t count = 0;
    std::vector<uint32_t> flags; // raw per-instance visibility words
};

bool RangesEquivalent(const std::vector<CapturedSlice>& a,
                      const std::vector<CapturedSlice>& b,
                      std::string& err)
{
    if (a.size() != b.size())
    {
        err = "range count mismatch (legacy=" + std::to_string(a.size())
            + " unified=" + std::to_string(b.size()) + ")";
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].viewId != b[i].viewId ||
            a[i].cascadeIndex != b[i].cascadeIndex ||
            a[i].offset != b[i].offset ||
            a[i].count != b[i].count)
        {
            err = "range[" + std::to_string(i) + "] differs: "
                + "legacy=(view=" + std::to_string(a[i].viewId)
                + " cascade=" + std::to_string((int)a[i].cascadeIndex)
                + " off=" + std::to_string(a[i].offset)
                + " count=" + std::to_string(a[i].count) + ") vs "
                + "unified=(view=" + std::to_string(b[i].viewId)
                + " cascade=" + std::to_string((int)b[i].cascadeIndex)
                + " off=" + std::to_string(b[i].offset)
                + " count=" + std::to_string(b[i].count) + ")";
            return false;
        }
    }
    return true;
}

bool FlagsEqual(const std::vector<CapturedSlice>& a,
                const std::vector<CapturedSlice>& b,
                std::string& err)
{
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].flags.size() != b[i].flags.size())
        {
            err = "slice[" + std::to_string(i) + "] flag-count mismatch";
            return false;
        }
        for (size_t j = 0; j < a[i].flags.size(); ++j)
        {
            if (a[i].flags[j] != b[i].flags[j])
            {
                err = "slice[" + std::to_string(i) + "] instance " + std::to_string(j)
                    + " differs: legacy=" + std::to_string(a[i].flags[j])
                    + " unified=" + std::to_string(b[i].flags[j]);
                return false;
            }
        }
    }
    return true;
}

uint32_t VisibleCount(const CapturedSlice& s)
{
    uint32_t n = 0;
    for (uint32_t w : s.flags)
        if (w != 0u)
            ++n;
    return n;
}

// Build a deterministic synthetic scene: instances on a 3D grid + a few
// guaranteed-outside-cascade-frustum positions to ensure non-trivial culling.
// Most instances cast shadows (cascade dispatches drop non-casters); every
// 7th does not, so the caster-drop path is exercised alongside frustum drops.
void PopulateScene(GPUScene& scene, uint32_t instanceCount)
{
    for (uint32_t i = 0; i < instanceCount; ++i)
    {
        GPUInstance inst{};
        inst.transform = Matrix4x4::Identity();
        inst.prevTransform = Matrix4x4::Identity();
        inst.normalMatrixCol0 = Vector3(1, 0, 0);
        inst.normalMatrixCol1 = Vector4(0, 1, 0, 0);
        inst.normalMatrixCol2 = Vector4(0, 0, 1, 0);
        inst.meshIndex = 0;
        inst.materialIndex = 0;
        inst.flags = (i % 7u == 0u) ? 0u : 1u; // bit 0 = castShadows
        inst.lodBias = 0.0f;
        // Spread instances across a -50..+50 cube so different cascades catch
        // distinct subsets when the frusta sample different sub-volumes.
        const float x = static_cast<float>((int32_t)(i % 10u) - 5) * 10.0f;
        const float y = static_cast<float>((int32_t)((i / 10u) % 5u) - 2) * 10.0f;
        const float z = static_cast<float>((int32_t)(i / 50u) - 1) * 25.0f;
        inst.boundingCenter = Vector3(x, y, z);
        inst.boundingRadius = 1.0f;
        scene.AddInstance(inst);
    }
}

// Four distinct frusta. We use ortho-style VPs at four different depth bands
// so cascades naturally catch overlapping-but-distinct subsets.
void BuildCascadeVPs(Matrix4x4 outVPs[4])
{
    for (int c = 0; c < 4; ++c)
    {
        const float halfExtent = 30.0f + 15.0f * static_cast<float>(c);
        Matrix4x4 proj = GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ(
            -halfExtent, halfExtent,
            -halfExtent, halfExtent,
            0.1f, 500.0f);
        Matrix4x4 view = Matrix4x4::LookAt(
            Vector3(0.0f, 0.0f, -100.0f - 5.0f * c),
            Vector3(0.0f, 0.0f, 0.0f),
            Vector3(0.0f, 1.0f, 0.0f));
        outVPs[c] = proj * view;
    }
}

// Degenerate plane: dot(c, normalize(n)) + w < -radius is trivially true for
// any sphere because w dominates. Mirrors the production sentinel used in
// ShadowMapRenderFeature for empty cascades.
constexpr float kDegenerateW = -1.0e38f;
const Vector4 kDegeneratePlane{1.0f, 0.0f, 0.0f, kDegenerateW};

// Execute the declared culling frame with a test-owned readback: copy the
// frame's visibility buffer into a readback buffer, wait idle, and slice the
// mapped words per published range.
bool ExecuteAndCapture(IDevice* device,
                       RG::RGFrame& frame,
                       GPUCullingPipeline& pipeline,
                       std::vector<CapturedSlice>& outSlices)
{
    const RG::RGBuffer visRG = pipeline.GetVisibilityRG();
    if (!visRG.IsValid())
    {
        std::cerr << "  visibility RGBuffer invalid — submissions were dropped\n";
        return false;
    }

    const auto ranges = pipeline.GetViewVisibilityRanges(); // copy: outlives Execute
    uint64_t totalElems = 0;
    for (const auto& r : ranges)
        totalElems = std::max<uint64_t>(totalElems, static_cast<uint64_t>(r.visibilityOffset) +
                                                        r.visibilityCount);
    const size_t bytes = std::max<size_t>(4, static_cast<size_t>(totalElems) * sizeof(uint32_t));

    const BufferHandle readback = device->CreateReadbackBuffer(bytes, "ParityVisibilityReadback");
    if (!readback.IsValid())
    {
        std::cerr << "  readback buffer creation failed\n";
        return false;
    }

    frame.AddPass("Parity.VisibilityReadback",
                  0,
                  [&](RG::RGPassBuilder& p)
                  {
                      p.Read(visRG, RG::RGBufferRead::CopySrc);
                      p.PreventCulling(); // consumed by the CPU, not the graph
                  },
                  [visRG, readback, bytes](RG::RGContext& ctx)
                  { ctx.Cmd->CopyBuffer(ctx.GetBuffer(visRG), readback, bytes, 0, 0); });

    frame.Execute();
    device->WaitForIdle();

    bool ok = false;
    if (void* mapped = device->MapBuffer(readback))
    {
        const uint32_t* words = static_cast<const uint32_t*>(mapped);
        outSlices.clear();
        outSlices.reserve(ranges.size());
        for (const auto& r : ranges)
        {
            CapturedSlice s;
            s.viewId = r.viewId;
            s.cascadeIndex = r.cascadeIndex;
            s.offset = r.visibilityOffset;
            s.count = r.visibilityCount;
            s.flags.assign(words + r.visibilityOffset, words + r.visibilityOffset + r.visibilityCount);
            outSlices.push_back(std::move(s));
        }
        device->UnmapBuffer(readback);
        ok = true;
    }
    else
    {
        std::cerr << "  readback map failed\n";
    }
    device->DestroyBuffer(readback);
    return ok;
}

bool RunLegacyAndCapture(IDevice* device,
                         RG::RGFrame& frame,
                         uint64_t frameIndex,
                         GPUScene& scene,
                         const Matrix4x4 cascadeVPs[4],
                         uint32_t cascadeCount,
                         uint32_t instanceCount,
                         const bool cascadeLive[4],
                         std::vector<CapturedSlice>& outSlices)
{
    auto pipeline = GPUCullingFactory::CreateBalanced(device);

    frame.BeginFrame(frameIndex);
    pipeline->BeginFrame(&frame, &scene);

    // For parity, both paths submit ALL N cascades. Dead cascades use the
    // same degenerate planes as the unified path, so the per-cascade
    // visibility slices must come out byte-identical between the two paths.
    for (uint32_t c = 0; c < cascadeCount; ++c)
    {
        ViewCullingInput input{};
        input.viewId = 1;
        input.cascadeIndex = static_cast<uint8_t>(c);
        input.viewMatrix = cascadeVPs[c];
        input.projMatrix = cascadeVPs[c];
        input.viewProjMatrix = cascadeVPs[c];
        if (cascadeLive && !cascadeLive[c])
        {
            for (int p = 0; p < 6; ++p)
                input.frustumPlanes[p] = kDegeneratePlane;
        }
        else
        {
            ExtractFrustumPlanes(cascadeVPs[c], input.frustumPlanes);
        }
        input.cameraPosition = Vector3(0.0f, 0.0f, -100.0f);
        input.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
        input.firstInstance = 0;
        input.instanceCount = instanceCount;
        input.frameIndex = 1;
        input.deltaTime = 1.0f / 60.0f;
        pipeline->SubmitView(input);
    }

    pipeline->EndFrame();
    return ExecuteAndCapture(device, frame, *pipeline, outSlices);
}

bool RunUnifiedAndCapture(IDevice* device,
                          RG::RGFrame& frame,
                          uint64_t frameIndex,
                          GPUScene& scene,
                          const Matrix4x4 cascadeVPs[4],
                          uint32_t cascadeCount,
                          uint32_t instanceCount,
                          const bool cascadeLive[4],
                          std::vector<CapturedSlice>& outSlices)
{
    auto pipeline = GPUCullingFactory::CreateBalanced(device);

    frame.BeginFrame(frameIndex);
    pipeline->BeginFrame(&frame, &scene);

    CascadeCullingGroup group{};
    group.viewId = 1;
    group.cascadeCount = cascadeCount;
    group.cameraPosition = Vector3(0.0f, 0.0f, -100.0f);
    group.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
    group.firstInstance = 0;
    group.instanceCount = instanceCount;
    group.frameIndex = 1;
    group.deltaTime = 1.0f / 60.0f;

    for (uint32_t c = 0; c < cascadeCount; ++c)
    {
        group.lightVP[c] = cascadeVPs[c];
        if (cascadeLive && !cascadeLive[c])
        {
            for (int p = 0; p < 6; ++p)
                group.frustumPlanes[c][p] = kDegeneratePlane;
        }
        else
        {
            Vector4 planes[6]{};
            ExtractFrustumPlanes(cascadeVPs[c], planes);
            for (int p = 0; p < 6; ++p)
                group.frustumPlanes[c][p] = planes[p];
        }
    }
    // Always submit even if all cascades are degenerate — this directly
    // exercises the kViewCount=N PSO at runtime regardless of liveness.
    // (Production's "skip all-empty group" optimization lives one layer up
    // in ShadowMapRenderFeature, not in GPUCullingPipeline.)
    pipeline->SubmitCascadeGroup(group);

    pipeline->EndFrame();
    return ExecuteAndCapture(device, frame, *pipeline, outSlices);
}

// Color fan-out (reflection probe faces): N independent cascade-None
// SubmitView dispatches (today's per-face path: keep non-casters, conservative
// margin on every slice) must be byte-identical to ONE CascadeCullingGroup
// with shadowCasterDispatch=false, whose slices publish at
// cascadeIndexBase + c instead of 0..N-1.
bool RunColorFanOutCase(IDevice* device,
                        RG::RGFrame& frame,
                        uint64_t& frameCounter,
                        GPUScene& scene,
                        const Matrix4x4 faceVPs[4],
                        uint32_t faceCount,
                        uint32_t instanceCount)
{
    constexpr uint8_t kFaceBase = 0x80u;
    std::cout << "\n--- Case: color fan-out (N=" << faceCount << ", base 0x80) ---\n";

    std::vector<CapturedSlice> legacy;
    {
        auto pipeline = GPUCullingFactory::CreateBalanced(device);
        frame.BeginFrame(frameCounter++);
        pipeline->BeginFrame(&frame, &scene);
        for (uint32_t f = 0; f < faceCount; ++f)
        {
            ViewCullingInput input{};
            input.viewId = 1;
            input.cascadeIndex = kCullingCascadeIndexNone;
            input.viewMatrix = faceVPs[f];
            input.projMatrix = faceVPs[f];
            input.viewProjMatrix = faceVPs[f];
            ExtractFrustumPlanes(faceVPs[f], input.frustumPlanes);
            input.cameraPosition = Vector3(0.0f, 0.0f, -100.0f);
            input.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
            input.firstInstance = 0;
            input.instanceCount = instanceCount;
            input.frameIndex = 1;
            input.deltaTime = 1.0f / 60.0f;
            pipeline->SubmitView(input);
        }
        pipeline->EndFrame();
        if (!ExecuteAndCapture(device, frame, *pipeline, legacy))
        {
            std::cerr << "  ❌ legacy path failed\n";
            return false;
        }
    }

    std::vector<CapturedSlice> unified;
    {
        auto pipeline = GPUCullingFactory::CreateBalanced(device);
        frame.BeginFrame(frameCounter++);
        pipeline->BeginFrame(&frame, &scene);
        CascadeCullingGroup group{};
        group.viewId = 1;
        group.cascadeCount = faceCount;
        group.cascadeIndexBase = kFaceBase;
        group.shadowCasterDispatch = false;
        group.cameraPosition = Vector3(0.0f, 0.0f, -100.0f);
        group.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
        group.firstInstance = 0;
        group.instanceCount = instanceCount;
        group.frameIndex = 1;
        group.deltaTime = 1.0f / 60.0f;
        for (uint32_t f = 0; f < faceCount; ++f)
        {
            group.lightVP[f] = faceVPs[f];
            Vector4 planes[6]{};
            ExtractFrustumPlanes(faceVPs[f], planes);
            for (int p = 0; p < 6; ++p)
                group.frustumPlanes[f][p] = planes[p];
        }
        pipeline->SubmitCascadeGroup(group);
        pipeline->EndFrame();
        if (!ExecuteAndCapture(device, frame, *pipeline, unified))
        {
            std::cerr << "  ❌ unified path failed\n";
            return false;
        }
    }

    if (legacy.size() != faceCount || unified.size() != faceCount)
    {
        std::cerr << "  ❌ slice count: legacy=" << legacy.size() << " unified=" << unified.size()
                  << " expected=" << faceCount << "\n";
        return false;
    }
    for (uint32_t f = 0; f < faceCount; ++f)
    {
        std::cout << "  face " << f << ": visible legacy=" << VisibleCount(legacy[f])
                  << " unified=" << VisibleCount(unified[f]) << " / " << legacy[f].count << "\n";
        if (unified[f].cascadeIndex != static_cast<uint8_t>(kFaceBase + f))
        {
            std::cerr << "  ❌ slice " << f << " published at cascade "
                      << (int)unified[f].cascadeIndex << ", expected " << (int)(kFaceBase + f) << "\n";
            return false;
        }
        if (legacy[f].offset != unified[f].offset || legacy[f].count != unified[f].count)
        {
            std::cerr << "  ❌ slice " << f << " layout differs\n";
            return false;
        }
    }
    std::string err;
    if (!FlagsEqual(legacy, unified, err))
    {
        std::cerr << "  ❌ raw visibility-flag parity: " << err << "\n";
        return false;
    }
    std::cout << "  ✓ ranges at base + c, raw visibility flags identical to N single dispatches\n";
    return true;
}

// Run a single (N, liveMask) parity case and return true if it passes.
bool RunParityCase(IDevice* device,
                   RG::RGFrame& frame,
                   uint64_t& frameCounter,
                   GPUScene& scene,
                   const Matrix4x4 cascadeVPs[4],
                   uint32_t cascadeCount,
                   uint32_t instanceCount,
                   const bool cascadeLive[4],
                   const std::string& caseName)
{
    std::cout << "\n--- Case: " << caseName << " (N=" << cascadeCount << ") ---\n";

    std::vector<CapturedSlice> legacy;
    std::vector<CapturedSlice> unified;

    if (!RunLegacyAndCapture(device, frame, frameCounter++, scene, cascadeVPs, cascadeCount,
                             instanceCount, cascadeLive, legacy))
    {
        std::cerr << "  ❌ legacy path failed\n";
        return false;
    }
    if (!RunUnifiedAndCapture(device, frame, frameCounter++, scene, cascadeVPs, cascadeCount,
                              instanceCount, cascadeLive, unified))
    {
        std::cerr << "  ❌ unified path failed\n";
        return false;
    }

    std::cout << "  legacy slices=" << legacy.size()
              << " unified slices=" << unified.size() << "\n";
    for (size_t i = 0; i < std::min(legacy.size(), unified.size()); ++i)
        std::cout << "  cascade " << (int)legacy[i].cascadeIndex << ": visible legacy="
                  << VisibleCount(legacy[i]) << " unified=" << VisibleCount(unified[i])
                  << " / " << legacy[i].count << "\n";

    std::string err;
    if (!RangesEquivalent(legacy, unified, err))
    {
        std::cerr << "  ❌ range equivalence: " << err << "\n";
        return false;
    }
    if (!FlagsEqual(legacy, unified, err))
    {
        std::cerr << "  ❌ raw visibility-flag parity: " << err << "\n";
        return false;
    }
    std::cout << "  ✓ ranges + raw visibility flags identical\n";
    return true;
}
} // namespace

int main()
{
    std::cout << "\n🧪 Cascade Culling Unified Parity Test Suite\n";
    std::cout <<   "=============================================\n";

    GPUScene::SetCullingShaderLoader(&CullingShaderPkgLoader);

    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = "CascadeCullingUnifiedParityTests";
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDebugLayer = false;

    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
    {
        std::cerr << "GPU device unavailable; skipping parity tests\n";
        return kSkipExitCode;
    }

    int exitCode = 0;
    {
        auto scene = GPUSceneFactory::CreateLargeScene(device.get());

        constexpr uint32_t kInstanceCount = 50;
        PopulateScene(*scene, kInstanceCount);
        if (scene->IsDirty())
            scene->FlushGPUBuffers();

        Matrix4x4 cascadeVPs[4]{};
        BuildCascadeVPs(cascadeVPs);

        FramePools pools(device.get());
        RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        uint64_t frameCounter = 0;

        struct Case
        {
            const char* name;
            uint32_t cascadeCount;
            bool live[4];
        };

        // Sweep every kViewCount spec-const variant {1..4}, plus mixed
        // live/dead patterns that exercise the degenerate-frustum path.
        // The "live" mask only matters for N>=2; for N=1 the single cascade
        // is always live (the production main-view path).
        const Case cases[] = {
            {"N=1 single view",              1, {true,  false, false, false}},
            {"N=2 both live",                2, {true,  true,  false, false}},
            {"N=2 first dead, second live",  2, {false, true,  false, false}},
            {"N=3 all live",                 3, {true,  true,  true,  false}},
            {"N=3 middle dead",              3, {true,  false, true,  false}},
            {"N=4 all live (production)",    4, {true,  true,  true,  true }},
            {"N=4 outer cascades dead",      4, {false, true,  true,  false}},
            {"N=4 only cascade 0 live",      4, {true,  false, false, false}},
        };

        int passed = 0;
        int failed = 0;
        for (const auto& c : cases)
        {
            if (RunParityCase(device.get(), frame, frameCounter, *scene, cascadeVPs,
                              c.cascadeCount, kInstanceCount, c.live, c.name))
                ++passed;
            else
                ++failed;
        }
        // The probe path fans six faces into a 4-slice and a 2-slice group.
        for (const uint32_t faceCount : {4u, 2u, 1u})
        {
            if (RunColorFanOutCase(device.get(), frame, frameCounter, *scene, cascadeVPs,
                                   faceCount, kInstanceCount))
                ++passed;
            else
                ++failed;
        }

        if (!g_ShaderPkgPath.empty())
            std::cout << "\nLoaded frustum_culling.shaderpkg from " << g_ShaderPkgPath.string() << "\n";

        std::cout << "\n📊 Parity summary: " << passed << "/" << (passed + failed)
                  << " cases passed\n";
        if (g_ShaderPkgPath.empty())
        {
            // The fallback bytes are not the shader under test, so a green parity
            // result attests to nothing. Report unrun rather than passed.
            std::cerr << "frustum_culling.shaderpkg not found — the parity cases ran against "
                         "fallback bytes and prove nothing about the real shader; reporting skip\n";
            exitCode = kSkipExitCode;
        }
        else if (failed > 0)
        {
            std::cerr << "❌ " << failed << " case(s) failed\n";
            exitCode = 1;
        }
        else
        {
            std::cout << "✅ All cascade-culling parity assertions passed across N=1..4.\n";
        }
        device->WaitForIdle();
    }
    device->Shutdown();
    return exitCode;
}
