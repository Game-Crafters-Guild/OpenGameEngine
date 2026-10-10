// The forward fragment adapter is compiled once per keyword set the pipeline
// asks for, and the pipeline's keyword sets come from shipped rendergraph JSON.
// Nothing else forces the cross product to exist: a combination no shipped graph
// happened to request could sit uncompilable in the tree indefinitely, and the
// first symptom is a material that fails to build at draw time on whichever
// pipeline the user switched to.
//
// These tests compile the adapter through the real compose + shaderc path for
// every lighting-model x Forward+ x Shadows combination the world pass can
// request, plus the exact set the shipped ForwardPlus_DebugOverlay world pass
// declares — with a companion test that keeps that hand-mirrored set honest
// against the graph on disk.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

// The world draw ORs Instanced onto whatever the pass node declares (entity
// batches always need indirect vertex fetch — RenderServicesWorldPass.cpp), so
// every set below carries it.
constexpr MaterialKeyword kWorldDrawBase = MaterialKeyword::Instanced;

// Assets/RenderPipelines/ForwardPlus_DebugOverlay.rendergraph, "World" pass:
// keywords ["ForwardPlus"] — no Shadows, no IBL. Mirrored by hand because
// ParseWorldPassKeywords lives in the Engine layer, above this module;
// DebugOverlayGraphStillDeclaresTheMirroredKeywords pins the mirror.
constexpr MaterialKeyword kDebugOverlayWorldPass =
    kWorldDrawBase | MaterialKeyword::ForwardPlus;

fs::path MakeTempCacheRoot()
{
    static std::atomic<uint32_t> counter{0};
    return fs::temp_directory_path() /
           ("ge_fwd_variant_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(counter.fetch_add(1)));
}

// A stock material on the engine's built-in surface shader: an empty
// surfaceShader makes the composer substitute Surfaces/standard_surface.glsl,
// which is what an ordinary scene material compiles as.
MaterialBuildResult BuildStockMaterial(const std::string& lightingModel, MaterialKeyword keywords,
                                       const std::string& surfaceShader = {}, bool heightMapped = false)
{
    MaterialDocument doc{};
    doc.materialName = "ForwardAdapterVariantProbe";
    doc.lightingModel = lightingModel;
    doc.surfaceShader = surfaceShader;
    if (heightMapped)
        doc.textures["heightMap"] = "__embedded__:0";

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.CacheRoot = MakeTempCacheRoot();

    // A height-mapped probe lives beside its cache: from the staged engine Materials folder,
    // Surfaces/standard_pbr.glsl resolves to that folder's own copy (Assets/Materials/Surfaces), which
    // declares no heightMap, so the binding would never derive Parallax.
    const fs::path materialPath = heightMapped ? ctx.CacheRoot / "ForwardAdapterVariantProbe.material"
                                               : TestPaths::StagedEngineAssetsDir() / "Materials" /
                                                     "ForwardAdapterVariantProbe.material";
    auto result = BuildMaterialToShaderPackage(doc, materialPath,
                                               "ForwardAdapterVariantProbe", ctx, ShaderSourceKind::SpirV, keywords);
    std::error_code ec;
    fs::remove_all(ctx.CacheRoot, ec);
    return result;
}

#define SKIP_WITHOUT_SHADERC(result)                                     \
    do                                                                   \
    {                                                                    \
        for (const auto& e : (result).errors)                            \
            if (e.find("shaderc is not available") != std::string::npos) \
                GTEST_SKIP() << "shaderc not built into this target";    \
    } while (0)

void ExpectBuildSucceeded(const MaterialBuildResult& result)
{
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
}

// shaderc runs at optimization level zero here, so OpName debug info survives and a
// GLSL identifier that reached the compiled fragment stage is searchable in its SPIR-V.
bool FragmentSpvNames(const MaterialBuildResult& result, const std::string& needle)
{
    // ExpectBuildSucceeded's ASSERTs return from ExpectBuildSucceeded, not from the
    // TEST, so a failed build still reaches here — with no package to read.
    if (result.package == nullptr)
        return false;
    const auto it = result.package->stageBytes.find("fs");
    if (it == result.package->stageBytes.end())
        return false;
    const auto& spv = it->second;
    return std::search(spv.begin(), spv.end(), needle.begin(), needle.end()) != spv.end();
}

// What a compiled fragment stage declares about its depth output: the DepthLess and DepthReplacing
// execution modes and a FragDepth built-in.
struct FragmentDepthDeclarations
{
    bool DepthLess = false;
    bool DepthReplacing = false;
    bool FragDepth = false;
    bool Discards = false; // OpKill, OpTerminateInvocation or OpDemoteToHelperInvocation
};

FragmentDepthDeclarations FragmentDepth(const MaterialBuildResult& result)
{
    constexpr uint32_t kOpExecutionMode = 16;
    constexpr uint32_t kOpDecorate = 71;
    constexpr uint32_t kExecutionModeDepthReplacing = 12;
    constexpr uint32_t kExecutionModeDepthLess = 15;
    constexpr uint32_t kDecorationBuiltIn = 11;
    constexpr uint32_t kBuiltInFragDepth = 22;
    constexpr uint32_t kOpKill = 252;
    constexpr uint32_t kOpTerminateInvocation = 4416;
    constexpr uint32_t kOpDemoteToHelperInvocation = 5380;
    FragmentDepthDeclarations out{};
    if (result.package == nullptr)
        return out;
    const auto it = result.package->stageBytes.find("fs");
    if (it == result.package->stageBytes.end() || it->second.size() % sizeof(uint32_t) != 0)
        return out;
    std::vector<uint32_t> words(it->second.size() / sizeof(uint32_t));
    std::memcpy(words.data(), it->second.data(), it->second.size());
    for (size_t at = 5; at < words.size();)
    {
        const uint32_t count = words[at] >> 16;
        const uint32_t opcode = words[at] & 0xFFFFu;
        if (count == 0 || at + count > words.size())
            break;
        if (opcode == kOpExecutionMode && count >= 3)
        {
            out.DepthLess |= words[at + 2] == kExecutionModeDepthLess;
            out.DepthReplacing |= words[at + 2] == kExecutionModeDepthReplacing;
        }
        if (opcode == kOpDecorate && count >= 4 && words[at + 2] == kDecorationBuiltIn)
            out.FragDepth |= words[at + 3] == kBuiltInFragDepth;
        out.Discards |= opcode == kOpKill || opcode == kOpTerminateInvocation || opcode == kOpDemoteToHelperInvocation;
        at += count;
    }
    return out;
}

} // namespace

TEST(ForwardAdapterVariantCompile, ShippedDebugOverlayWorldPassSetCompiles)
{
    // ForwardPlus_DebugOverlay puts every StandardPBR material in the scene on
    // this variant. It is the shipped path, so it must build.
    const auto result = BuildStockMaterial("StandardPBR", kDebugOverlayWorldPass);
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
}

TEST(ForwardAdapterVariantCompile, DebugOverlayGraphStillDeclaresTheMirroredKeywords)
{
    // Positive control for the constant above: if the shipped graph gains or
    // loses a keyword, the test above stops testing the shipped variant and this
    // one says so instead of quietly passing.
    const fs::path graph =
        TestPaths::StagedEngineAssetsDir() / "RenderPipelines" / "ForwardPlus_DebugOverlay.rendergraph";
    std::ifstream in(graph, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "shipped graph not found at " << graph.string();

    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "shipped graph is not valid JSON: " << graph.string();
    ASSERT_TRUE(j.contains("passes"));

    bool sawWorldPass = false;
    for (const auto& pass : j["passes"])
    {
        if (!pass.contains("type") || pass["type"] != "WorldRender")
            continue;
        sawWorldPass = true;
        const std::vector<std::string> keywords =
            pass.contains("keywords") ? pass["keywords"].get<std::vector<std::string>>()
                                      : std::vector<std::string>{};
        EXPECT_EQ(keywords, (std::vector<std::string>{"ForwardPlus"}))
            << "kDebugOverlayWorldPass no longer mirrors the shipped world pass";
    }
    EXPECT_TRUE(sawWorldPass) << "no WorldRender pass in " << graph.string();
}

TEST(ForwardAdapterVariantCompile, EveryPassKeywordCombinationCompilesForEveryLightingModel)
{
    // ParseWorldPassKeywords recognises exactly ForwardPlus, Instanced, Shadows
    // and IBL, so a pass node can author any subset of those; Instanced is always
    // present by the time the draw resolves. WorldRenderNode then ORs GTAO on top
    // per VIEW (whenever an AO PostProcessVolume is active), so it is an axis no
    // graph authors but every pass can reach. Crossed with the material's lighting
    // model, this is the whole space authored content can reach.
    for (const char* lightingModel : {"StandardPBR", "ShadowOnly", "Unlit"})
    {
        for (bool forwardPlus : {false, true})
        {
            for (bool shadows : {false, true})
            {
                for (bool ibl : {false, true})
                {
                    for (bool gtao : {false, true})
                    {
                        MaterialKeyword keywords = kWorldDrawBase;
                        if (forwardPlus)
                            keywords |= MaterialKeyword::ForwardPlus;
                        if (shadows)
                            keywords |= MaterialKeyword::Shadows;
                        if (ibl)
                            keywords |= MaterialKeyword::IBL;
                        if (gtao)
                            keywords |= MaterialKeyword::GTAO;

                        SCOPED_TRACE(std::string(lightingModel) +
                                     (forwardPlus ? " +ForwardPlus" : "") +
                                     (shadows ? " +Shadows" : "") + (ibl ? " +IBL" : "") +
                                     (gtao ? " +GTAO" : ""));
                        const auto result = BuildStockMaterial(lightingModel, keywords);
                        SKIP_WITHOUT_SHADERC(result);
                        ExpectBuildSucceeded(result);
                    }
                }
            }
        }
    }
}

// Material occlusion is an AMBIENT term, and the ambient term lives in ibl.glsl, so the
// variant that matters is IBL-on / GTAO-off: what the default editor and every scene
// without an AO PostProcessVolume actually renders. A keyword branch there silently
// dropped occlusion maps from indirect diffuse. Probing the compiled fragment SPIR-V
// rather than the shader source makes this a property of the shipped artifact: the
// occlusion response must be reachable from main() in BOTH variants.
TEST(ForwardAdapterVariantCompile, SurfaceOcclusionReachesIndirectDiffuseWithAndWithoutGtao)
{
    constexpr MaterialKeyword kIblWorldPass =
        kWorldDrawBase | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
        MaterialKeyword::IBL;

    for (const bool gtao : {false, true})
    {
        SCOPED_TRACE(gtao ? "GTAO on" : "GTAO off (the default editor)");
        const auto keywords = gtao ? (kIblWorldPass | MaterialKeyword::GTAO) : kIblWorldPass;
        const auto result = BuildStockMaterial("StandardPBR", keywords);
        SKIP_WITHOUT_SHADERC(result);
        ExpectBuildSucceeded(result);
        EXPECT_TRUE(FragmentSpvNames(result, "GE_GtaoMultiBounce"))
            << "the occlusion response must reach indirect diffuse in this variant; "
               "gating it on the GTAO keyword discards every material occlusion map "
               "in scenes without an AO volume";
    }
}

// SSR uses actual forward material outputs, so test the relevant material
// cross-product through compose + shaderc instead of only compiling SSR itself.
TEST(ForwardAdapterVariantCompile, SssrExportsActualBaseLobeAcrossLayeredMaterials)
{
    const auto ssr = kWorldDrawBase | MaterialKeyword::ForwardPlus | MaterialKeyword::IBL |
                     MaterialKeyword::SSSRNormalRoughness;
    for (auto extra : {MaterialKeyword::None, MaterialKeyword::GTAO,
        MaterialKeyword::ClearCoat | MaterialKeyword::CoatNormal | MaterialKeyword::Fuzz,
        MaterialKeyword::Anisotropy | MaterialKeyword::Iridescence | MaterialKeyword::DDGI})
    {
        SCOPED_TRACE(static_cast<uint64_t>(extra));
        const auto result = BuildStockMaterial("StandardPBR", ssr | extra);
        SKIP_WITHOUT_SHADERC(result);
        ExpectBuildSucceeded(result);
        EXPECT_TRUE(FragmentSpvNames(result, "oSpecularWeight"));
        EXPECT_TRUE(FragmentSpvNames(result, "oSpecularRadiance"));
    }
}

// The glass tint variant the light-space cascade draws writes the transmittance
// attachment, so it declares the colour output; the depth-only coverage shape
// declares none. A key carrying both is refused by name instead of failing on the
// undeclared output inside the tint block.
TEST(ForwardAdapterVariantCompile, GlassTintVariantCompilesAndRefusesTheDepthOnlyShapeByName)
{
    constexpr MaterialKeyword kGlassTint = kWorldDrawBase | MaterialKeyword::Transmission |
                                           MaterialKeyword::TransmissionThick |
                                           MaterialKeyword::DepthOnlyTransmissionColor;
    const auto tint = BuildStockMaterial("StandardPBR", kGlassTint, "Surfaces/standard_pbr.glsl");
    SKIP_WITHOUT_SHADERC(tint);
    ExpectBuildSucceeded(tint);
    EXPECT_TRUE(FragmentSpvNames(tint, "oColor")) << "the tint variant lost its transmittance output";

    const auto both = BuildStockMaterial(
        "StandardPBR", kGlassTint | MaterialKeyword::DepthOnlyFragment | MaterialKeyword::LodCrossfade,
        "Surfaces/standard_pbr.glsl");
    EXPECT_FALSE(both.success) << "a depth-only glass tint variant composed";
    bool named = false;
    for (const auto& e : both.errors)
        named = named || e.find("GE_DEPTH_ONLY_FRAGMENT and GE_GLASS_SHADOW_COLOR are different attachment shapes") !=
                             std::string::npos;
    EXPECT_TRUE(named) << "the refusal does not name the two keywords";
}

TEST(ForwardAdapterVariantCompile, SssrUnlitVariantKeepsSpecularOutputsInert)
{
    const auto result = BuildStockMaterial("Unlit", kWorldDrawBase | MaterialKeyword::SSSRNormalRoughness);
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
}

TEST(ForwardAdapterVariantCompile, ShippedTexturedSurfacesUseAdapterTextureAliases)
{
    for (const auto* surface : {"standard_pbr", "textured_pbr", "unlit_textured"})
    {
        SCOPED_TRACE(surface);
        const std::string model = std::string(surface) == "unlit_textured" ? "Unlit" : "StandardPBR";
        for (const auto extra : {MaterialKeyword::None, MaterialKeyword::SSSRNormalRoughness})
        {
            const auto result = BuildStockMaterial(model,
                kWorldDrawBase | MaterialKeyword::ForwardPlus | MaterialKeyword::IBL | extra,
                std::string("Surfaces/") + surface + ".glsl");
            SKIP_WITHOUT_SHADERC(result);
            ExpectBuildSucceeded(result);
        }
    }
}

// The relief's depth is the only thing that gives a fragment stage a depth output: a height-mapped
// material's prepass head and its colour pipeline without a prepass (ParallaxDepthOffset), and its
// colour pipeline after a prepass (ParallaxDepthFromPrepass), which tests at the depth it reads and does
// not march, declare a depth_less FragDepth; without those keywords none does.
TEST(ForwardAdapterVariantCompile, TheReliefDepthDeclaresDepthLessOnlyUnderItsKeyword)
{
    constexpr MaterialKeyword kColour = kWorldDrawBase | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
    constexpr MaterialKeyword kPrepass = kWorldDrawBase | MaterialKeyword::DepthOnlyFragment;
    constexpr MaterialKeyword kOffset = MaterialKeyword::ParallaxDepthOffset;
    constexpr MaterialKeyword kFromPrepass = MaterialKeyword::ParallaxDepthFromPrepass;
    constexpr MaterialKeyword kMultisample = MaterialKeyword::ParallaxPrepassDepthMultisample;
    constexpr MaterialKeyword kTolerance = MaterialKeyword::ParallaxDepthTolerance;
    for (const MaterialKeyword keywords : {kColour | kOffset, kColour | kOffset | kTolerance, kPrepass | kOffset,
                                           kColour | kFromPrepass, kColour | kFromPrepass | kMultisample})
    {
        SCOPED_TRACE(static_cast<uint64_t>(keywords));
        const auto result = BuildStockMaterial("StandardPBR", keywords, "Surfaces/standard_pbr.glsl", true);
        SKIP_WITHOUT_SHADERC(result);
        ExpectBuildSucceeded(result);
        const FragmentDepthDeclarations depth = FragmentDepth(result);
        EXPECT_TRUE(depth.DepthLess);
        EXPECT_TRUE(depth.DepthReplacing);
        EXPECT_TRUE(depth.FragDepth);
        const bool fromPrepass = HasKeyword(keywords, kFromPrepass);
        EXPECT_EQ(FragmentSpvNames(result, "ge_prepassDepth"), fromPrepass);
        // The read variant discards where the depth it read is a nearer surface inside the relief; the
        // opaque writers have nothing to discard. Only the kill instruction's presence is checked: a
        // discard whose condition is made always false keeps it, so only a deleted discard reads red.
        EXPECT_EQ(depth.Discards, fromPrepass);
        if (fromPrepass)
            EXPECT_FALSE(FragmentSpvNames(result, "GE_ParallaxMarch(")) << "the colour pass reads the hit, never marches";
    }
    for (const MaterialKeyword keywords : {kColour, kPrepass})
    {
        SCOPED_TRACE(static_cast<uint64_t>(keywords));
        const auto result = BuildStockMaterial("StandardPBR", keywords, "Surfaces/standard_pbr.glsl", true);
        SKIP_WITHOUT_SHADERC(result);
        ExpectBuildSucceeded(result);
        const FragmentDepthDeclarations depth = FragmentDepth(result);
        EXPECT_FALSE(depth.DepthLess);
        EXPECT_FALSE(depth.DepthReplacing);
        EXPECT_FALSE(depth.FragDepth);
        EXPECT_FALSE(FragmentSpvNames(result, "ge_prepassDepth"));
    }
}

// A material that does not march compiles the same program whether or not the pass asked for the
// relief's depth, so turning it on for the world pass and the prepass recompiles nothing else.
TEST(ForwardAdapterVariantCompile, TheReliefDepthKeywordsLeaveAFlatMaterialsProgramUnchanged)
{
    constexpr MaterialKeyword kColour = kWorldDrawBase | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
    constexpr MaterialKeyword kPrepass = kWorldDrawBase | MaterialKeyword::DepthOnlyFragment;
    constexpr MaterialKeyword kRelief = MaterialKeyword::ParallaxDepthOffset | MaterialKeyword::ParallaxDepthFromPrepass |
                                        MaterialKeyword::ParallaxPrepassDepthMultisample |
                                        MaterialKeyword::ParallaxDepthTolerance;
    for (const MaterialKeyword keywords : {kColour, kPrepass})
    {
        SCOPED_TRACE(static_cast<uint64_t>(keywords));
        const auto without = BuildStockMaterial("StandardPBR", keywords, "Surfaces/standard_pbr.glsl");
        const auto with = BuildStockMaterial("StandardPBR", keywords | kRelief, "Surfaces/standard_pbr.glsl");
        SKIP_WITHOUT_SHADERC(without);
        ExpectBuildSucceeded(without);
        ExpectBuildSucceeded(with);
        ASSERT_TRUE(without.package && with.package);
        EXPECT_EQ(without.package->stageBytes.at("fs"), with.package->stageBytes.at("fs"));
        EXPECT_EQ(without.package->stageBytes.at("vs"), with.package->stageBytes.at("vs"));
    }
}

namespace
{
// Whether a compiled program's fragment stage reads the view's metered exposure (set 0, binding 47).
bool FragmentReadsTheViewsExposure(const MaterialBuildResult& result)
{
    constexpr uint32_t kFragmentStageBit = 1u << 1;
    if (result.package == nullptr)
        return false;
    for (const DescriptorSetMeta& set : result.package->meta.Sets)
        for (const DescriptorBindingMeta& binding : set.Bindings)
            if (set.Set == 0u && binding.Name == "ExposureHistory" && binding.Binding == 47u &&
                (binding.StagesMask & kFragmentStageBit) != 0u)
                return true;
    return false;
}
} // namespace

// The standard surfaces opt into the emissive exposure weight, so their shading variant reads the
// view's exposure; their depth-only variant shades nothing and reads none, and a surface that does not
// opt in reads none in any variant. The motion-vector shape is pinned with the motion variant's tests
// (DeformationMotionVariantTests.cpp), which compose it on a surface with a vertex modifier.
TEST(ForwardAdapterVariantCompile, OnlyTheStandardSurfacesShadingVariantReadsTheViewsExposure)
{
    constexpr MaterialKeyword kColour = kWorldDrawBase | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
    constexpr MaterialKeyword kDepth = kWorldDrawBase | MaterialKeyword::DepthOnlyFragment;
    for (const char* surface : {"Surfaces/standard_pbr.glsl", "Surfaces/standard_pbr_extended.glsl"})
    {
        SCOPED_TRACE(surface);
        const auto colour = BuildStockMaterial("StandardPBR", kColour, surface);
        SKIP_WITHOUT_SHADERC(colour);
        ExpectBuildSucceeded(colour);
        EXPECT_TRUE(FragmentReadsTheViewsExposure(colour));
        const auto depth = BuildStockMaterial("StandardPBR", kDepth, surface);
        ExpectBuildSucceeded(depth);
        EXPECT_FALSE(FragmentReadsTheViewsExposure(depth));
    }
    const auto plain = BuildStockMaterial("StandardPBR", kColour);
    ExpectBuildSucceeded(plain);
    EXPECT_FALSE(FragmentReadsTheViewsExposure(plain)) << "a surface without the opt-in reads no exposure";
}

namespace
{
constexpr uint32_t kProbeWidth = 64;
constexpr uint32_t kProbeHeight = 8;

std::vector<uint8_t> DrawAdapterProbe(IDevice& device,
                                     const ShaderProgramCompileResult& program,
                                     bool alphaToCoverage)
{
    TextureDesc texture{};
    texture.width = kProbeWidth;
    texture.height = kProbeHeight;
    texture.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    texture.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    texture.sampleCount = alphaToCoverage ? 4u : 1u;
    const TextureHandle target = device.CreateTexture(texture);
    texture.sampleCount = 1;
    const TextureHandle resolved = alphaToCoverage ? device.CreateTexture(texture) : target;
    const size_t bytes = size_t(kProbeWidth) * kProbeHeight * 4;
    const BufferHandle readback = device.CreateReadbackBuffer(bytes);
    PipelineDesc pipeline{};
    pipeline.type = PipelineType::Graphics;
    pipeline.vertexShader = program.stageBytes.at("vs");
    pipeline.pixelShader = program.stageBytes.at("fs");
    pipeline.colorAttachmentFormats = {texture.format};
    pipeline.rasterizationSamples = alphaToCoverage ? 4u : 1u;
    pipeline.rasterizationState.cullMode = CullModeFlagBits::None;
    pipeline.depthStencilState.depthTestEnable = false;
    pipeline.depthStencilState.depthWriteEnable = false;
    pipeline.colorBlendState.alphaToCoverageEnable = alphaToCoverage;
    pipeline.debugName = "ForwardAdapterAlphaProbe";
    const PipelineHandle handle = device.CreatePipeline(pipeline);
    EXPECT_TRUE(target.IsValid() && resolved.IsValid() && readback.IsValid() && handle.IsValid());
    std::vector<uint8_t> pixels;
    if (target.IsValid() && resolved.IsValid() && readback.IsValid() && handle.IsValid())
    {
        auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined,
                                                                 ResourceState::RenderTarget));
        if (alphaToCoverage)
            commands->Barrier(ResourceBarrier::CreateTextureBarrier(resolved, ResourceState::Undefined,
                                                                     ResourceState::RenderTarget));
        RenderPassDesc pass{};
        pass.colorTargetCount = 1;
        pass.colorTargets[0] = target;
        if (alphaToCoverage)
            pass.resolveColorTargets[0] = resolved;
        pass.clearColor[0] = true;
        std::fill_n(pass.clearColorValue[0], 4, 0.0f);
        pass.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        commands->BeginRenderPass(pass);
        commands->SetViewport(0, 0, float(kProbeWidth), float(kProbeHeight));
        commands->SetScissor(0, 0, kProbeWidth, kProbeHeight);
        commands->SetPipeline(handle);
        commands->Draw(3);
        commands->EndRenderPass();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(resolved, ResourceState::RenderTarget,
                                                                 ResourceState::CopySource));
        commands->CopyTextureToBuffer(resolved, readback, kProbeWidth, kProbeHeight);
        commands->End();
        device.ExecuteCommandLists({commands.get()});
        device.WaitForIdle();
        const auto* mapped = static_cast<const uint8_t*>(device.MapBuffer(readback));
        EXPECT_NE(mapped, nullptr);
        if (mapped)
        {
            pixels.assign(mapped, mapped + bytes);
            device.UnmapBuffer(readback);
        }
    }
    if (handle.IsValid()) device.DestroyPipeline(handle);
    if (readback.IsValid()) device.DestroyBuffer(readback);
    if (alphaToCoverage && resolved.IsValid()) device.DestroyTexture(resolved);
    if (target.IsValid()) device.DestroyTexture(target);
    return pixels;
}
} // namespace

// Execute the adapter's actual alpha block on the GPU. The four stripes supply opacity
// 0, 1/3, 2/3 and 1: ordinary opaque output must cover all of them, Blend must retain
// their alpha, and an explicitly declared coverage surface must keep partial MSAA samples.
// The block is read from the staged adapter, never copied into this oracle.
TEST(ForwardAdapterAlphaReadback, OpaqueBlendAndExplicitCoverageKeepTheirDistinctContracts)
{
    const fs::path adapter = GameEngine::Rendering::Tests::GetAdapterShaderDir() /
                             "Adapters/adapter_forward.glsl";
    std::ifstream input(adapter, std::ios::binary);
    ASSERT_TRUE(input.good()) << adapter.string();
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const size_t begin = source.find("    float alpha = outputAlpha;");
    ASSERT_NE(begin, std::string::npos);
    const size_t end = source.find("#ifdef GE_LOD_CROSSFADE", begin);
    ASSERT_NE(end, std::string::npos);
    const std::string alphaBlock = source.substr(begin, end - begin);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = false;
    desc.enableDynamicRendering = true;
    desc.enableDebugLayer = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->GetValidationStats().Enabled)
    {
        device->Shutdown();
        GTEST_SKIP() << "Vulkan validation layer unavailable";
    }
    const fs::path cache = MakeTempCacheRoot();
    for (int mode = 0; mode < 3; ++mode)
    {
        SCOPED_TRACE(mode == 0 ? "opaque" : mode == 1 ? "blend" : "coverage");
        ShaderProgramCompileRequest request{};
        request.debugName = "ForwardAdapterAlphaProbe";
        request.cacheRoot = cache;
        request.baseDirectory = adapter.parent_path();
        ShaderStageCompileSpec vertex{};
        vertex.stage = "vs";
        vertex.sourcePath = "alpha-probe.vert";
        vertex.inlineSource = R"(#version 450
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1);
}
)";
        ShaderStageCompileSpec fragment{};
        fragment.stage = "fs";
        fragment.sourcePath = "alpha-probe.frag";
        if (mode == 1) fragment.defines = {"GE_ALPHA_BLEND"};
        if (mode == 2) fragment.defines = {"GE_SURFACE_ALPHA_TO_COVERAGE"};
        fragment.inlineSource = "#version 450\nlayout(location=0) out vec4 color;\nvoid main() {\n"
                                "float outputAlpha = float(uint(gl_FragCoord.x) / 16u) / 3.0;\n" +
                                alphaBlock + "\ncolor = vec4(1, 1, 1, alpha);\n}\n";
        request.stages = {vertex, fragment};
        ShaderProgramCompileResult program{};
        std::string error;
        const bool compiled = ShaderCompileService::CompileProgramToCache(
            request, ShaderSourceKind::SpirV, program, &error);
        EXPECT_TRUE(compiled) << error;
        if (!compiled) continue;
        const auto pixels = DrawAdapterProbe(*device, program, mode == 2);
        EXPECT_EQ(pixels.size(), size_t(kProbeWidth) * kProbeHeight * 4);
        if (pixels.empty()) continue;
        size_t partial = 0;
        for (uint32_t y = 0; y < kProbeHeight; ++y)
            for (uint32_t x = 0; x < kProbeWidth; ++x)
            {
                const size_t at = (size_t(y) * kProbeWidth + x) * 4;
                const uint32_t stripe = x / 16;
                if (mode == 2)
                {
                    if (stripe == 0) EXPECT_EQ(pixels[at], 0);
                    else if (stripe == 3) EXPECT_EQ(pixels[at], 255);
                    else partial += pixels[at] > 0 && pixels[at] < 255;
                }
                else
                {
                    EXPECT_EQ(pixels[at], 255);
                    EXPECT_NEAR(pixels[at + 3], mode == 0 ? 255 : stripe * 85, 1);
                }
            }
        if (mode == 2)
            EXPECT_GT(partial, size_t(kProbeWidth) * kProbeHeight / 4)
                << "the coverage material lost its fractional sample masks";
    }
    EXPECT_EQ(device->GetValidationStats().ErrorCount, 0u);
    device->Shutdown();
    std::error_code ec;
    fs::remove_all(cache, ec);
}

// Execute the adapter's actual emission block on the GPU at two view exposures, as the tonemap then
// exposes it: the four stripes carry exposure weights 0, 0.5, 1 and 1, read where a surface's
// GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT reads its material lane. Weight 1 brightens with the
// exposure (physical), weight 0 reads the same at both, 0.5 follows the square root of the exposure
// (log interpolation). The block and the weight's scale are read from the staged shaders, never
// copied into this oracle; the view's exposure is the probe's constant.
TEST(ForwardAdapterEmissionReadback, ExposureWeightSetsHowFarTheEmissionFollowsTheExposure)
{
    const fs::path shaders = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    const auto readSource = [](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.good()) << path.string();
        return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    };
    const std::string adapter = readSource(shaders / "Adapters/adapter_forward.glsl");
    const size_t blockBegin = adapter.find("    // Emissive contribution");
    ASSERT_NE(blockBegin, std::string::npos);
    const size_t blockEnd = adapter.find("    // Alpha handling.", blockBegin);
    ASSERT_NE(blockEnd, std::string::npos);
    const std::string emissionBlock = adapter.substr(blockBegin, blockEnd - blockBegin);
    const std::string exposure = readSource(shaders / "Includes/view_exposure.glsl");
    const size_t scaleBegin = exposure.find("float GE_EmissiveExposureScale(");
    ASSERT_NE(scaleBegin, std::string::npos);
    const size_t scaleEnd = exposure.find("\n}\n", scaleBegin);
    ASSERT_NE(scaleEnd, std::string::npos);
    const std::string scaleFunction = exposure.substr(scaleBegin, scaleEnd + 3 - scaleBegin);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = false;
    desc.enableDynamicRendering = true;
    desc.enableDebugLayer = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->GetValidationStats().Enabled)
    {
        device->Shutdown();
        GTEST_SKIP() << "Vulkan validation layer unavailable";
    }
    constexpr float kEmission = 0.25f;
    constexpr float kStripeWeights[4] = {0.0f, 0.5f, 1.0f, 1.0f};
    const fs::path cache = MakeTempCacheRoot();
    std::vector<uint8_t> exposed[2];
    for (int view = 0; view < 2; ++view)
    {
        const float viewExposure = view == 0 ? 0.5f : 2.0f;
        SCOPED_TRACE(viewExposure);
        ShaderProgramCompileRequest request{};
        request.debugName = "ForwardAdapterEmissionProbe";
        request.cacheRoot = cache;
        request.baseDirectory = shaders / "Adapters";
        ShaderStageCompileSpec vertex{};
        vertex.stage = "vs";
        vertex.sourcePath = "emission-probe.vert";
        vertex.inlineSource = R"(#version 450
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1);
}
)";
        ShaderStageCompileSpec fragment{};
        fragment.stage = "fs";
        fragment.sourcePath = "emission-probe.frag";
        fragment.inlineSource =
            "#version 450\nlayout(location=0) out vec4 color;\n"
            "float GE_ViewExposureScale() { return " + std::to_string(viewExposure) + "; }\n" + scaleFunction +
            "#define GE_EMISSION_FOLLOWS_EXPOSURE_WEIGHT\n"
            "#define GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT weights[uint(gl_FragCoord.x) / 16u]\n"
            "struct Probe { vec3 emissive; };\nvoid main() {\n"
            "const float weights[4] = float[4](" + std::to_string(kStripeWeights[0]) + ", " +
            std::to_string(kStripeWeights[1]) + ", " + std::to_string(kStripeWeights[2]) + ", " +
            std::to_string(kStripeWeights[3]) + ");\n"
            "Probe so = Probe(vec3(" + std::to_string(kEmission) + "));\n"
            "vec3 finalColor = vec3(0.0);\nfloat emissiveMultiplier = 1.0;\n" + emissionBlock +
            "color = vec4(finalColor * GE_ViewExposureScale(), 1.0);\n}\n";
        request.stages = {vertex, fragment};
        ShaderProgramCompileResult program{};
        std::string error;
        const bool compiled = ShaderCompileService::CompileProgramToCache(
            request, ShaderSourceKind::SpirV, program, &error);
        ASSERT_TRUE(compiled) << error;
        exposed[view] = DrawAdapterProbe(*device, program, false);
        ASSERT_EQ(exposed[view].size(), size_t(kProbeWidth) * kProbeHeight * 4);
    }
    for (uint32_t stripe = 0; stripe < 4; ++stripe)
    {
        SCOPED_TRACE(stripe);
        const float weight = kStripeWeights[stripe];
        const size_t at = (size_t(kProbeHeight / 2) * kProbeWidth + stripe * 16 + 8) * 4;
        // The tonemap's input: emission x exposure^weight, in 8-bit levels.
        EXPECT_NEAR(exposed[0][at], 255.0f * kEmission * std::pow(0.5f, weight), 1.0f);
        EXPECT_NEAR(exposed[1][at], 255.0f * kEmission * std::pow(2.0f, weight), 1.0f);
    }
    EXPECT_EQ(device->GetValidationStats().ErrorCount, 0u);
    device->Shutdown();
    std::error_code ec;
    fs::remove_all(cache, ec);
}
