// Which depth the auto-exposure meter splits the frame with, and who owns the depth with the water in
// it: the ocean's surface step resolves it once and publishes View.DepthResolvedPostOcean; the fog and
// the meter read it when it resolves, else View.DepthResolved. Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanRenderNode.h"
#include "RGPassQuery.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "StagedTestPaths.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
namespace RGQuery = GameEngine::Testing::RGQuery;

namespace
{
struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = 1;
    d.format = static_cast<uint32_t>(format);
    d.usage = usage;
    return d;
}
TextureDesc ColorTargetDesc()
{
    return TargetDesc(TextureFormat::RGBA8_UNORM,
                      static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource));
}
TextureDesc DepthTargetDesc()
{
    return TargetDesc(TextureFormat::D32_FLOAT,
                      static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource));
}
} // namespace

namespace
{
// Which depth the auto-exposure meter splits the frame with, and who resolves the depth with the
// water in it. The ocean's post-world step (it draws the surface) resolves the view depth once after
// the surface and publishes View.DepthResolvedPostOcean; fog and auto exposure read it when it
// resolves, else View.DepthResolved.
struct MeteringDepthFixture
{
    std::unique_ptr<IDevice> Device;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    Rendering::ViewId View{};

    bool Up(float waterCenterZ, bool fog)
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        EXPECT_TRUE(Rs->Initialize(Device.get()));
        // The ocean surface material compiles through the real composer from the staged shader
        // mirror (the host normally provides this context); without it the surface never draws.
        Rendering::MaterialBuildContext mbc{};
        mbc.AdapterShaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
        mbc.CacheRoot = std::filesystem::temp_directory_path() / "ge_metering_depth_cache";
        mbc.IncludeDirs = {mbc.AdapterShaderDir};
        mbc.PackageShaderDirs = {mbc.AdapterShaderDir};
        Rs->Materials().SetMaterialBuildContext(mbc);
        // Looking along +Z from 50 m above the sea: water 150 m ahead is in view, 150 m behind is not.
        const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
            Mathematics::Vector3(0.0f, 50.0f, 0.0f), Mathematics::Vector3(0.0f, 50.0f, 1.0f),
            Mathematics::Vector3(0.0f, 1.0f, 0.0f));
        const Mathematics::Matrix4x4 projection =
            Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.5f, 600.0f);
        const Mathematics::Matrix4x4 viewProj = projection * view;
        CameraData camera{};
        std::memcpy(camera.view, view.Data(), sizeof(camera.view));
        std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
        std::memcpy(camera.viewProj, viewProj.Data(), sizeof(camera.viewProj));
        camera.cameraPos[1] = 50.0f;
        const CameraId cam = Rs->Views().AllocateCamera("MeteringDepthCam");
        Rs->Views().SetCameraData(cam, camera);
        View = Rs->Views().AllocateView("MeteringDepthView", cam);
        Rs->Views().SetViewRenderLayerMask(View, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        Rs->Views().SetViewTargets(View, 0, 0, 0, clear);

        PostProcessSettings pp{};
        pp.AutoExposureActive = true;
        pp.VolumetricFogIntensity = fog ? 1.0f : 0.0f;
        pp.VolumetricFogDensity = 0.02f;
        Rs->Views().SetViewPostProcessOverride(View, pp);

        if (waterCenterZ != 0.0f)
        {
            auto& ocean = Rs->EnsureFeature<GameEngine::Ocean::OceanRenderFeature>();
            EXPECT_TRUE(ocean.Initialize(Device.get()));
            ocean.SetHasOcean(true);
            GameEngine::Ocean::OceanRenderFeature::WaterBodyBox water{};
            water.CenterZ = waterCenterZ;
            water.HalfX = 40.0f;
            water.HalfZ = 40.0f;
            ocean.SetWaterBodies({water}, true);
        }

        EXPECT_TRUE(Registry.Register(
            "DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); }, true));
        EXPECT_TRUE(Registry.Register(
            "OceanRender", [] { return std::make_unique<GameEngine::Ocean::OceanRenderNode>(); }, true));
        EXPECT_TRUE(Registry.Register(
            "VolumetricFog", [] { return std::make_unique<Nodes::VolumetricFogNode>(); }, true));
        EXPECT_TRUE(Registry.Register(
            "AutoExposure", [] { return std::make_unique<Nodes::AutoExposureNode>(); }, true));
        return true;
    }

    static RenderPipelineBlueprint::Pass Step(const char* id, const char* type, const char* json)
    {
        RenderPipelineBlueprint::Pass p;
        p.id = id;
        p.type = type;
        p.enabled = true;
        p.perView = true;
        p.passJson = json;
        return p;
    }

    RenderPipelineBlueprint Blueprint(bool withOcean, bool withFog) const
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "MeteringDepth";
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.passes.push_back(Step("DepthResolve", "DepthResolve", R"({"id":"DepthResolve","type":"DepthResolve"})"));
        if (withOcean)
        {
            bp.passes.push_back(Step("Ocean", "OceanRender", R"({"id":"Ocean","type":"OceanRender"})"));
            bp.passes.push_back(Step("OceanUnderwater", "OceanRender",
                                     R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
        }
        if (withFog)
            bp.passes.push_back(Step("Fog", "VolumetricFog", R"({"id":"Fog","type":"VolumetricFog","output":"SceneColor"})"));
        bp.passes.push_back(
            Step("AutoExposure", "AutoExposure", R"({"id":"AutoExposure","type":"AutoExposure","input":"SceneColor"})"));
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        return bp;
    }

    void Down()
    {
        Device->WaitForIdle();
        Rs->Shutdown();
        Device->Shutdown();
    }
};

struct MeteringDepthFrame
{
    std::vector<std::string> MeterDepthReads; // depth-like textures the histogram pass samples
    std::vector<std::string> FogDepthReads;   // depth-like textures the fog passes sample
    uint32_t PostOceanResolves = 0;           // passes that write the post-ocean depth
    bool SurfaceDrawn = false;                // an OceanSurface pass was declared
    bool MeterDeclared = false;
};

MeteringDepthFrame DeclareOneFrame(MeteringDepthFixture& f, RenderPipelineInstance& instance, uint32_t index)
{
    MeteringDepthFrame out;
    FramePools pools(f.Device.get());
    RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(index);
    RenderGraph::RGTexture color = frame.ImportPersistentTexture("MD.Color", ColorTargetDesc());
    RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MD.Depth", DepthTargetDesc());
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);

    const RenderGraph::RGGraph& g = frame.Graph();
    std::set<RenderGraph::RGPassId> resolveWriters;
    for (const RenderGraph::RGAccessRecord& a : g.Accesses())
    {
        const char* pn = g.PassName(a.Pass);
        const char* rn = g.ResourceName(a.Resource);
        if (!pn || !rn)
            continue;
        const std::string pass(pn), res(rn);
        if (pass.find("OceanSurface") != std::string::npos)
            out.SurfaceDrawn = true;
        const bool isDepth = res.find("DepthResolved") != std::string::npos;
        if (res.find("DepthResolvedPostOcean") != std::string::npos && RenderGraph::IsWrite(a.Access))
            resolveWriters.insert(a.Pass);
        const bool sampled = a.Access == RenderGraph::RGAccess::Sampled ||
                             a.Access == RenderGraph::RGAccess::SampledCompute;
        if (pass.find("AutoExposureHistogram") != std::string::npos)
        {
            out.MeterDeclared = true;
            if (sampled && isDepth)
                out.MeterDepthReads.push_back(res);
        }
        if (sampled && isDepth && RGQuery::Matches(pass, RGQuery::Subtree{"VolumetricFog"}))
            out.FogDepthReads.push_back(res);
    }
    out.PostOceanResolves = static_cast<uint32_t>(resolveWriters.size());
    frame.MarkOutput(color);
    frame.Execute();
    f.Device->WaitForIdle();
    return out;
}

// The first frame registers the ocean's surface contributor and its material may compile over a few
// frames: declare until the surface draws, or a bounded number of frames when it never does.
MeteringDepthFrame DeclareMeteringDepthFrame(MeteringDepthFixture& f, bool withOcean, bool withFog)
{
    RenderPipelineInstance instance(*f.Rs, f.Registry);
    instance.SetBlueprint(f.Blueprint(withOcean, withFog));
    constexpr uint32_t kMaxFrames = 30;
    MeteringDepthFrame last;
    for (uint32_t i = 0; i < kMaxFrames; ++i)
    {
        last = DeclareOneFrame(f, instance, i);
        if (last.SurfaceDrawn || !withOcean)
            break;
    }
    return last;
}

bool OnlyPostOcean(const std::vector<std::string>& reads)
{
    return !reads.empty() && std::all_of(reads.begin(), reads.end(), [](const std::string& r)
                                         { return r.find("DepthResolvedPostOcean") != std::string::npos; });
}
bool OnlyViewDepth(const std::vector<std::string>& reads)
{
    return !reads.empty() && std::all_of(reads.begin(), reads.end(), [](const std::string& r)
                                         { return r.find("DepthResolvedPostOcean") == std::string::npos; });
}
} // namespace

// Water in view, no fog: the ocean's surface step resolves the depth once after the water and the
// meter reads that depth, so the sea counts as scene, not sky.
TEST(RenderPipelineDeclareTests, AutoExposureMetersThePostOceanDepthWhenTheWaterDraws)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/150.0f, /*fog=*/false))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/true, /*withFog=*/false);
    ASSERT_TRUE(r.MeterDeclared);
    ASSERT_TRUE(r.SurfaceDrawn) << "the ocean surface must draw for this pin to cover anything";
    EXPECT_EQ(r.PostOceanResolves, 1u);
    EXPECT_TRUE(OnlyPostOcean(r.MeterDepthReads));
    f.Down();
}

// Water and fog: both read the one resolve the ocean step owns; nothing resolves the depth twice.
TEST(RenderPipelineDeclareTests, FogAndAutoExposureShareTheOceanStepsPostOceanResolve)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/150.0f, /*fog=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/true, /*withFog=*/true);
    ASSERT_TRUE(r.MeterDeclared);
    ASSERT_TRUE(r.SurfaceDrawn);
    EXPECT_EQ(r.PostOceanResolves, 1u) << "the fog no longer resolves its own copy";
    EXPECT_TRUE(OnlyPostOcean(r.MeterDepthReads));
    EXPECT_TRUE(OnlyPostOcean(r.FogDepthReads)) << "the fog reads the same post-ocean depth";
    f.Down();
}

// No water and no fog: nothing takes a post-world resolve; the meter reads the view's resolved depth.
TEST(RenderPipelineDeclareTests, AutoExposureMetersTheViewDepthWithoutWaterOrFog)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/false))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/false);
    ASSERT_TRUE(r.MeterDeclared);
    EXPECT_EQ(r.PostOceanResolves, 0u);
    EXPECT_TRUE(OnlyViewDepth(r.MeterDepthReads));
    f.Down();
}

// Fog without water: the fog takes the post-world resolve itself (after every world depth writer, not
// the copy taken right after the prepass), and the meter after it reads the same depth.
TEST(RenderPipelineDeclareTests, FogTakesThePostWorldResolveWithoutWaterAndTheMeterSharesIt)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/0.0f, /*fog=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/false, /*withFog=*/true);
    ASSERT_TRUE(r.MeterDeclared);
    EXPECT_EQ(r.PostOceanResolves, 1u) << "one post-world resolve, declared by the fog";
    EXPECT_TRUE(OnlyPostOcean(r.FogDepthReads)) << "the fog reads the post-world depth";
    EXPECT_TRUE(OnlyPostOcean(r.MeterDepthReads)) << "the meter reads the same depth";
    f.Down();
}

// Water in the scene but behind the camera: the surface does not draw, so no post-ocean resolve is
// declared and the meter reads the view's resolved depth.
TEST(RenderPipelineDeclareTests, WaterOutOfViewDeclaresNoPostOceanResolve)
{
    MeteringDepthFixture f;
    if (!f.Up(/*waterCenterZ=*/-150.0f, /*fog=*/false))
        GTEST_SKIP() << "No Vulkan device available";
    const MeteringDepthFrame r = DeclareMeteringDepthFrame(f, /*withOcean=*/true, /*withFog=*/false);
    ASSERT_TRUE(r.MeterDeclared);
    EXPECT_FALSE(r.SurfaceDrawn);
    EXPECT_EQ(r.PostOceanResolves, 0u);
    EXPECT_TRUE(OnlyViewDepth(r.MeterDepthReads));
    f.Down();
}
