// Integration tests for Material registration and end-to-end
// model material conversion -> registration -> draw pipeline.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Assets/ModelAsset.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineCache.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Engine/Rendering/ShaderCompilationCache.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

#include "EngineLogCapture.h"
#include "TestDeviceHelper.h"
#include "StagedTestPaths.h"

class MaterialBridgeIntegrationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        m_rs = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_rs->Initialize(m_device.get()));
    }

    void TearDown() override
    {
        if (m_rs)
            m_rs->Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    std::unique_ptr<IDevice> m_device;
    std::unique_ptr<RenderServices> m_rs;
};

// A document naming a vertex modifier must leave the key carrying exactly one modifier keyword,
// INCLUDING in this harness, which builds RenderServices without an EngineCore and so never warms
// the material build context. The derivation runs for that context's populating side effect rather
// than behind it: gated on readiness, a cold context yields NEITHER bit, and MaterialDepthClassify
// / DepthDrawRecorder / RenderExtractionSystem all branch on
// HasVertexMod || HasVertexOutputMod — a modifier material would batch as unmodified geometry.
TEST_F(MaterialBridgeIntegrationTest, VertexModifierDocumentAlwaysCarriesOneModifierKeyword)
{
    MaterialDocument doc{};
    doc.materialName = "ColdContextModifier";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";

    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);

    const MaterialKeyword kw = mat->GetVariantKey().materialKeywords;
    const bool simple = HasKeyword(kw, MaterialKeyword::HasVertexMod);
    const bool extended = HasKeyword(kw, MaterialKeyword::HasVertexOutputMod);
    EXPECT_TRUE(simple || extended) << "a modifier document produced a key with neither form";
    EXPECT_FALSE(simple && extended) << "both modifier defines would co-emit into one variant";
}

namespace
{
// Stands in for the engine's worker dispatcher: keeps every pipeline build the
// device queues until the test runs it, so the window in which a build is
// pending is observable. Removes itself from the device before its jobs go.
class HeldPipelineBuilds
{
  public:
    explicit HeldPipelineBuilds(IDevice& device) : m_Device(device)
    {
        m_Device.SetPipelineBuildDispatcher([this](std::function<void()> job) { Jobs.push_back(std::move(job)); });
    }
    ~HeldPipelineBuilds() { m_Device.SetPipelineBuildDispatcher({}); }
    HeldPipelineBuilds(const HeldPipelineBuilds&) = delete;
    HeldPipelineBuilds& operator=(const HeldPipelineBuilds&) = delete;

    void RunAll()
    {
        std::vector<std::function<void()>> jobs = std::move(Jobs);
        Jobs.clear();
        for (std::function<void()>& job : jobs)
            job();
    }

    std::vector<std::function<void()>> Jobs;

  private:
    IDevice& m_Device;
};
} // namespace

// ---- Real shader compilation integration tests ----
// These tests inject real shader paths via SetMaterialBuildContext so
// CompileMaterialPipeline compiles actual GLSL through shaderc.


class MaterialPipelineCompilationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_shaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
        if (!std::filesystem::exists(m_shaderDir))
            GTEST_SKIP() << "Staged shader directory not found: " << m_shaderDir.string();

        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        m_rs = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_rs->Initialize(m_device.get()));

        // Inject real shader paths so CompileMaterialPipeline uses actual GLSL.
        MaterialBuildContext ctx{};
        ctx.AdapterShaderDir = m_shaderDir;
        ctx.CacheRoot = std::filesystem::temp_directory_path() / "ge_test_shader_cache";
        ctx.IncludeDirs = {m_shaderDir};
        m_rs->Materials().SetMaterialBuildContext(ctx);
    }

    void TearDown() override
    {
        if (m_rs)
            m_rs->Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    std::filesystem::path m_shaderDir;
    std::unique_ptr<IDevice> m_device;
    std::unique_ptr<RenderServices> m_rs;
};

// A tag-block shader-graph surface flows through the same compile pipeline as a
// hand-authored one: PrepareMaterialDocumentForShaderPackage materializes the
// graph under CacheRoot/Generated (derived data, never beside the .material) and
// repoints surfaceShader at it; BuildMaterialToShaderPackage then compiles the
// materialized EvaluateSurface to SPIR-V like any other surface.
TEST_F(MaterialPipelineCompilationTest, ShaderGraphSurface_MaterializesUnderCacheRootAndCompiles)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_sg_materialize_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    // Minimal @sg-* tag-block graph. The tag block is authoritative; the body is
    // regenerated from the tags when the surface is materialized. The authored
    // body carries no #include lines, so it does not read as already-materialized.
    {
        std::ofstream out(projectRoot / "mini_graph.glsl", std::ios::binary);
        out << "// Auto-generated. The @sg-* tag block is authoritative.\n"
               "//\n"
               "// @sg-graph     MiniGraph\n"
               "// @sg-version   1\n"
               "// @sg-stage     surface\n"
               "// @sg-lighting  StandardPBR\n"
               "//\n"
               "// @sg-node      node_color   type=ColorConstant  pos=(120,140)  r=0.5 g=0.25 b=0.125\n"
               "// @sg-node      node_output  type=SurfaceOutput  pos=(600,140)\n"
               "//\n"
               "// @sg-edge      node_color.value -> node_output.BaseColor\n"
               "\n"
               "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
               "{\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    return o;\n"
               "}\n";
    }

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    ctx.AssetSourceRoots = {{"project", projectRoot}};

    MaterialDocument doc{};
    doc.materialName = "GraphMini";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "mini_graph.glsl"; // sibling of the .material
    ASSERT_EQ(doc.schemaVersion, 3);

    const fs::path materialPath = projectRoot / "mini_graph.material";
    std::vector<std::string> errors;
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(doc, materialPath, ctx, errors))
        << (errors.empty() ? std::string("(no errors reported)") : errors.front());

    // The materialized surface: absolute path, under CacheRoot/Generated, carrying
    // the regenerated EvaluateSurface body and the authoritative tag block.
    const fs::path materialized(doc.surfaceShader);
    EXPECT_TRUE(materialized.is_absolute());
    ASSERT_TRUE(fs::exists(materialized)) << materialized.generic_string();
    EXPECT_EQ(materialized.parent_path(), ctx.CacheRoot / "Generated")
        << "generated surface must land under CacheRoot/Generated";

    std::ifstream in(materialized, std::ios::binary);
    const std::string materializedText((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    EXPECT_NE(materializedText.find("SurfaceOutput EvaluateSurface"), std::string::npos);
    EXPECT_NE(materializedText.find("@sg-graph"), std::string::npos);

    const auto result =
        BuildMaterialToShaderPackage(doc, materialPath, "GraphMini", ctx, ShaderSourceKind::SpirV, MaterialKeyword::Instanced);
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    ASSERT_TRUE(result.package->stageBytes.count("vs"));
    ASSERT_TRUE(result.package->stageBytes.count("fs"));
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    const auto& fsBytes = result.package->stageBytes.at("fs");
    ASSERT_FALSE(fsBytes.empty());

    // shaderc runs at optimization level zero, so OpName debug names survive: the
    // fragment SPIR-V still carries the graph's EvaluateSurface function name.
    const std::string needle = "EvaluateSurface";
    EXPECT_NE(std::search(fsBytes.begin(), fsBytes.end(), needle.begin(), needle.end()),
              fsBytes.end())
        << "fragment SPIR-V should carry the EvaluateSurface name (OpName debug info)";

    // Reflected meta covers both compiled stages.
    EXPECT_TRUE(result.package->meta.Stages.count("vs"));
    EXPECT_TRUE(result.package->meta.Stages.count("fs"));

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// ---- Converted Unity shader graphs.
//
// The Unity importer emits SG v2 tag blocks in a specific shape: parameter nodes
// carrying slot/swizzle for semantic material fields, textures as `tex` pin
// defaults naming canonical slots, vec4 colour math lowered into a vec3 node plus
// a scalar alpha node, and vector pin defaults written as GLSL constructors. This
// fixture is that shape, authored here rather than copied from a vendor asset
// (real .shadergraph files are licensed content). If the emitter and the engine
// ever disagree about the grammar, this fails before anyone opens the editor.

namespace
{
const char* kConvertedGraphSource = R"(// Converted from a Unity .shadergraph by the engine's Unity importer.
// The @sg-* tag block is authoritative; the body is regenerated on save.
//
// @sg-graph     ConvertedBasic
// @sg-version   1
// @sg-stage     surface
// @sg-lighting  StandardPBR
// @sg-property  uBaseColor vec4 default=1, 1, 1, 1 public=true
// @sg-texture   albedoMap "" hint=srgb
// @sg-texture   emissiveMap "" hint=srgb
// @sg-texture   normalMap "" hint=linear
//
// @sg-node      n_albedo   type=SampleTexture2D pos=(-1569,-152) tex=albedoMap
// @sg-node      n_basergb  type=Vec3Parameter   pos=(-1377,-34) slot=0 swizzle=xyz variableName=uBaseColor
// @sg-node      n_basea    type=FloatParameter  pos=(-1377,106) slot=0 component=w variableName=uBaseColor
// @sg-node      n_tint     type=VectorMultiply  pos=(-1230,-152)
// @sg-node      n_tinta    type=Multiply        pos=(-1230,-12)
// @sg-node      n_emis     type=SampleTexture2D pos=(-1550,324) tex=emissiveMap
// @sg-node      n_emiscol  type=ColorConstant   pos=(-1510,606) r=0.5 g=0.25 b=0.125
// @sg-node      n_emismul  type=VectorMultiply  pos=(-1308,473)
// @sg-node      n_enable   type=FloatConstant   pos=(-1340,360) value=1
// @sg-node      n_branch   type=ColorMix        pos=(-1139,325) a=vec3(0.0)
// @sg-node      n_rough    type=FloatParameter  pos=(-169,402) slot=1 component=y variableName=roughness
// @sg-node      n_metal    type=FloatParameter  pos=(-145,525) slot=1 component=x variableName=metallic
// @sg-node      n_nrm      type=SampleTexture2D pos=(-1520,824) tex=normalMap
// @sg-node      n_nrmamt   type=FloatConstant   pos=(-1313,898) value=0.75
// @sg-node      n_nrmstr   type=NormalStrength  pos=(-1139,824)
// @sg-node      surface_out type=SurfaceOutput  pos=(640,0)
// @sg-edge      n_albedo.rgb -> n_tint.a
// @sg-edge      n_basergb.value -> n_tint.b
// @sg-edge      n_tint.result -> surface_out.BaseColor
// @sg-edge      n_albedo.a -> n_tinta.a
// @sg-edge      n_basea.value -> n_tinta.b
// @sg-edge      n_tinta.result -> surface_out.Opacity
// @sg-edge      n_emis.rgb -> n_emismul.a
// @sg-edge      n_emiscol.value -> n_emismul.b
// @sg-edge      n_emismul.result -> n_branch.b
// @sg-edge      n_enable.value -> n_branch.weight
// @sg-edge      n_branch.result -> surface_out.Emissive
// @sg-edge      n_rough.value -> surface_out.Roughness
// @sg-edge      n_metal.value -> surface_out.Metallic
// @sg-edge      n_nrm.rgb -> n_nrmstr.value
// @sg-edge      n_nrmamt.value -> n_nrmstr.strength
// @sg-edge      n_nrmstr.result -> surface_out.Normal
)";
} // namespace

TEST_F(MaterialPipelineCompilationTest, ConvertedUnityGraphShape_MaterializesAndCompilesToSpirV)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_sg_converted_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);
    {
        std::ofstream out(projectRoot / "converted_graph.glsl", std::ios::binary);
        out << kConvertedGraphSource;
    }

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    ctx.AssetSourceRoots = {{"project", projectRoot}};

    MaterialDocument doc{};
    doc.materialName = "ConvertedBasic";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "converted_graph.glsl";

    const fs::path materialPath = projectRoot / "converted_graph.material";
    std::vector<std::string> errors;
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(doc, materialPath, ctx, errors))
        << (errors.empty() ? std::string("(no errors reported)") : errors.front());

    const fs::path materialized(doc.surfaceShader);
    ASSERT_TRUE(fs::exists(materialized)) << materialized.generic_string();
    std::ifstream in(materialized, std::ios::binary);
    const std::string materializedText((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    std::cout << "[converted graph] materialized surface:\n" << materializedText << "\n";

    // Each sampler must keep its own slot: a texture landing on the wrong pin makes
    // the compiler fall back to albedoMap for every sample.
    EXPECT_NE(materializedText.find("emissiveMap"), std::string::npos) << materializedText;
    EXPECT_NE(materializedText.find("normalMap"), std::string::npos) << materializedText;
    // The lowered alpha lane and the tangent-space normal both have to survive.
    EXPECT_NE(materializedText.find("o.opacity"), std::string::npos) << materializedText;
    EXPECT_NE(materializedText.find("SG_NormalStrength"), std::string::npos) << materializedText;

    const auto result = BuildMaterialToShaderPackage(doc, materialPath, "ConvertedBasic", ctx, ShaderSourceKind::SpirV,
                                                     MaterialKeyword::Instanced);
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    ASSERT_TRUE(result.package->stageBytes.count("fs"));
    const auto& fsBytes = result.package->stageBytes.at("fs");
    ASSERT_FALSE(fsBytes.empty());
    const std::string needle = "EvaluateSurface";
    EXPECT_NE(std::search(fsBytes.begin(), fsBytes.end(), needle.begin(), needle.end()),
              fsBytes.end());

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// Unlit material compiles a different shader path successfully.
TEST_F(MaterialPipelineCompilationTest, Unlit_CompilesShaders)
{
    MaterialDocument doc{};
    doc.materialName = "RealUnlit";
    doc.lightingModel = "Unlit";
    doc.surfaceShader = "Surfaces/unlit_solid.glsl";
    doc.properties["baseColor"] = std::vector<float>{0.0f, 1.0f, 0.0f, 1.0f};

    GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);

    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const auto* gd = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(gd, nullptr);
    EXPECT_TRUE(gd->VertexShader && !gd->VertexShader->empty());
    EXPECT_TRUE(gd->PixelShader && !gd->PixelShader->empty());
    EXPECT_NE(mat->GetShaderMeta(), nullptr);
}

// Blend-mode material compiles and gets correct alpha state.
TEST_F(MaterialPipelineCompilationTest, BlendMode_CompilesShaders)
{
    MaterialDocument doc{};
    doc.materialName = "RealBlend";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.5f};

    GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);

    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const auto* gd = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(gd, nullptr);
    EXPECT_TRUE(gd->VertexShader && !gd->VertexShader->empty());
    EXPECT_TRUE(gd->PixelShader && !gd->PixelShader->empty());
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Blend);
    EXPECT_FALSE(gd->DepthStencil.depthWriteEnable)
        << "Blend mode disables depth writes";
}

// Async base-shader registration (feat/async-material-registration).
//
// The model/scene-load path drives RegisterAndPrewarmMaterial, which requests
// BaseCompileMode::Async so a cold load's base-shader compiles run off the main
// thread; the material is skipped by the draw stream (dark) until its pipeline
// publishes on a later BeginFrame, then swaps in automatically.
//
// The real off-thread invalid->valid transition needs a live JobSystem, which
// this headless target cannot stand up (EngineCore::Initialize has a documented
// slow teardown here — see RenderPipelineDeclareTests). It is runtime-verified in
// the editor instead (the compile pill counts base compiles; frames keep pumping;
// the scene resolves once compiles publish). What this test locks down is the
// determinism seam that keeps the rest of the suite green: when the engine/job
// system is NOT up, CanSubmitAsyncBaseCompile() is false, so an Async request
// falls back to the inline synchronous compile — the material is never left dark
// in a headless/test context — and FlushAsyncMaterialCompiles() is a safe seam.
TEST_F(MaterialPipelineCompilationTest, AsyncRequest_FallsBackToSyncWhenEngineDown_AndFlushIsUsable)
{
    // Flush with nothing outstanding must be a safe no-op (no engine, no submits).
    m_rs->Materials().FlushAsyncMaterialCompiles();

    MaterialDocument doc{};
    doc.materialName = "AsyncFallbackPBR";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.properties["baseColor"] = std::vector<float>{0.25f, 0.5f, 0.75f, 1.0f};

    // RegisterAndPrewarmMaterial internally requests BaseCompileMode::Async. With
    // the engine down the request degrades to a synchronous compile, so the
    // material is immediately usable — never dark in a headless context.
    const GUID asyncGuid = GUID::Generate();
    Material* asyncMat = m_rs->RegisterAndPrewarmMaterial(asyncGuid, doc);
    ASSERT_NE(asyncMat, nullptr);
    ASSERT_TRUE(asyncMat->GetGraphicsPipelineId().IsValid())
        << "Async request must fall back to a synchronous compile when the job "
           "system is unavailable, so headless/test materials are never left dark";

    // Draining after the fact must not disturb an already-published pipeline.
    const auto beforeFlush = asyncMat->GetGraphicsPipelineId();
    m_rs->Materials().FlushAsyncMaterialCompiles();
    EXPECT_EQ(asyncMat->GetGraphicsPipelineId().Value, beforeFlush.Value)
        << "Flush must leave an already-valid pipeline untouched";

    // Refactor equivalence: the explicit Synchronous path (its InternBaseMaterial-
    // Pipeline assembly was extracted for reuse by the async worker) must still
    // produce an equivalently-usable pipeline — non-empty SPIR-V + shader meta.
    MaterialDocument syncDoc = doc;
    syncDoc.materialName = "SyncPBR";
    const GUID syncGuid = GUID::Generate();
    Material* syncMat = m_rs->Materials().RegisterMaterialFromDocument(
        syncGuid, syncDoc, MaterialKeyword::None,
        MaterialSystem::BaseCompileMode::Synchronous);
    ASSERT_NE(syncMat, nullptr);
    ASSERT_TRUE(syncMat->GetGraphicsPipelineId().IsValid());

    const auto* asyncGd = m_device->LookupGraphicsPipeline(asyncMat->GetGraphicsPipelineId());
    const auto* syncGd = m_device->LookupGraphicsPipeline(syncMat->GetGraphicsPipelineId());
    ASSERT_NE(asyncGd, nullptr);
    ASSERT_NE(syncGd, nullptr);
    EXPECT_TRUE(asyncGd->VertexShader && !asyncGd->VertexShader->empty());
    EXPECT_TRUE(asyncGd->PixelShader && !asyncGd->PixelShader->empty());
    EXPECT_TRUE(syncGd->VertexShader && !syncGd->VertexShader->empty());
    EXPECT_TRUE(syncGd->PixelShader && !syncGd->PixelShader->empty());
    EXPECT_NE(asyncMat->GetShaderMeta(), nullptr);
    EXPECT_NE(syncMat->GetShaderMeta(), nullptr);
}

// Publish-drain guard direction: a stale async base-pipeline publish must never
// overwrite a pipeline that a synchronous (re)compile already made valid while the
// async compile was in flight. Drives the drain via the test seam (no job system).
TEST_F(MaterialPipelineCompilationTest, AsyncPublish_DoesNotClobberValidPipeline)
{
    MaterialDocument doc{};
    doc.materialName = "GuardDir";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc); // sync => valid pid
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const auto realPid = mat->GetGraphicsPipelineId();

    ShaderCacheKey key{};
    key.VariantKey = mat->GetVariantKey();
    key.Source.SurfaceShaderPath = doc.surfaceShader;
    key.Source.MaterialAssetPath = mat->GetMaterialAssetPath();
    // A different, bogus-but-valid pipeline id: the valid-pipeline guard must skip it.
    m_rs->Materials().EnqueueBasePipelinePublishForTesting(
        guid, Rendering::GraphicsPipelineId{realPid.Value + 100000u}, nullptr, key);
    m_rs->Materials().FlushAsyncMaterialCompiles();

    EXPECT_EQ(mat->GetGraphicsPipelineId().Value, realPid.Value)
        << "a stale async publish must not overwrite an already-valid pipeline";
}

// F1: a base compile that finishes after the material was re-registered with
// different pipeline-affecting state must be discarded (its ShaderCacheKey no
// longer matches the material's live key) and re-driven for the current key —
// never published as the wrong shader.
TEST_F(MaterialPipelineCompilationTest, AsyncPublish_StaleKeyDiscardedAndRedriven)
{
    MaterialDocument doc{};
    doc.materialName = "StaleKey";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    // Reproduce the in-flight window: force the material back to "no pipeline yet"
    // so the drain reaches the key comparison (the valid-pipeline guard would
    // otherwise short-circuit before it).
    Material::TestFactory::SetGraphicsPipelineId(*mat, {});
    ASSERT_FALSE(mat->GetGraphicsPipelineId().IsValid());

    // Enqueue a STALE publish: a bogus-but-valid pid compiled against a DIFFERENT
    // surface, as if the document changed after this compile started. The live key
    // still resolves to standard_pbr, so the keys mismatch.
    ShaderCacheKey staleKey{};
    staleKey.VariantKey = mat->GetVariantKey();
    staleKey.Source.SurfaceShaderPath = "Surfaces/unlit_solid.glsl"; // != live standard_pbr
    staleKey.Source.MaterialAssetPath = mat->GetMaterialAssetPath();
    const Rendering::GraphicsPipelineId bogus{777777u};
    m_rs->Materials().EnqueueBasePipelinePublishForTesting(guid, bogus, nullptr, staleKey);

    m_rs->Materials().FlushAsyncMaterialCompiles();

    // The stale pid must be discarded; the re-drive (synchronous fallback headless)
    // compiled the material's live surface into a real, different pipeline.
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid())
        << "re-drive must compile the live key into a valid pipeline";
    EXPECT_NE(mat->GetGraphicsPipelineId().Value, bogus.Value)
        << "the stale-key pipeline must be discarded, not published";
    EXPECT_NE(mat->GetShaderMeta(), nullptr);
}

// The failed-publish restore must not CLOBBER either: when a synchronous
// (re)compile published a fresh pipeline while the failed async compile was in
// flight, its stale last-good stash must not overwrite the newer valid id —
// the restore applies only while the material still has no pipeline.
TEST_F(MaterialPipelineCompilationTest, AsyncPublish_FailedCompileDoesNotClobberValidPipeline)
{
    MaterialDocument doc{};
    doc.materialName = "FailedNoClobber";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc); // sync => valid pid
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const auto freshPid = mat->GetGraphicsPipelineId();

    ShaderCacheKey liveKey{};
    liveKey.VariantKey = mat->GetVariantKey();
    liveKey.Source.SurfaceShaderPath = mat->GetCompileSpec().surfaceShaderPath;
    liveKey.Source.VertexModifierPath = mat->GetCompileSpec().vertexModifierPath;
    liveKey.Source.MaterialAssetPath = mat->GetMaterialAssetPath();
    // A failed publish (invalid result id) carrying a STALE last-good — an id
    // retired by an older submit, superseded by the pipeline published above.
    const Rendering::GraphicsPipelineId staleLastGood{freshPid.Value + 100000u};
    m_rs->Materials().EnqueueBasePipelinePublishForTesting(
        guid, Rendering::GraphicsPipelineId{}, nullptr, liveKey, staleLastGood);

    m_rs->Materials().FlushAsyncMaterialCompiles();

    EXPECT_EQ(mat->GetGraphicsPipelineId().Value, freshPid.Value)
        << "a failed publish's stale last-good must not clobber a pipeline "
           "something newer already published";
}

namespace
{
// A throwaway project root holding one declared surface, "declared.glsl",
// installed as the material build context's project root so a document naming
// it resolves there. Removed with the object.
struct DeclaredSurfaceProject
{
    DeclaredSurfaceProject(MaterialSystem& materials, const std::filesystem::path& shaderDir)
        : Root(std::filesystem::temp_directory_path() /
               ("ge_declared_publish_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed())))
    {
        std::filesystem::create_directories(Root);
        MaterialBuildContext ctx{};
        ctx.AdapterShaderDir = shaderDir;
        ctx.CacheRoot = std::filesystem::temp_directory_path() / "ge_test_shader_cache";
        ctx.IncludeDirs = {shaderDir};
        ctx.ProjectRoots = {Root};
        materials.SetMaterialBuildContext(ctx);
    }
    ~DeclaredSurfaceProject()
    {
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }
    void WriteSurface(const char* declarations) const
    {
        std::ofstream out(Root / "declared.glsl", std::ios::binary | std::ios::trunc);
        out << declarations
            << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    return o;\n"
               "}\n";
    }
    std::filesystem::path Root;
};

ShaderCacheKey LiveKeyOf(const Material& mat)
{
    ShaderCacheKey key{};
    key.VariantKey = mat.GetVariantKey();
    key.Source.SurfaceShaderPath = mat.GetCompileSpec().surfaceShaderPath;
    key.Source.VertexModifierPath = mat.GetCompileSpec().vertexModifierPath;
    key.Source.MaterialAssetPath = mat.GetMaterialAssetPath();
    return key;
}
} // namespace

// The declared-property table is bound WITH the pipeline: a successful async
// publish re-lays the CPU cache from the program it compiled, so a `// @property`
// edit that adds, removes or reorders lanes reaches the CPU side in the frame the
// new shader binds — never at submit, while the old shader still renders.
TEST_F(MaterialPipelineCompilationTest, AsyncPublish_SuccessAppliesTheDeclaredTable)
{
    DeclaredSurfaceProject project(m_rs->Materials(), m_shaderDir);
    project.WriteSurface("// @property float density default=1\n");

    MaterialDocument doc{};
    doc.materialName = "DeclaredPublish";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "declared.glsl";
    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc); // sync => valid pid
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    ASSERT_NE(mat->GetDeclaredProperties(), nullptr);
    ASSERT_NE(mat->GetDeclaredProperties()->Find("density"), nullptr);
    ASSERT_EQ(mat->GetDeclaredProperties()->Find("extra"), nullptr);

    // The edit adds a lane (a longer file, so the stamp-keyed cache re-parses);
    // the submit retired the pipeline while the worker compiles it.
    project.WriteSurface("// @property float density default=1\n"
                         "// @property float extra default=2\n");
    Material::TestFactory::SetGraphicsPipelineId(*mat, {});

    const Rendering::GraphicsPipelineId published{555555u};
    m_rs->Materials().EnqueueBasePipelinePublishForTesting(guid, published, nullptr, LiveKeyOf(*mat));
    m_rs->Materials().FlushAsyncMaterialCompiles();

    ASSERT_EQ(mat->GetGraphicsPipelineId().Value, published.Value) << "the publish must have landed";
    ASSERT_NE(mat->GetDeclaredProperties(), nullptr);
    EXPECT_NE(mat->GetDeclaredProperties()->Find("extra"), nullptr)
        << "a successful publish must re-lay the declared table from the program it compiled";
}

// A FAILED publish restores the last-good pipeline and leaves its declared table
// alone: re-laying the lanes for a shader that never bound would put the CPU
// cache out of step with the shader still rendering.
TEST_F(MaterialPipelineCompilationTest, AsyncPublish_FailedCompileKeepsTheDeclaredTable)
{
    DeclaredSurfaceProject project(m_rs->Materials(), m_shaderDir);
    project.WriteSurface("// @property float density default=1\n");

    MaterialDocument doc{};
    doc.materialName = "DeclaredPublishFailed";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "declared.glsl";
    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc); // sync => valid pid
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const auto lastGood = mat->GetGraphicsPipelineId();
    const ShaderPropertyTable* table = mat->GetDeclaredProperties();
    ASSERT_NE(table, nullptr);
    ASSERT_NE(table->Find("density"), nullptr);

    // The edit adds a lane but the compile FAILS: an invalid result id carrying
    // the retired id as last-good.
    project.WriteSurface("// @property float density default=1\n"
                         "// @property float extra default=2\n");
    Material::TestFactory::SetGraphicsPipelineId(*mat, {});

    m_rs->Materials().EnqueueBasePipelinePublishForTesting(
        guid, Rendering::GraphicsPipelineId{}, nullptr, LiveKeyOf(*mat), lastGood);
    m_rs->Materials().FlushAsyncMaterialCompiles();

    EXPECT_EQ(mat->GetGraphicsPipelineId().Value, lastGood.Value)
        << "a failed publish must restore the last-good pipeline";
    EXPECT_EQ(mat->GetDeclaredProperties(), table)
        << "a failed publish must leave the last-good shader's declared table in place";
    ASSERT_NE(mat->GetDeclaredProperties(), nullptr);
    EXPECT_EQ(mat->GetDeclaredProperties()->Find("extra"), nullptr)
        << "the lane the failed program declared must not appear on the CPU side";
}

// T6, both halves against one material through the production paths: a
// recompile of an unchanged document moves the compile version and leaves the
// content revision alone; a parameter edit through the ordinary setter moves
// the content revision and leaves the compile version alone. The two counters
// answer different questions and a consumer must read the right one.
TEST_F(MaterialPipelineCompilationTest, RecompileMovesVersionOnlyAndParameterEditMovesRevisionOnly)
{
    MaterialDocument doc{};
    doc.materialName = "TwoCounters";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    const uint32_t versionBeforeRecompile = mat->GetVersion();
    const uint32_t revisionBeforeRecompile = mat->GetContentRevision();
    m_rs->Materials().RecompileMaterialPipeline(guid, doc);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid())
        << "the headless inline fallback must republish before the call returns";
    EXPECT_GT(mat->GetVersion(), versionBeforeRecompile)
        << "a recompile must bump the version so stale variant-cache rows retire";
    EXPECT_EQ(mat->GetContentRevision(), revisionBeforeRecompile)
        << "recompiling an unchanged document is not a content change";

    const uint32_t versionBeforeEdit = mat->GetVersion();
    const uint32_t revisionBeforeEdit = mat->GetContentRevision();
    mat->SetFloat("roughness"_sid, 0.125f);
    EXPECT_GT(mat->GetContentRevision(), revisionBeforeEdit);
    EXPECT_EQ(mat->GetVersion(), versionBeforeEdit)
        << "a parameter edit must not read as a recompile";
}

// User keywords: an in-place .material hot-reload that flips the keyword set must
// recompile the pipeline (route through CompileMaterialPipeline, which clears the
// per-material variant cache), while a no-op re-register must not.
TEST_F(MaterialPipelineCompilationTest, KeywordEditRecompilesPipeline)
{
    const GUID guid = GUID::Generate();
    MaterialDocument doc{};
    doc.materialName = "KwMat";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.keywords = {"FLOW_MODE"};

    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    const uint32_t versionA = mat->GetVersion();
    EXPECT_NE(mat->GetVariantKey().userKeywordHash, 0ull);

    // Hot-reload: same GUID, different keyword set -> variant key changes ->
    // pipelineAffectingChanged -> recompile (version bumps).
    doc.keywords = {"HIGH_DETAIL"};
    Material* mat2 = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    EXPECT_EQ(mat2, mat); // in-place reload keeps the Material*
    EXPECT_GT(mat->GetVersion(), versionA) << "keyword flip must recompile";

    // No-op re-register with the SAME keyword set -> no pipeline-affecting change.
    const uint32_t versionB = mat->GetVersion();
    m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    EXPECT_EQ(mat->GetVersion(), versionB) << "identical keywords must not recompile";
}

// An async recompile requested while a base compile is already in flight
// supersedes it: the in-flight result must be discarded at publish and re-driven
// against the current sources. The cache key is identical in this window (a
// content edit does not move it), so the F1 key guard cannot catch it — without
// the redrive mark the pre-edit result publishes as if current.
TEST_F(MaterialPipelineCompilationTest, RecompileAsync_SupersededInFlightResultRedriven)
{
    MaterialDocument doc{};
    doc.materialName = "SupersededRecompile";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    // Simulate the in-flight window: the seam marks the GUID in flight and queues
    // a result carrying the material's LIVE key (as a real worker would after a
    // content-only edit) with a bogus-but-valid pipeline id standing in for the
    // pre-edit compile.
    ShaderCacheKey liveKey{};
    liveKey.VariantKey = mat->GetVariantKey();
    liveKey.Source.SurfaceShaderPath = mat->GetCompileSpec().surfaceShaderPath;
    liveKey.Source.VertexModifierPath = mat->GetCompileSpec().vertexModifierPath;
    liveKey.Source.MaterialAssetPath = mat->GetMaterialAssetPath();
    const Rendering::GraphicsPipelineId bogus{888888u};
    m_rs->Materials().EnqueueBasePipelinePublishForTesting(guid, bogus, nullptr, liveKey);

    // The supersede: a recompile lands while that compile is "in flight". It
    // retires the pipeline and coalesces into a redrive mark instead of a
    // duplicate submit.
    m_rs->Materials().RecompileMaterialPipeline(guid, doc);
    ASSERT_FALSE(mat->GetGraphicsPipelineId().IsValid());

    m_rs->Materials().FlushAsyncMaterialCompiles();

    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid())
        << "the re-drive must compile the live sources into a valid pipeline";
    EXPECT_NE(mat->GetGraphicsPipelineId().Value, bogus.Value)
        << "the superseded in-flight result must be discarded, never published";
}

// RecompileMaterialPipeline must refresh the material's user texture-slot map
// in lockstep with the recompose (slice-K review F1): a surface edit can repack
// the GE_TEXSLOT_* ordinals, and a recompiled shader reading new ordinals
// against a stale routing map silently binds textures to the wrong physical
// slots. Also exercises project-root surface resolution end-to-end: the
// material has no asset path (headless registration), so both the slot
// resolution and the shaderc compile resolve the surface purely through
// MaterialBuildContext::ProjectRoots.
TEST_F(MaterialPipelineCompilationTest, RecompileRefreshesUserTextureSlotMap)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_recompile_slotmap_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    // v1: flowMask is the only user name -> packs to ordinal 2.
    {
        std::ofstream out(projectRoot / "slots_v1.glsl", std::ios::binary);
        out << "// @texture albedoMap srgb\n"
               "// @texture normalMap linear\n"
               "// @texture flowMask linear\n"
               "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = texture(GE_USER_TEXTURE(flowMask), sIn.uv0).rgb;\n"
               "    return o;\n"
               "}\n";
    }
    // v2: auxMask sorts before flowMask -> auxMask=2, flowMask=3.
    {
        std::ofstream out(projectRoot / "slots_v2.glsl", std::ios::binary);
        out << "// @texture albedoMap srgb\n"
               "// @texture normalMap linear\n"
               "// @texture auxMask linear\n"
               "// @texture flowMask linear\n"
               "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = texture(GE_USER_TEXTURE(flowMask), sIn.uv0).rgb\n"
               "                * texture(GE_USER_TEXTURE(auxMask), sIn.uv0).rgb;\n"
               "    return o;\n"
               "}\n";
    }

    // Re-inject the build context with the temp dir as the LIVE project root.
    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = std::filesystem::temp_directory_path() / "ge_test_shader_cache";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    MaterialDocument doc{};
    doc.materialName = "RecompileSlotMap";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "slots_v1.glsl";

    GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid())
        << "project-root-resolved surface must compile without a material asset path";

    // v1 routing: flowMask -> ordinal 2 (kMetalRough's physical slot).
    mat->SetBindlessTextureIndex("flowMask"_sid, 77u);
    EXPECT_EQ(mat->GetBindlessTextureIndex(TextureSlot::kMetalRough), 77u);

    // Recompose against v2: the packed ordinals move (auxMask=2, flowMask=3).
    MaterialDocument doc2 = doc;
    doc2.surfaceShader = "slots_v2.glsl";
    m_rs->Materials().RecompileMaterialPipeline(guid, doc2);

    mat->SetBindlessTextureIndex("flowMask"_sid, 88u);
    EXPECT_EQ(mat->GetBindlessTextureIndex(TextureSlot::kEmissive), 88u)
        << "recompile must refresh the name->slot routing map in lockstep with the recompose";
    mat->SetBindlessTextureIndex("auxMask"_sid, 99u);
    EXPECT_EQ(mat->GetBindlessTextureIndex(TextureSlot::kMetalRough), 99u);

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// ---- Parallax: a bound height map on a surface that declares one ----
//
// The Parallax keyword is derived from the document's binding AND the resolved surface's declared
// slots, so it is decided where registration resolves the surface. A refusal states the fix and
// reports once per material: runtime materials re-register every frame.

namespace
{
MaterialDocument MakeHeightMappedDocument(const char* name, const char* surfaceShader)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = surfaceShader;
    doc.textures["heightMap"] = "__embedded__:0"; // headless-safe live ref
    return doc;
}

bool CompilesParallax(const Material& mat)
{
    const std::vector<std::string> defines = GenerateDefines(mat.GetVariantKey(), "StandardPBR");
    return std::find(defines.begin(), defines.end(), "GE_PARALLAX_ENABLED") != defines.end();
}

// The runtime compiles at optimization level zero, so the march's function name survives into the
// fragment SPIR-V: its presence is what the GPU runs, where the key is only what was asked for.
bool FragmentMarches(const std::vector<uint8_t>& fragmentBytes)
{
    const std::string name = "GE_ParallaxMarch";
    return std::search(fragmentBytes.begin(), fragmentBytes.end(), name.begin(), name.end()) != fragmentBytes.end();
}

// The fragment SPIR-V of a registered material's base pipeline.
std::vector<uint8_t> BasePipelineFragment(IDevice& device, const Material& mat)
{
    const auto* desc = device.LookupGraphicsPipeline(mat.GetGraphicsPipelineId());
    if (!desc || !desc->PixelShader)
        return {};
    return *desc->PixelShader;
}

constexpr MaterialKeyword kWorldColourPass =
    MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL | MaterialKeyword::Instanced;

constexpr std::string_view kTriplanarRefusal =
    "Surfaces/triplanar_pbr does not use a Height map; bind it on a Standard PBR material";
constexpr std::string_view kHexTilingRefusal =
    "Parallax cannot combine with hex tiling: turn Hex Tiling off, or remove the Height map";
} // namespace

TEST_F(MaterialPipelineCompilationTest, HeightMapOnTheStandardSurfaceCompilesTheParallaxVariant)
{
    MaterialDocument doc = MakeHeightMappedDocument("Relief", "Surfaces/standard_pbr.glsl");
    doc.textureTransforms["heightMap"] = {3.0f, 0.0f, 0.25f, 0.0f, 0.0f, 3.0f, 0.5f, 0.0f};

    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_TRUE(CompilesParallax(*mat));
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid()) << "the parallax variant must compile";
    EXPECT_TRUE(FragmentMarches(BasePipelineFragment(*m_device, *mat)))
        << "the registered key carries Parallax, so the compiled base pipeline must march";

    // heightMap is the surface's own name on ordinal 6, and its tiling lands there with it.
    mat->SetBindlessTextureIndex("heightMap"_sid, 57u);
    EXPECT_EQ(mat->GetBindlessTextureIndex(static_cast<TextureSlot>(6)), 57u);
    const float* heightRows = mat->GetTextureTransforms() + 6 * 8;
    EXPECT_EQ(heightRows[0], 3.0f);
    EXPECT_EQ(heightRows[2], 0.25f);
    EXPECT_EQ(heightRows[5], 3.0f);
    EXPECT_EQ(heightRows[6], 0.5f);

    // Binding the map is the whole opt-in: the relief depth defaults to 0.02 of a repeat.
    EXPECT_FLOAT_EQ(mat->GetFloat("reliefDepth"_sid, -1.0f), 0.02f);

    MaterialDocument unbound = doc;
    unbound.textures["heightMap"] = "";
    Material* flat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), unbound);
    ASSERT_NE(flat, nullptr);
    EXPECT_FALSE(CompilesParallax(*flat)) << "an empty height slot compiles no march";
    EXPECT_FALSE(FragmentMarches(BasePipelineFragment(*m_device, *flat)));
}

// Every live compile after registration goes through ShaderCompilationCache, which rebuilds the
// document from the material's compile spec: the base pipeline, its asynchronous recompile, the
// prewarm and the draw-time pass variants. None of them sees the material's texture bindings, so
// the registered decision has to reach the compiled program through the spec.
TEST_F(MaterialPipelineCompilationTest, CacheDrivenCompilesOfAParallaxMaterialMarch)
{
    const MaterialDocument doc = MakeHeightMappedDocument("CacheRelief", "Surfaces/standard_pbr.glsl");
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(CompilesParallax(*mat)) << "the premise: registration derived the keyword";

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = std::filesystem::temp_directory_path() / ("ge_cache_parallax_" + GUID::Generate().ToString());
    ctx.IncludeDirs = {m_shaderDir};
    ShaderCompilationCache cache;

    // The shape of the base compile and the prewarm: the material's key and its spec.
    ShaderCacheKey key{};
    key.VariantKey = mat->GetVariantKey();
    key.VariantKey.materialKeywords |= kWorldColourPass;
    key.VariantKey.vertexFlags = VertexAttributeFlags::StandardMeshWithTangent;
    key.Source.SurfaceShaderPath = mat->GetCompileSpec().surfaceShaderPath;
    const auto compiled = cache.GetOrCompile(key, mat->GetCompileSpec(), ctx, "CacheRelief", ShaderSourceKind::SpirV);
    ASSERT_TRUE(compiled) << "compile failed";
    EXPECT_TRUE(FragmentMarches(compiled->fragmentBytes)) << "GetOrCompile compiled the flat program";

    // The shape of the draw-time colour variant.
    const auto variant = cache.GetOrCompileVariant(*mat, kWorldColourPass, ctx, ShaderSourceKind::SpirV,
                                                   VertexAttributeFlags::StandardMeshWithTangent);
    ASSERT_TRUE(variant) << "compile failed";
    EXPECT_TRUE(FragmentMarches(variant->fragmentBytes)) << "GetOrCompileVariant compiled the flat program";

    // A refused material (hex tiling on) keeps the flat program on the same paths.
    MaterialDocument hex = doc;
    hex.materialName = "CacheHexRelief";
    hex.properties["hexTiling"] = 1.0f;
    Material* refused = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), hex);
    ASSERT_NE(refused, nullptr);
    const auto refusedVariant = cache.GetOrCompileVariant(*refused, kWorldColourPass, ctx, ShaderSourceKind::SpirV,
                                                          VertexAttributeFlags::StandardMeshWithTangent);
    ASSERT_TRUE(refusedVariant) << "compile failed";
    EXPECT_FALSE(FragmentMarches(refusedVariant->fragmentBytes));

    std::error_code ec;
    std::filesystem::remove_all(ctx.CacheRoot, ec);
}

TEST_F(MaterialPipelineCompilationTest, HeightMapOnTriplanarIsRefusedNamingTheSurface)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
    Logger::Log::Warning("parallax refusal capture live");

    const GUID guid = GUID::Generate();
    const MaterialDocument doc = MakeHeightMappedDocument("TriplanarRelief", "Surfaces/triplanar_pbr.glsl");
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_FALSE(CompilesParallax(*mat));
    m_rs->Materials().RegisterMaterialFromDocument(guid, doc);

    Logger::Log::Flush();
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "parallax refusal capture live"), 1u) << "capture not live";
    EXPECT_EQ(TestLog::CountLinesContaining(lines, kTriplanarRefusal), 1u)
        << "the refusal names the surface, states the fix, and reports once";
    EXPECT_NE(TestLog::FirstLineContaining(lines, kTriplanarRefusal).find("TriplanarRelief"), std::string::npos)
        << "the report names the material";
}

TEST_F(MaterialPipelineCompilationTest, HexTilingWithAHeightMapIsRefused)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
    Logger::Log::Warning("hex refusal capture live");

    MaterialDocument doc = MakeHeightMappedDocument("HexRelief", "Surfaces/standard_pbr.glsl");
    doc.properties["hexTiling"] = 1.0f;
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_FALSE(CompilesParallax(*mat)) << "the material renders without parallax";

    Logger::Log::Flush();
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "hex refusal capture live"), 1u) << "capture not live";
    EXPECT_EQ(TestLog::CountLinesContaining(lines, kHexTilingRefusal), 1u);
}

// The material inspector lays out a document no open scene uses, and a document just edited, the
// way registration will build it: the refusal comes from the document itself, with no material
// registered.
TEST_F(MaterialPipelineCompilationTest, AnUnregisteredDocumentShowsItsRefusal)
{
    MaterialDocument doc = MakeHeightMappedDocument("RefusalRow", "Surfaces/standard_pbr.glsl");
    doc.properties["hexTiling"] = 1.0f;
    const auto hex = m_rs->Materials().ResolveSurfaceTextureUse(doc, {});
    EXPECT_EQ(hex.ParallaxRefusal, kHexTilingRefusal);
    EXPECT_NE(std::find(hex.DeclaredNames.begin(), hex.DeclaredNames.end(), "heightMap"), hex.DeclaredNames.end())
        << "the standard surface's declared names lost its height map";

    doc.properties["hexTiling"] = 0.0f;
    const auto fixed = m_rs->Materials().ResolveSurfaceTextureUse(doc, {});
    EXPECT_TRUE(fixed.ParallaxRefusal.empty()) << fixed.ParallaxRefusal;

    const MaterialDocument triplanar = MakeHeightMappedDocument("RefusalRow", "Surfaces/triplanar_pbr.glsl");
    EXPECT_EQ(m_rs->Materials().ResolveSurfaceTextureUse(triplanar, {}).ParallaxRefusal, kTriplanarRefusal);

    // Registration decides by the same rule: the refused document compiles no march, the fixed one does.
    doc.properties["hexTiling"] = 1.0f;
    Material* refused = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(refused, nullptr);
    EXPECT_FALSE(CompilesParallax(*refused));
    doc.properties["hexTiling"] = 0.0f;
    Material* marching = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(marching, nullptr);
    EXPECT_TRUE(CompilesParallax(*marching));
}

// A surface .glsl edit must reach the registered materials that use it:
// NotifyShaderSourceEdited + the BeginFrame drain invalidate the in-memory
// shader cache entries for that file and recompile the referencing pipelines.
// Before this lane existed, a saved surface edit changed NOTHING on screen —
// the hot-reload bridge cleared VkPipelines but the ShaderCompilationCache
// kept serving the old SPIR-V (2026-07-25 authoring-loop walk).
TEST_F(MaterialPipelineCompilationTest, ShaderSourceEditPropagatesToRegisteredMaterials)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_edit_prop_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    // No mtime handling: the disk shaderpkg key hashes the CONTENT of the surface
    // the composer substitutes as a literal #include, so a same-tick rewrite keys
    // differently and the pipeline-id assertions below cannot see a stale package.
    auto writeSurface = [&](const char* file, const char* colorExpr)
    {
        const fs::path path = projectRoot / file;
        std::ofstream out(path, std::ios::binary);
        out << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = "
            << colorExpr
            << ";\n"
               "    return o;\n"
               "}\n";
    };
    writeSurface("edit_me.glsl", "vec3(0.0, 1.0, 0.0)");
    writeSurface("bystander.glsl", "vec3(0.5, 0.5, 0.5)");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = std::filesystem::temp_directory_path() / "ge_test_shader_cache";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    MaterialDocument docA{};
    docA.materialName = "EditTarget";
    docA.lightingModel = "StandardPBR";
    docA.surfaceShader = "edit_me.glsl";
    MaterialDocument docB = docA;
    docB.materialName = "Bystander";
    docB.surfaceShader = "bystander.glsl";

    const GUID guidA = GUID::Generate();
    const GUID guidB = GUID::Generate();
    Material* matA = m_rs->Materials().RegisterMaterialFromDocument(guidA, docA);
    Material* matB = m_rs->Materials().RegisterMaterialFromDocument(guidB, docB);
    ASSERT_NE(matA, nullptr);
    ASSERT_NE(matB, nullptr);
    ASSERT_TRUE(matA->GetGraphicsPipelineId().IsValid());
    ASSERT_TRUE(matB->GetGraphicsPipelineId().IsValid());
    const auto idA1 = matA->GetGraphicsPipelineId();
    const auto idB1 = matB->GetGraphicsPipelineId();

    // Valid edit: new SPIR-V -> new interned pipeline id for A; B untouched.
    writeSurface("edit_me.glsl", "vec3(1.0, 0.0, 0.0)");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "edit_me.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    ASSERT_TRUE(matA->GetGraphicsPipelineId().IsValid());
    EXPECT_NE(matA->GetGraphicsPipelineId().Value, idA1.Value)
        << "a surface content edit must produce a recompiled pipeline";
    EXPECT_EQ(matB->GetGraphicsPipelineId().Value, idB1.Value)
        << "materials on other surfaces must not be touched";
    const auto idA2 = matA->GetGraphicsPipelineId();

    // Broken edit: compile fails, the previous pipeline must survive
    // (fail-visible: mesh keeps rendering with the last good shader).
    writeSurface("edit_me.glsl", "vec3(1.0, 0.0, 0.0"); // unbalanced paren
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "edit_me.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    ASSERT_TRUE(matA->GetGraphicsPipelineId().IsValid());
    EXPECT_EQ(matA->GetGraphicsPipelineId().Value, idA2.Value)
        << "a failed recompile must keep the previous pipeline running";

    // Fix it: the failure memo must not wedge the material — the next edit
    // recompiles and publishes a fresh pipeline.
    writeSurface("edit_me.glsl", "vec3(0.0, 0.0, 1.0)");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "edit_me.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    ASSERT_TRUE(matA->GetGraphicsPipelineId().IsValid());
    EXPECT_NE(matA->GetGraphicsPipelineId().Value, idA2.Value)
        << "a fixed surface must recompile after a failed edit";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// Registration is a per-frame path for a runtime material, and the declared-
// property resolve underneath is filesystem work. A re-registration that changes
// neither shader reference nor the material's directory keeps its table even
// when the file on disk moved (the shader did not recompile either, so the two
// stay coherent); the shader-source edit re-applies it through its own path, and
// a reference change re-resolves on the registration itself.
TEST_F(MaterialPipelineCompilationTest, ReregistrationKeepsTheDeclaredTableUntilAReferenceOrSourceChanges)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_declared_rereg_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);
    auto writeSurface = [&](const char* file, const char* declarations)
    {
        std::ofstream out(projectRoot / file, std::ios::binary);
        out << declarations
            << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    return o;\n"
               "}\n";
    };
    writeSurface("declared_a.glsl", "// @property float density default=1\n");
    writeSurface("declared_b.glsl", "// @property float glow default=1\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = std::filesystem::temp_directory_path() / "ge_test_shader_cache";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    MaterialDocument doc{};
    doc.materialName = "DeclaredReregistration";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "declared_a.glsl";
    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const ShaderPropertyTable* table = mat->GetDeclaredProperties();
    ASSERT_NE(table, nullptr);
    ASSERT_NE(table->Find("density"), nullptr);

    // A longer file so the stamp-keyed cache would re-parse if the resolve ran.
    writeSurface("declared_a.glsl",
                 "// @property float density default=1\n"
                 "// @property float extra default=2\n");
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);
    EXPECT_EQ(mat->GetDeclaredProperties(), table)
        << "an unchanged re-registration must not re-resolve the table";
    EXPECT_EQ(mat->GetDeclaredProperties()->Find("extra"), nullptr);

    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "declared_a.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    ASSERT_NE(mat->GetDeclaredProperties(), nullptr);
    EXPECT_NE(mat->GetDeclaredProperties()->Find("extra"), nullptr)
        << "the shader-source edit re-applies the table with the recompile";

    doc.surfaceShader = "declared_b.glsl";
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);
    ASSERT_NE(mat->GetDeclaredProperties(), nullptr);
    EXPECT_NE(mat->GetDeclaredProperties()->Find("glow"), nullptr)
        << "a surface reference change re-resolves on the registration";
    EXPECT_EQ(mat->GetDeclaredProperties()->Find("density"), nullptr);

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// ---- Fixed-function state on re-registration (.material hot-reload) ----
//
// `blend`, `zWrite` and `zTest` are the inputs to DeriveMaterialBlendState, whose
// output supplies the pipeline's color-blend attachment and its depth write/test
// flags. A hot-reload that edits one of them therefore has to rebuild the PSO, or
// the reload updates the document while the material keeps drawing with the old
// depth/blend state. Each case pins both halves: the recompile ran (version bump)
// and the live pipeline desc carries the newly authored state.
//
// The re-registration gate compares the DERIVED state rather than the three raw
// fields, so the last case pins the other direction: an edit the derivation
// discards must not spend a recompile.

TEST_F(MaterialPipelineCompilationTest, ZTestEditRebuildsThePipeline)
{
    MaterialDocument doc{};
    doc.materialName = "ZTestReload";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const uint32_t versionBefore = mat->GetVersion();
    const auto* before = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(before, nullptr);
    ASSERT_TRUE(before->DepthStencil.depthTestEnable);

    doc.zTest = false;
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);

    EXPECT_GT(mat->GetVersion(), versionBefore) << "a zTest edit must recompile";
    const auto* after = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(after, nullptr);
    EXPECT_FALSE(after->DepthStencil.depthTestEnable)
        << "the rebuilt pipeline must carry the authored zTest";
}

TEST_F(MaterialPipelineCompilationTest, ZWriteEditRebuildsThePipeline)
{
    MaterialDocument doc{};
    doc.materialName = "ZWriteReload";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const uint32_t versionBefore = mat->GetVersion();
    const auto* before = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(before, nullptr);
    ASSERT_TRUE(before->DepthStencil.depthWriteEnable);

    doc.zWrite = false;
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);

    EXPECT_GT(mat->GetVersion(), versionBefore) << "a zWrite edit must recompile";
    const auto* after = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(after, nullptr);
    EXPECT_FALSE(after->DepthStencil.depthWriteEnable)
        << "the rebuilt pipeline must carry the authored zWrite";
}

TEST_F(MaterialPipelineCompilationTest, BlendEquationEditRebuildsThePipeline)
{
    MaterialDocument doc{};
    doc.materialName = "BlendReload";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = MaterialAlphaMode::Blend;

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const uint32_t versionBefore = mat->GetVersion();
    const auto* before = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->ColorBlend.attachments.size(), 1u);
    ASSERT_EQ(before->ColorBlend.attachments[0].dstColorBlendFactor,
              BlendFactor::OneMinusSrcAlpha);

    // Straight-alpha -> additive.
    MaterialBlendState additive{};
    additive.SrcColorFactor = MaterialBlendFactor::One;
    additive.DstColorFactor = MaterialBlendFactor::One;
    doc.blend = additive;
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);

    EXPECT_GT(mat->GetVersion(), versionBefore) << "a blend-equation edit must recompile";
    const auto* after = m_device->LookupGraphicsPipeline(mat->GetGraphicsPipelineId());
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->ColorBlend.attachments.size(), 1u);
    EXPECT_EQ(after->ColorBlend.attachments[0].srcColorBlendFactor, BlendFactor::One);
    EXPECT_EQ(after->ColorBlend.attachments[0].dstColorBlendFactor, BlendFactor::One)
        << "the rebuilt pipeline must carry the authored blend equation";
}

// A property value is uniform data, not pipeline state: it must not spend a
// recompile on every re-registration (runtime materials re-register per frame).
TEST_F(MaterialPipelineCompilationTest, PropertyEditDoesNotRebuildThePipeline)
{
    MaterialDocument doc{};
    doc.materialName = "PropertyReload";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.properties["roughness"] = 0.5f;

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const uint32_t versionBefore = mat->GetVersion();

    doc.properties["roughness"] = 0.25f;
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);

    EXPECT_EQ(mat->GetVersion(), versionBefore)
        << "a property-value edit must not recompile the pipeline";
}

// `blend` is only consulted for Blend materials, so editing it on an Opaque one
// changes nothing the pipeline can see. This is why the gate compares the derived
// state and not the raw fields.
TEST_F(MaterialPipelineCompilationTest, BlendEditOnOpaqueDoesNotRebuildThePipeline)
{
    MaterialDocument doc{};
    doc.materialName = "OpaqueBlendReload";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const uint32_t versionBefore = mat->GetVersion();

    MaterialBlendState additive{};
    additive.SrcColorFactor = MaterialBlendFactor::One;
    additive.DstColorFactor = MaterialBlendFactor::One;
    doc.blend = additive;
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), mat);

    EXPECT_EQ(mat->GetVersion(), versionBefore)
        << "a blend edit an Opaque material's pipeline cannot see must not recompile";
}

// The drain runs FULL shaderc compiles on the serial frame-begin thread, so K
// materials sharing one edited surface must not all recompile inside a single
// BeginFrame (~250-500ms each cold — a K-compile stall is a visible freeze).
// kShaderEditRecompileBudgetPerFrame caps the per-drain work and carries the
// remainder; this pins both halves of that contract: one drain does NOT finish
// the set, and successive drains DO — with bystanders never touched.
TEST_F(MaterialPipelineCompilationTest, ShaderEditRecompilesAreBudgetedAcrossFrames)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_edit_budget_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    // No mtime handling: the disk shaderpkg key hashes the CONTENT of the surface
    // the composer substitutes as a literal #include, so a same-tick rewrite keys
    // differently. Without content-keying, a same-tick rewrite could serve the old
    // package, the recompile would yield byte-identical SPIR-V and therefore an
    // unchanged pipeline id, and that reads as a budget failure rather than the
    // cache artifact it is.
    auto writeSurface = [&](const char* file, const char* colorExpr)
    {
        const fs::path path = projectRoot / file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = "
            << colorExpr
            << ";\n"
               "    return o;\n"
               "}\n";
    };
    writeSurface("shared_edit.glsl", "vec3(0.0, 1.0, 0.0)");
    writeSurface("bystander.glsl", "vec3(0.5, 0.5, 0.5)");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    // More materials on the shared surface than any sane per-frame budget.
    constexpr size_t kSharedCount = 5;
    std::vector<Material*> shared;
    std::vector<uint32_t> beforeIds;
    for (size_t i = 0; i < kSharedCount; ++i)
    {
        MaterialDocument doc{};
        doc.materialName = "BudgetTarget" + std::to_string(i);
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "shared_edit.glsl";
        Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
        ASSERT_NE(mat, nullptr);
        ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
        shared.push_back(mat);
        beforeIds.push_back(mat->GetGraphicsPipelineId().Value);
    }

    MaterialDocument bystanderDoc{};
    bystanderDoc.materialName = "BudgetBystander";
    bystanderDoc.lightingModel = "StandardPBR";
    bystanderDoc.surfaceShader = "bystander.glsl";
    Material* bystander =
        m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), bystanderDoc);
    ASSERT_NE(bystander, nullptr);
    ASSERT_TRUE(bystander->GetGraphicsPipelineId().IsValid());
    const uint32_t bystanderId = bystander->GetGraphicsPipelineId().Value;

    auto recompiledCount = [&]
    {
        size_t n = 0;
        for (size_t i = 0; i < shared.size(); ++i)
        {
            if (shared[i]->GetGraphicsPipelineId().Value != beforeIds[i])
                ++n;
        }
        return n;
    };

    writeSurface("shared_edit.glsl", "vec3(1.0, 0.0, 0.0) * 0.25"); // different length too
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "shared_edit.glsl");

    // Drain 1: the budget must clip. Asserted as a range rather than the exact
    // constant so tuning the budget doesn't rewrite the test — what matters is
    // that progress is made AND the frame is not asked to do all K compiles.
    m_rs->Materials().DrainPendingShaderSourceEdits();
    const size_t afterFirst = recompiledCount();
    EXPECT_GT(afterFirst, 0u) << "the first drain must make progress, not defer everything";
    EXPECT_LT(afterFirst, kSharedCount)
        << "one drain must not run all " << kSharedCount << " compiles — that is the hitch this "
           "budget exists to bound";

    // Successive drains (i.e. following BeginFrames) must finish the carry. The
    // loop is bounded so a carry that never drains fails instead of hanging.
    size_t drains = 1;
    while (recompiledCount() < kSharedCount && drains < 32)
    {
        m_rs->Materials().DrainPendingShaderSourceEdits();
        ++drains;
    }
    EXPECT_EQ(recompiledCount(), kSharedCount)
        << "every material sharing the edited surface must end up recompiled (" << drains
        << " drains)";
    EXPECT_GT(drains, 1u) << "the carry must actually have spanned more than one drain";

    // Bystanders are untouched throughout, and the carry is empty: a further
    // drain is a no-op, not a re-run of the whole affected set.
    EXPECT_EQ(bystander->GetGraphicsPipelineId().Value, bystanderId)
        << "materials on other surfaces must never be recompiled by this lane";
    std::vector<uint32_t> settled;
    for (Material* mat : shared)
        settled.push_back(mat->GetGraphicsPipelineId().Value);
    m_rs->Materials().DrainPendingShaderSourceEdits();
    for (size_t i = 0; i < shared.size(); ++i)
    {
        EXPECT_EQ(shared[i]->GetGraphicsPipelineId().Value, settled[i])
            << "a drain with an empty carry and no new edits must do nothing (material " << i
            << ")";
    }

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// An INCLUDE edit — a file no material's spec names — must reach every material
// whose surface pulls it in transitively, and only those. The trigger matches
// the edited filename against each compile's recorded include closure; before
// closure recording existed, saving an include fired NOTHING (walk finding 6).
TEST_F(MaterialPipelineCompilationTest, IncludeEditPropagatesToMaterialsWhoseSurfacesIncludeIt)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_include_edit_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    auto writeFile = [&](const char* file, const std::string& text)
    {
        const fs::path path = projectRoot / file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << text;
    };
    auto writeHelper = [&](const char* colorExpr)
    {
        writeFile("shared_helper.glsl",
                  std::string("vec3 SharedHelperColor() { return ") + colorExpr + "; }\n");
    };
    auto writeIncludingSurface = [&](const char* file)
    {
        writeFile(file,
                  "#include \"shared_helper.glsl\"\n"
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                  "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "    o.baseColor = SharedHelperColor();\n"
                  "    return o;\n"
                  "}\n");
    };
    writeHelper("vec3(0.0, 1.0, 0.0)");
    writeIncludingSurface("inc_user_a.glsl");
    writeIncludingSurface("inc_user_b.glsl");
    writeFile("inc_bystander.glsl",
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
              "    SurfaceOutput o = DefaultSurfaceOutput();\n"
              "    o.baseColor = vec3(0.5, 0.5, 0.5);\n"
              "    return o;\n"
              "}\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    auto registerOn = [&](const char* name, const char* surface)
    {
        MaterialDocument doc{};
        doc.materialName = name;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = surface;
        Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
        EXPECT_NE(mat, nullptr);
        EXPECT_TRUE(mat && mat->GetGraphicsPipelineId().IsValid());
        return mat;
    };
    Material* matA = registerOn("IncUserA", "inc_user_a.glsl");
    Material* matB = registerOn("IncUserB", "inc_user_b.glsl");
    Material* bystander = registerOn("IncBystander", "inc_bystander.glsl");
    ASSERT_TRUE(matA && matB && bystander);
    const uint32_t idA1 = matA->GetGraphicsPipelineId().Value;
    const uint32_t idB1 = matB->GetGraphicsPipelineId().Value;
    const uint32_t idBystander = bystander->GetGraphicsPipelineId().Value;

    // Edit the INCLUDE. Neither material's spec names it — only the recorded
    // closure can route this. Budget-paced: drain until the carry settles.
    writeHelper("vec3(1.0, 0.0, 0.0) * 0.25");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "shared_helper.glsl");
    for (size_t drains = 0; drains < 32; ++drains)
    {
        m_rs->Materials().DrainPendingShaderSourceEdits();
        if (matA->GetGraphicsPipelineId().Value != idA1
            && matB->GetGraphicsPipelineId().Value != idB1)
            break;
    }
    EXPECT_NE(matA->GetGraphicsPipelineId().Value, idA1)
        << "material A's surface includes the edited helper — it must recompile";
    EXPECT_NE(matB->GetGraphicsPipelineId().Value, idB1)
        << "material B's surface includes the edited helper — it must recompile";
    EXPECT_EQ(bystander->GetGraphicsPipelineId().Value, idBystander)
        << "a material whose closure does not contain the edited file must not recompile";

    // An edit to a file in NO closure fires nothing.
    const uint32_t idA2 = matA->GetGraphicsPipelineId().Value;
    const uint32_t idB2 = matB->GetGraphicsPipelineId().Value;
    writeFile("orphan_helper.glsl", "vec3 OrphanColor() { return vec3(1.0); }\n");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "orphan_helper.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    m_rs->Materials().DrainPendingShaderSourceEdits();
    EXPECT_EQ(matA->GetGraphicsPipelineId().Value, idA2);
    EXPECT_EQ(matB->GetGraphicsPipelineId().Value, idB2);
    EXPECT_EQ(bystander->GetGraphicsPipelineId().Value, idBystander)
        << "an edit outside every closure must recompile nothing";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// A SECOND save of the same file while the first save's recompiles are still
// carried across budgeted frames.
//
// By then most affected entries are already out of the cache — the first sweep
// dropped them and their materials have not had their budgeted turn yet — so a
// requeue driven by "which entries did this sweep drop" would name only the
// handful recompiled in between. The carry is REPLACED by the newest save's
// affected set, so the rest would be dropped from the queue while still holding
// their pre-edit pipelines, with nothing left to disturb them.
//
// The requeue is driven by the closure ROWS instead, which outlive the entries.
// Each material gets its own surface so each has its own source identity;
// materials sharing one identity would hide the hazard, because the first one
// recompiled repopulates the entry the others would then be found through.
TEST_F(MaterialPipelineCompilationTest, RepeatEditMidCarry_StillRequeuesSweptMaterials)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_repeat_edit_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    auto writeFile = [&](const std::string& file, const std::string& text)
    {
        const fs::path path = projectRoot / file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << text;
    };
    auto writeHelper = [&](const char* colorExpr)
    {
        writeFile("carry_helper.glsl",
                  std::string("vec3 CarryHelperColor() { return ") + colorExpr + "; }\n");
    };

    // More materials than two budgeted frames can finish, each on its own surface.
    constexpr size_t kCount = 5;
    static_assert(kCount > 4, "must exceed 2x the per-frame recompile budget");
    writeHelper("vec3(0.0, 1.0, 0.0)");
    for (size_t i = 0; i < kCount; ++i)
    {
        writeFile("carry_surface_" + std::to_string(i) + ".glsl",
                  "#include \"carry_helper.glsl\"\n"
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                  "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "    o.baseColor = CarryHelperColor();\n"
                  "    return o;\n"
                  "}\n");
    }

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    std::vector<Material*> riders;
    std::vector<uint32_t> beforeIds;
    for (size_t i = 0; i < kCount; ++i)
    {
        MaterialDocument doc{};
        doc.materialName = "CarryRider" + std::to_string(i);
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "carry_surface_" + std::to_string(i) + ".glsl";
        Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
        ASSERT_NE(mat, nullptr);
        ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
        riders.push_back(mat);
        beforeIds.push_back(mat->GetGraphicsPipelineId().Value);
    }

    auto recompiledCount = [&]
    {
        size_t n = 0;
        for (size_t i = 0; i < riders.size(); ++i)
        {
            if (riders[i]->GetGraphicsPipelineId().Value != beforeIds[i])
                ++n;
        }
        return n;
    };

    // Save 1: the budget clips, leaving materials carried whose entries are gone.
    writeHelper("vec3(1.0, 0.0, 0.0) * 0.25");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "carry_helper.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    const size_t afterFirst = recompiledCount();
    ASSERT_GT(afterFirst, 0u) << "the first drain must make progress";
    ASSERT_LT(afterFirst, kCount)
        << "the first drain must leave materials carried, or this test proves nothing";

    // Save 2, mid-carry. This replaces the carry outright, so the new affected
    // set has to name the materials whose entries the first sweep already took.
    writeHelper("vec3(0.0, 0.0, 1.0) * 0.5");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "carry_helper.glsl");
    for (size_t drains = 0; drains < 32 && recompiledCount() < kCount; ++drains)
        m_rs->Materials().DrainPendingShaderSourceEdits();

    EXPECT_EQ(recompiledCount(), kCount)
        << "every material on the edited include must end up recompiled — including the ones "
           "whose cache entries the FIRST save had already swept";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// Opening a different project in the same process must drop the SPIR-V the old
// project put in the in-memory cache. Entries are keyed by PATH, not by content,
// so the new project can author the same relative surface reference and — with
// the old entries retained — be served the old project's bytes for the rest of
// the session.
TEST_F(MaterialPipelineCompilationTest, ProjectSwitchDropsRetainedShaderCache)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_project_switch_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    auto writeSurface = [&](const char* colorExpr)
    {
        const fs::path path = projectRoot / "switch_me.glsl";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = "
            << colorExpr
            << ";\n"
               "    return o;\n"
               "}\n";
    };

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};

    MaterialDocument doc{};
    doc.materialName = "SwitchRider";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "switch_me.glsl";

    writeSurface("vec3(0.0, 1.0, 0.0)"); // project A's file
    m_rs->Materials().SetMaterialBuildContext(ctx);
    Material* matA = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(matA, nullptr);
    ASSERT_TRUE(matA->GetGraphicsPipelineId().IsValid());
    const uint32_t idA = matA->GetGraphicsPipelineId().Value;

    // Project B has a DIFFERENT file at the same relative path — the case a
    // path-keyed entry cannot distinguish on its own.
    writeSurface("vec3(1.0, 0.0, 0.0) * 0.25");
    m_rs->Materials().OnProjectSwitched();
    // The editor re-derives the build context from the new workspace root right
    // after the switch; without that the reset alone leaves nothing able to compile.
    m_rs->Materials().SetMaterialBuildContext(ctx);

    Material* matB = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(matB, nullptr);
    ASSERT_TRUE(matB->GetGraphicsPipelineId().IsValid());
    EXPECT_NE(matB->GetGraphicsPipelineId().Value, idA)
        << "a material registered after a project switch must compile the new project's source, "
           "not be served the previous project's retained SPIR-V";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// Two materials in DIFFERENT directories can author the SAME surface filename and
// resolve to two different files, so an edit to one's private helper must recompile
// that one and leave the co-rider alone. The requeue probe folds the material asset
// path into the source identity it looks up; a probe that dropped the path would
// collapse both materials onto one identity and the co-rider would ride along.
//
// Registration cannot supply the asset path headlessly: MaterialSystem derives it
// from the asset database (needs an initialized EngineCore) or from the material
// compiler's record (needs a live engine), and this fixture has neither. Without
// the TestFactory seam every material here carries an EMPTY path, and a probe built
// from an empty path matches every row whatever it probes with — which is exactly
// why a mutation blanking the probe's asset path left the whole suite green.
TEST_F(MaterialPipelineCompilationTest, SameNamedSurfaceInTwoDirs_RequeueOnlyTheEditedRider)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_twodir_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    const fs::path dirA = projectRoot / "matA";
    const fs::path dirB = projectRoot / "matB";
    fs::create_directories(dirA);
    fs::create_directories(dirB);

    auto write = [](const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << text;
    };
    // One surface FILENAME, two files, each pulling in its own private helper — so
    // a helper edit can only route through the recorded closure row, never through
    // a spec's own reference.
    static constexpr const char* kSurfaceBody =
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
        "    SurfaceOutput o = DefaultSurfaceOutput();\n"
        "    o.baseColor = TwoDirColor();\n"
        "    return o;\n"
        "}\n";
    write(dirA / "helper_a.glsl", "vec3 TwoDirColor() { return vec3(0.0, 1.0, 0.0); }\n");
    write(dirB / "helper_b.glsl", "vec3 TwoDirColor() { return vec3(0.0, 0.0, 1.0); }\n");
    write(dirA / "twodir_surface.glsl",
          std::string("#include \"helper_a.glsl\"\n") + kSurfaceBody);
    write(dirB / "twodir_surface.glsl",
          std::string("#include \"helper_b.glsl\"\n") + kSurfaceBody);

    // Neither surface sits at a project ROOT: shader resolution is
    // directory-local, so the only way to reach one is through the owning
    // material's own directory. That is also why the path-less first registration
    // below cannot compose — and why no row keyed on the empty asset path is ever
    // recorded, leaving the per-material rows as the only ones in play.
    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "twodir_surface.glsl";

    auto registerIn = [&](const char* name, const fs::path& materialAssetPath) -> Material*
    {
        MaterialDocument d = doc;
        d.materialName = name;
        const GUID guid = GUID::Generate();
        Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, d);
        if (!mat)
            return nullptr;
        EXPECT_FALSE(mat->GetGraphicsPipelineId().IsValid())
            << "with no asset path the surface cannot resolve at all — the compose failure here "
               "is what guarantees no empty-path closure row exists to mask the probe";
        // The row the probe must match is recorded from the key
        // CompileMaterialPipeline builds, so the asset path has to be on the
        // material BEFORE that build — seam, then recompile.
        Material::TestFactory::SetMaterialAssetPath(*mat, materialAssetPath);
        m_rs->Materials().RecompileMaterialPipeline(guid, d);
        return mat;
    };
    Material* matA = registerIn("TwoDirA", dirA / "a.material");
    Material* matB = registerIn("TwoDirB", dirB / "b.material");
    ASSERT_TRUE(matA != nullptr && matB != nullptr);
    ASSERT_TRUE(matA->GetGraphicsPipelineId().IsValid());
    ASSERT_TRUE(matB->GetGraphicsPipelineId().IsValid());
    const uint32_t idA1 = matA->GetGraphicsPipelineId().Value;
    const uint32_t idB1 = matB->GetGraphicsPipelineId().Value;
    ASSERT_NE(idA1, idB1)
        << "the two materials must have compiled DIFFERENT surfaces, or the asset path never "
           "entered the identity and this test proves nothing";

    // Edit A's private helper. Neither spec names it; only A's closure row can
    // route it, and only A's row may match.
    write(dirA / "helper_a.glsl", "vec3 TwoDirColor() { return vec3(1.0, 0.0, 0.0) * 0.25; }\n");
    m_rs->Materials().NotifyShaderSourceEdited(dirA / "helper_a.glsl");
    for (size_t drains = 0; drains < 32; ++drains)
    {
        m_rs->Materials().DrainPendingShaderSourceEdits();
        if (matA->GetGraphicsPipelineId().Value != idA1)
            break;
    }
    EXPECT_NE(matA->GetGraphicsPipelineId().Value, idA1)
        << "the material whose surface includes the edited helper must recompile";
    EXPECT_EQ(matB->GetGraphicsPipelineId().Value, idB1)
        << "the co-rider resolves a DIFFERENT file of the same name — it must not recompile";

    // ...and symmetrically, so neither result is an artifact of row iteration order.
    const uint32_t idA2 = matA->GetGraphicsPipelineId().Value;
    write(dirB / "helper_b.glsl", "vec3 TwoDirColor() { return vec3(1.0, 1.0, 0.0) * 0.5; }\n");
    m_rs->Materials().NotifyShaderSourceEdited(dirB / "helper_b.glsl");
    for (size_t drains = 0; drains < 32; ++drains)
    {
        m_rs->Materials().DrainPendingShaderSourceEdits();
        if (matB->GetGraphicsPipelineId().Value != idB1)
            break;
    }
    EXPECT_NE(matB->GetGraphicsPipelineId().Value, idB1)
        << "B's own helper edit must reach B";
    EXPECT_EQ(matA->GetGraphicsPipelineId().Value, idA2)
        << "A must not ride along on B's helper edit either";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// A material that has never built anything has NO closure row, so the identity
// match cannot name it: the requeue's direct surface / vertex-modifier reference
// check is the only thing that can reach it. That is the path the inspector's
// "Recompile Shader" button rides — it calls NotifyShaderSourceEdited with the
// authored surface string — and the button's own hint promises it covers edits the
// watcher missed.
//
// The repro has to be a COMPOSE failure, not a compile failure. A bad
// EvaluateSurface signature errors inside the compile service, which records the
// closure it resolved anyway (ShaderCompilationCacheClosureTest.
// FailedCompile_RecordsClosureOfPresentButBrokenInclude), and the identity match
// would then subsume the direct check. An authored surface that does not RESOLVE
// fails in ShaderComposer before any dependency is recorded.
TEST_F(MaterialPipelineCompilationTest, NeverCompiledMaterial_RequeuedByDirectSurfaceReference)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_late_surface_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    MaterialDocument doc{};
    doc.materialName = "LateSurfaceRider";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "arrives_late.glsl";

    // Register with the surface file ABSENT. An authored surface that does not
    // resolve is a hard compose failure, so nothing is composed and no dependency
    // is recorded: the material exists with no pipeline and no closure row.
    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_FALSE(mat->GetGraphicsPipelineId().IsValid())
        << "an unresolved authored surface must fail the build, or a closure row exists and this "
           "test cannot isolate the direct-reference check";

    // The author's fix: the file appears. Only the direct spec reference can
    // requeue this material — no row exists to name its identity.
    {
        std::ofstream out(projectRoot / "arrives_late.glsl", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = vec3(0.0, 1.0, 0.0);\n"
               "    return o;\n"
               "}\n";
    }
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "arrives_late.glsl");
    m_rs->Materials().DrainPendingShaderSourceEdits();
    EXPECT_TRUE(mat->GetGraphicsPipelineId().IsValid())
        << "a material that never compiled must still be requeued by its own authored surface "
           "reference — that is what the inspector's Recompile Shader button depends on";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// The project switch clears the compiled entries but must NOT clear the closure
// ROWS. SetProjectFolder does not tear down the material registry, so every
// material registered before the switch survives holding a live pipeline, and
// re-registration only recompiles when pipeline-affecting state changed — which for
// a primitive or the default material it has not. With their rows gone, an adapter
// / engine-include / transitive-helper edit can name no identity at all, and the
// direct filename check by construction cannot fire for a file no spec references,
// so the include-edit trigger is dead for those materials for the rest of the
// session. Reachable on an ORDINARY startup: EditorApplication::Initialize
// auto-loads the last project through SetProjectFolder.
TEST_F(MaterialPipelineCompilationTest, ProjectSwitchKeepsClosureRowsForSurvivingMaterials)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("ge_shader_switch_rows_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(projectRoot);

    auto writeFile = [&](const char* file, const std::string& text)
    {
        const fs::path path = projectRoot / file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << text;
    };
    auto writeHelper = [&](const char* colorExpr)
    {
        writeFile("switch_helper.glsl",
                  std::string("vec3 SwitchHelperColor() { return ") + colorExpr + "; }\n");
    };
    writeHelper("vec3(0.0, 1.0, 0.0)");
    writeFile("switch_rider.glsl",
              "#include \"switch_helper.glsl\"\n"
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
              "    SurfaceOutput o = DefaultSurfaceOutput();\n"
              "    o.baseColor = SwitchHelperColor();\n"
              "    return o;\n"
              "}\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_shaderDir;
    ctx.CacheRoot = projectRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {m_shaderDir};
    ctx.ProjectRoots = {projectRoot};
    m_rs->Materials().SetMaterialBuildContext(ctx);

    MaterialDocument doc{};
    doc.materialName = "SwitchSurvivor";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "switch_rider.glsl";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const uint32_t idBefore = mat->GetGraphicsPipelineId().Value;

    // The switch. The material is neither re-registered nor recompiled — exactly
    // the survivor this test is about. The editor re-derives the build context from
    // the new workspace root right afterwards.
    m_rs->Materials().OnProjectSwitched();
    m_rs->Materials().SetMaterialBuildContext(ctx);
    ASSERT_EQ(mat->GetGraphicsPipelineId().Value, idBefore)
        << "the switch must leave the survivor's pipeline alone, or it is not a survivor";

    // An INCLUDE edit: no spec names this file, so the closure row is the only
    // route. If the switch dropped the row, nothing fires.
    writeHelper("vec3(1.0, 0.0, 0.0) * 0.25");
    m_rs->Materials().NotifyShaderSourceEdited(projectRoot / "switch_helper.glsl");
    for (size_t drains = 0; drains < 32; ++drains)
    {
        m_rs->Materials().DrainPendingShaderSourceEdits();
        if (mat->GetGraphicsPipelineId().Value != idBefore)
            break;
    }
    EXPECT_NE(mat->GetGraphicsPipelineId().Value, idBefore)
        << "a material that survived the project switch must still be reachable by an include "
           "edit — its closure row has to outlive the switch";

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// ---- Hot-reload propagation tests ----
// Verify that EvictTextureCacheEntry walks the tracked material refs, resets
// stale bindless indices, queues re-binds, and clears the tracking maps.

class TextureHotReloadPropagationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        m_rs = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_rs->Initialize(m_device.get()));
    }

    void TearDown() override
    {
        if (m_rs)
            m_rs->Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    Material* MakeMaterial(const GUID& guid, const char* name = "HotReloadTest")
    {
        MaterialDocument doc{};
        doc.materialName = name;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "surfaces/standard_surface.glsl";
        return m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    }

    std::unique_ptr<IDevice> m_device;
    std::unique_ptr<RenderServices> m_rs;
};

TEST_F(TextureHotReloadPropagationTest, EvictResetsMaterialBindlessIndexToDefault)
{
    GUID matGuid = GUID::Generate();
    GUID texGuid = GUID::Generate();
    StringId slot = "albedoMap"_sid;

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    // Stash the bindless default for this slot so we can compare against it.
    const uint32_t defaultIdx = mat->GetBindlessTextureIndex(slot);

    // Simulate a real binding: a non-default bindless index points at the
    // texture's slot in the bindless heap. Track the (mat, slot) -> texGuid
    // edge so eviction finds this material.
    constexpr uint32_t kFakeBindlessIdx = 42u;
    mat->SetBindlessTextureIndex(slot, kFakeBindlessIdx);
    ASSERT_EQ(mat->GetBindlessTextureIndex(slot), kFakeBindlessIdx);
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, slot, texGuid);

    m_rs->Textures().Evict(texGuid);

    // Material's bindless index must be reset to the slot's default —
    // sampling from the stale index after the GPU handle was destroyed
    // would read garbage from the parked bindless slot.
    EXPECT_EQ(mat->GetBindlessTextureIndex(slot), defaultIdx);
}

TEST_F(TextureHotReloadPropagationTest, RebindRemovesPreviousReverseEntry)
{
    GUID matGuid = GUID::Generate();
    GUID texA = GUID::Generate();
    GUID texB = GUID::Generate();
    StringId slot = "albedoMap"_sid;

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, slot, texA);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texA), 1u);

    // Rebind same slot to a different texture. The previous (mat, slot) entry
    // under texA must be removed so a later eviction of texA doesn't try to
    // "rebind" a slot that has moved on.
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, slot, texB);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texA), 0u);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texB), 1u);
}

// Evicting a texture three materials share queues a rebind for every one of them
// and keeps the (material, slot) -> texture references, so the rebind sweep finds
// them and a second eviction during the reload re-triggers. The pending-bind count
// is the scene-wide companion to IsMaterialTextureBindingComplete, and the number
// get_editor_state / take_screenshot report: a capture taken while any bind is
// outstanding renders bindless defaults for those materials, which on screen is
// indistinguishable from a material that never binds at all.
TEST_F(TextureHotReloadPropagationTest, EvictQueuesARebindForEveryReferencingMaterialAndKeepsTheReferences)
{
    auto& textures = m_rs->Textures();
    EXPECT_EQ(textures.PendingMaterialTextureBindCount(), 0u)
        << "no decode in flight must report a settled frame";

    GUID texGuid = GUID::Generate();
    StringId slot = "albedoMap"_sid;
    constexpr size_t kMatCount = 3;
    GUID mats[kMatCount];
    for (size_t i = 0; i < kMatCount; ++i)
    {
        mats[i] = GUID::Generate();
        ASSERT_NE(MakeMaterial(mats[i]), nullptr);
        textures.TrackMaterialTextureRefForTesting(mats[i], slot, texGuid);
    }
    // A tracked edge is not a pending decode: these materials are bound.
    EXPECT_EQ(textures.PendingMaterialTextureBindCount(), 0u);
    EXPECT_EQ(textures.CountMaterialsReferencingTextureForTesting(texGuid), kMatCount);
    for (size_t i = 0; i < kMatCount; ++i)
        EXPECT_TRUE(textures.IsMaterialTextureBindingComplete(mats[i]));

    // Eviction re-queues one bind per (material, slot) edge, so the world-wide
    // count and the per-material answers must agree about the same window.
    textures.Evict(texGuid);
    EXPECT_EQ(textures.PendingMaterialTextureBindCount(), kMatCount);
    for (size_t i = 0; i < kMatCount; ++i)
    {
        EXPECT_FALSE(textures.IsMaterialTextureBindingComplete(mats[i]))
            << "Material " << i << " is pending, so the count must not read settled";
        GUID readBack{};
        EXPECT_TRUE(textures.MaterialTextureRefForTesting(mats[i], slot, readBack));
        EXPECT_EQ(readBack, texGuid) << "eviction must keep material " << i << "'s reference for the rebind";
    }
    EXPECT_EQ(textures.CountMaterialsReferencingTextureForTesting(texGuid), kMatCount);
}

// Audit follow-up 1.6: a `.material` hot-reload that drops a texture slot
// must unbind the previous handle. Before this fix, ApplyDocumentToMaterial
// reset transforms/properties on re-Register but left `m_Textures` and the
// bindless index untouched — the dropped slot kept sampling the stale
// texture. RegisterMaterialFromDocument now sweeps slots that are no longer
// in the new doc and resets them to bindless defaults.
TEST_F(TextureHotReloadPropagationTest, ReregisterDroppingSlotClearsBinding)
{
    GUID matGuid = GUID::Generate();
    GUID texA = GUID::Generate();
    GUID texB = GUID::Generate();
    const StringId albedo = "albedoMap"_sid;
    const StringId normal = "normalMap"_sid;

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    const uint32_t defaultAlbedoIdx = mat->GetBindlessTextureIndex(albedo);
    const uint32_t defaultNormalIdx = mat->GetBindlessTextureIndex(normal);

    // Simulate "previously bound" slots: handle, bindless index, ref-tracking.
    TextureHandle fakeAlbedo(101, 1);
    TextureHandle fakeNormal(102, 1);
    mat->SetTexture(albedo, fakeAlbedo);
    mat->SetTexture(normal, fakeNormal);
    mat->SetBindlessTextureIndex(albedo, 50u);
    mat->SetBindlessTextureIndex(normal, 51u);
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, albedo, texA);
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, normal, texB);
    ASSERT_TRUE(mat->GetTexture(albedo).IsValid());
    ASSERT_TRUE(mat->GetTexture(normal).IsValid());

    // Re-register with a doc that has no texture slots — both bindings must
    // be cleared, not silently retained.
    MaterialDocument doc{};
    doc.materialName = "ClearTest";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    Material* same = m_rs->Materials().RegisterMaterialFromDocument(matGuid, doc);
    EXPECT_EQ(same, mat);

    EXPECT_FALSE(mat->GetTexture(albedo).IsValid())
        << "Dropped slot must be unbound on re-register";
    EXPECT_FALSE(mat->GetTexture(normal).IsValid())
        << "Dropped slot must be unbound on re-register";
    EXPECT_EQ(mat->GetBindlessTextureIndex(albedo), defaultAlbedoIdx)
        << "Bindless index for dropped slot must revert to default";
    EXPECT_EQ(mat->GetBindlessTextureIndex(normal), defaultNormalIdx);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texA), 0u)
        << "Reverse ref for dropped slot must be erased";
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texB), 0u);
}

// Companion: a re-Register that keeps the slot in the doc with an embedded-image
// reference (kEmbeddedTexturePrefix + N) must NOT clear the existing binding —
// ResolveEmbeddedTextures re-binds those after RegisterMaterialFromDocument returns.
TEST_F(TextureHotReloadPropagationTest, ReregisterKeepingSlotPreservesBinding)
{
    GUID matGuid = GUID::Generate();
    GUID texA = GUID::Generate();
    const StringId albedo = "albedoMap"_sid;

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    TextureHandle fakeAlbedo(101, 1);
    mat->SetTexture(albedo, fakeAlbedo);
    mat->SetBindlessTextureIndex(albedo, 50u);
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, albedo, texA);

    // New doc keeps the slot with an embedded-style reference; the
    // texture-resolve loop skips embedded refs (null GUID), so the binding
    // must survive untouched until ResolveEmbeddedTextures runs.
    MaterialDocument doc{};
    doc.materialName = "KeepTest";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    doc.textures["albedoMap"] = std::string(kEmbeddedTexturePrefix) + "0";
    m_rs->Materials().RegisterMaterialFromDocument(matGuid, doc);

    EXPECT_EQ(mat->GetTexture(albedo), fakeAlbedo)
        << "Slot listed in new doc must NOT be cleared by the sweep";
    EXPECT_EQ(mat->GetBindlessTextureIndex(albedo), 50u);
}

TEST_F(TextureHotReloadPropagationTest, UnregisterMaterialClearsAllRefs)
{
    GUID matGuid = GUID::Generate();
    GUID texA = GUID::Generate();
    GUID texB = GUID::Generate();

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, "albedoMap"_sid, texA);
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, "normalMap"_sid, texB);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texA), 1u);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texB), 1u);

    // Unregistering the material must drop all its slot refs — otherwise a
    // future eviction would walk a dangling (matGuid, slotName) entry and
    // Find() would return nullptr per Material lookup.
    m_rs->Materials().Registry().Unregister(matGuid);

    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texA), 0u);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(texB), 0u);
}

// ---- Runtime texture override (VideoTextureSystem's binding route) ----
//
// A video texture overrides a material slot with a handle that has no asset
// GUID at all. The override is only reversible because BindMaterialTexture
// writes the GPU side WITHOUT touching the tracked authored ref: that edge is
// the sole record of what the slot is supposed to hold, and
// RestoreMaterialTextureBinding reads it back. If a bind ever cleared or
// rewrote the edge, ending the override would silently make the runtime
// texture the material's new "authored" content.

TEST_F(TextureHotReloadPropagationTest, RuntimeOverrideBindPreservesAuthoredRef)
{
    GUID matGuid = GUID::Generate();
    GUID authoredTex = GUID::Generate();
    StringId slot = "albedoMap"_sid;

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);
    m_rs->Textures().TrackMaterialTextureRefForTesting(matGuid, slot, authoredTex);

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "RuntimeOverride";
    const TextureHandle overrideTex = m_device->CreateTexture(td);
    ASSERT_TRUE(overrideTex.IsValid());

    m_rs->Textures().BindMaterialTexture(mat, slot, overrideTex);
    EXPECT_EQ(mat->GetTexture(slot), overrideTex);

    // The hinge: the authored edge is untouched, so the override is reversible.
    GUID readBack{};
    EXPECT_TRUE(m_rs->Textures().MaterialTextureRefForTesting(matGuid, slot, readBack));
    EXPECT_EQ(readBack, authoredTex);
    EXPECT_EQ(m_rs->Textures().CountMaterialsReferencingTextureForTesting(authoredTex), 1u);

    m_rs->Textures().DiscardTextureDeferred(overrideTex);
    m_rs->Textures().FlushPendingUploads();
}

TEST_F(TextureHotReloadPropagationTest, RestoreAfterOverrideReturnsSlotToDefault)
{
    GUID matGuid = GUID::Generate();
    StringId slot = "albedoMap"_sid;

    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);
    const uint32_t defaultIdx = mat->GetBindlessTextureIndex(slot);

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "RuntimeOverrideNoAuthored";
    const TextureHandle overrideTex = m_device->CreateTexture(td);
    ASSERT_TRUE(overrideTex.IsValid());

    m_rs->Textures().BindMaterialTexture(mat, slot, overrideTex);
    ASSERT_EQ(mat->GetTexture(slot), overrideTex);
    ASSERT_NE(mat->GetBindlessTextureIndex(slot), defaultIdx);

    // No authored ref for this slot: ending the override must return it to the
    // slot's bindless default, not leave the material sampling a texture that
    // is about to be retired. This is the exact release VideoTextureSystem runs
    // when a video source is cleared or its entity is disabled.
    m_rs->Textures().RestoreMaterialTextureBinding(mat, matGuid, slot);
    EXPECT_FALSE(mat->GetTexture(slot).IsValid());
    EXPECT_EQ(mat->GetBindlessTextureIndex(slot), defaultIdx);

    m_rs->Textures().DiscardTextureDeferred(overrideTex);
    m_rs->Textures().FlushPendingUploads();
}

// Publish-gate (option c): a record-worker variant miss must NOT compile inline — a
// cold shaderc variant is 10+s and would stall the record thread / scene-build pump.
// It enqueues an async compile (deduped) and skips the draw; MaterialSystem::BeginFrame
// drains the queue and submits the compiles to the prewarm workers (inline when the
// job system is down, as here). This locks the determinism seam: miss -> enqueue
// (deduped) -> drain -> the variant is warm and the draw self-heals. The zero-record-
// thread-compiles property is runtime-verified in the editor on a cold load.
TEST_F(MaterialPipelineCompilationTest, PublishGate_VariantMissEnqueuesAsync_DrainWarmsCache)
{
    MaterialDocument doc{};
    doc.materialName = "PublishGatePBR";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid())
        << "base pipeline must be published before a variant can be compiled";
    const Material* other = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(other, nullptr);

    auto& variants = m_rs->Materials().Variants();

    // A vertex-flags combo the guessed prewarm enumeration does not cover (adds
    // vertex color) -> a genuine cold variant key the record path would previously
    // have self-heal-compiled inline on the record thread.
    const VertexAttributeFlags vf = VertexAttributeFlags::StandardMesh
        | VertexAttributeFlags::HasColor;
    const MaterialKeyword passKw = MaterialKeyword::ForwardPlus
        | MaterialKeyword::Instanced | MaterialKeyword::Shadows;

    // Miss -> enqueue + skip. The variant is NOT compiled inline.
    const auto key = variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, passKw, /*sampleCount=*/1u,
        FrontFace::CounterClockwise);
    EXPECT_FALSE(variants.HasColorVariantForTesting(key))
        << "a missed variant must not be compiled inline on the record path";
    EXPECT_EQ(variants.PendingVariantCompileCount(), 1u);
    EXPECT_TRUE(variants.HasPipelineBuildsInFlight(*mat)) << "a queued miss is in flight";
    EXPECT_FALSE(variants.HasPipelineBuildsInFlight(*other))
        << "another material's draws are not waiting on this compile";

    // Re-request the same key -> deduped (still one pending, still not compiled).
    variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, passKw, 1u,
        FrontFace::CounterClockwise);
    EXPECT_EQ(variants.PendingVariantCompileCount(), 1u)
        << "an in-flight variant compile must not be requested twice";

    const uint64_t before = variants.AsyncVariantCompileCount();

    // Drain: with the engine job system down (headless) the compile runs inline.
    variants.SubmitPendingVariantCompiles();

    EXPECT_EQ(variants.PendingVariantCompileCount(), 0u) << "drain must consume the queue";
    EXPECT_FALSE(variants.HasColorVariantForTesting(key))
        << "the compiled unit is queued for the serial apply — the compile path "
           "must not write the live cache node";
    EXPECT_EQ(variants.AsyncVariantCompileCount(), before + 1u)
        << "exactly one variant compile ran for the deduped request";
    EXPECT_TRUE(variants.HasPipelineBuildsInFlight(*mat))
        << "a compiled variant stays in flight until the serial apply publishes it — "
           "draws still skip it, so render-once consumers must keep waiting";

    variants.ApplyPendingVariantPublishes();
    EXPECT_TRUE(variants.HasColorVariantForTesting(key))
        << "the serial apply must warm the variant cache so the draw self-heals";
    EXPECT_FALSE(variants.HasPipelineBuildsInFlight(*mat)) << "the publish ends the in-flight window";
}

// A draw that finds a backend pipeline cold records its material as waiting on
// the device's build (EnsureConcreteWarm). Render-once consumers (model
// thumbnails) poll HasPipelineBuildsInFlight and must not render inside that
// window. Once the build settles, Warm or Failed, the material no longer waits
// and nothing is queued again; the per-frame prune then forgets the wait, so a
// later hot-reload invalidation does not request a pipeline no draw uses.
TEST_F(MaterialPipelineCompilationTest, ConcreteWarm_MaterialWaitsWhileItsBackendBuildIsPending)
{
    MaterialDocument doc{};
    doc.materialName = "ConcreteWarmPBR";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    const GraphicsPipelineId id = mat->GetGraphicsPipelineId();
    ASSERT_TRUE(id.IsValid()) << "base pipeline must be published before its concrete build is asked for";
    const Material* other = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(other, nullptr);

    auto& variants = m_rs->Materials().Variants();
    HeldPipelineBuilds held(*m_device);
    PipelineFormatKey fk{};
    fk.ColorCount = 1;
    fk.ColorFormats[0] = TextureFormat::R16G16B16A16_FLOAT;
    fk.DepthFormat = TextureFormat::D32_FLOAT;
    ASSERT_FALSE(m_device->TryGetWarmGraphicsPipeline(id, fk).IsValid()) << "the pass format must start cold";
    ASSERT_FALSE(variants.HasPipelineBuildsInFlight(*mat));

    EXPECT_FALSE(variants.EnsureConcreteWarm(*mat, id, fk)) << "the build ran on the declaring thread";
    ASSERT_EQ(held.Jobs.size(), 1u);
    EXPECT_TRUE(variants.HasPipelineBuildsInFlight(*mat)) << "a material whose pipeline is building does not wait";
    EXPECT_FALSE(variants.HasPipelineBuildsInFlight(*other)) << "another material's draws are not waiting on this build";
    variants.PruneSettledConcreteWarmWaits();
    EXPECT_TRUE(variants.HasPipelineBuildsInFlight(*mat)) << "the prune dropped a wait whose build is still pending";

    held.RunAll();
    EXPECT_FALSE(variants.HasPipelineBuildsInFlight(*mat)) << "the landed build kept the material waiting";
    EXPECT_TRUE(variants.EnsureConcreteWarm(*mat, id, fk));

    // An id with no interned desc: its build fails, once.
    const GraphicsPipelineId neverInterned{std::numeric_limits<uint32_t>::max()};
    EXPECT_FALSE(variants.EnsureConcreteWarm(*mat, neverInterned, fk));
    EXPECT_TRUE(variants.HasPipelineBuildsInFlight(*mat));
    held.RunAll();
    EXPECT_FALSE(variants.HasPipelineBuildsInFlight(*mat)) << "a failed build kept the material waiting";
    EXPECT_FALSE(variants.EnsureConcreteWarm(*mat, neverInterned, fk));
    EXPECT_TRUE(held.Jobs.empty()) << "a failed build was queued again";

    variants.PruneSettledConcreteWarmWaits();
    m_device->GetMutablePipelineCache().Clear(); // hot reload
    EXPECT_FALSE(variants.HasPipelineBuildsInFlight(*mat)) << "a settled wait outlived the prune";
    EXPECT_TRUE(held.Jobs.empty()) << "the invalidation re-requested a pipeline no draw uses";
}

// Depth mirror of the publish-gate success path: a depth-variant miss enqueues,
// dedupes, and the drain warms the depth cache (RunDepthVariantCompile body).
TEST_F(MaterialPipelineCompilationTest, PublishGate_DepthVariantMissEnqueuesAsync_DrainWarmsCache)
{
    MaterialDocument doc{};
    doc.materialName = "PublishGateDepthPBR";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    auto& variants = m_rs->Materials().Variants();
    const VertexAttributeFlags vf = VertexAttributeFlags::StandardMesh
        | VertexAttributeFlags::HasColor;
    const MaterialKeyword depthKw = MaterialKeyword::Instanced;

    const auto key = variants.EnqueueDepthVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, depthKw, /*sampleCount=*/1u,
        /*depthBiasEnable=*/false, /*depthClampEnable=*/false, FrontFace::CounterClockwise);
    EXPECT_FALSE(variants.HasDepthVariantForTesting(key));
    EXPECT_EQ(variants.PendingVariantCompileCount(), 1u);

    variants.EnqueueDepthVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, depthKw, 1u, false, false,
        FrontFace::CounterClockwise);
    EXPECT_EQ(variants.PendingVariantCompileCount(), 1u) << "depth in-flight compile deduped";

    const uint64_t before = variants.AsyncVariantCompileCount();
    variants.SubmitPendingVariantCompiles();

    EXPECT_EQ(variants.PendingVariantCompileCount(), 0u);
    EXPECT_FALSE(variants.HasDepthVariantForTesting(key))
        << "the compiled depth unit is queued for the serial apply";
    EXPECT_EQ(variants.AsyncVariantCompileCount(), before + 1u);

    variants.ApplyPendingVariantPublishes();
    EXPECT_TRUE(variants.HasDepthVariantForTesting(key))
        << "the serial apply must warm the depth variant cache";
}

// Each variant publish notifies the registered listener with the owning
// material's GUID (color and depth paths both). Render-once consumers
// (thumbnails) re-render on this signal: the publish-gate skips draws whose
// variant is still compiling, which self-heals for per-frame scene meshes but
// would leave a cached thumbnail blank forever.
TEST_F(MaterialPipelineCompilationTest, PublishGate_VariantPublishNotifiesListener)
{
    MaterialDocument doc{};
    doc.materialName = "PublishListenerPBR";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    auto& variants = m_rs->Materials().Variants();

    // The listener fires from the serial publish apply (after the unit is in the
    // cache); guard the vector anyway so the test matches subscriber contracts.
    std::mutex publishedMutex;
    std::vector<GUID> published;
    EventSubscription publishedSub = variants.VariantPublished.Subscribe(
        [&](const GUID& publishedGuid)
        {
            std::lock_guard lock(publishedMutex);
            published.push_back(publishedGuid);
        });

    const VertexAttributeFlags vf = VertexAttributeFlags::StandardMesh
        | VertexAttributeFlags::HasColor;
    variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList,
        MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced | MaterialKeyword::Shadows,
        /*sampleCount=*/1u, FrontFace::CounterClockwise);
    {
        std::lock_guard lock(publishedMutex);
        EXPECT_TRUE(published.empty()) << "enqueue alone must not publish";
    }

    variants.SubmitPendingVariantCompiles();
    {
        std::lock_guard lock(publishedMutex);
        EXPECT_TRUE(published.empty())
            << "the event fires when the unit lands in the cache (serial apply), "
               "not when the compile finishes — a listener reacting earlier would "
               "still miss the variant";
    }
    variants.ApplyPendingVariantPublishes();
    {
        std::lock_guard lock(publishedMutex);
        ASSERT_EQ(published.size(), 1u) << "color variant publish must notify once";
        EXPECT_EQ(published[0], guid);
    }

    variants.EnqueueDepthVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, MaterialKeyword::Instanced,
        /*sampleCount=*/1u, /*depthBiasEnable=*/false, /*depthClampEnable=*/false,
        FrontFace::CounterClockwise);
    variants.SubmitPendingVariantCompiles();
    variants.ApplyPendingVariantPublishes();
    {
        std::lock_guard lock(publishedMutex);
        ASSERT_EQ(published.size(), 2u) << "depth variant publish must notify too";
        EXPECT_EQ(published[1], guid);
    }

    // The subscription must not outlive `published`; drop it before TearDown.
    publishedSub.Reset();
}

// Failure-path marker clearing (RAII guard): a depth variant whose surface can't
// compile must clear its in-flight marker on the RunDepthVariantCompile failure
// exit — otherwise the key is dedup-blocked forever (permanently dark draws). Depth
// has no base-invalid short-circuit, so a bogus surface reaches RunDepthVariantCompile.
TEST_F(MaterialPipelineCompilationTest, PublishGate_DepthVariantCompileFailure_ClearsMarker)
{
    MaterialDocument doc{};
    doc.materialName = "PublishGateBogusDepth";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/__ge_nonexistent_bogus__.glsl"; // never resolves -> compile fails

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr); // registered even though the base compile fails

    auto& variants = m_rs->Materials().Variants();
    const VertexAttributeFlags vf = VertexAttributeFlags::StandardMesh;
    const MaterialKeyword depthKw = MaterialKeyword::Instanced;

    const auto key = variants.EnqueueDepthVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, depthKw, /*sampleCount=*/1u,
        /*depthBiasEnable=*/false, /*depthClampEnable=*/false, FrontFace::CounterClockwise);
    EXPECT_EQ(variants.PendingVariantCompileCount(), 1u);

    const uint64_t before = variants.AsyncVariantCompileCount();
    variants.SubmitPendingVariantCompiles(); // inline -> RunDepthVariantCompile fails

    EXPECT_EQ(variants.PendingVariantCompileCount(), 0u) << "drain consumes the queue";
    EXPECT_FALSE(variants.HasDepthVariantForTesting(key)) << "a failed compile warms nothing";
    EXPECT_EQ(variants.AsyncVariantCompileCount(), before) << "a failed compile is not counted";

    // The marker MUST have cleared on the failure exit — a re-enqueue of the same
    // key must NOT be dedup-blocked (else the draw is permanently dark).
    variants.EnqueueDepthVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, depthKw, 1u, false, false,
        FrontFace::CounterClockwise);
    EXPECT_EQ(variants.PendingVariantCompileCount(), 1u)
        << "failure-path marker clear must allow re-enqueue (no permanent wedge)";
}

// A variant compile still running on a worker when its material unregisters
// queues its publish AFTER the pre-unregister purge, and the serial apply runs
// AFTER the eviction drain — so an unguarded apply would resurrect a cache row
// keyed on the freed Material* (an unreachable leak; a wrong-pipeline
// stale-serve if the address is reused). The apply must drop any record whose
// GUID no longer resolves in the registry, exactly like the base-pipeline
// publish drain: look up by GUID, never trust the captured pointer.
TEST_F(MaterialPipelineCompilationTest, PublishGate_LatePublishForUnregisteredMaterialDropped)
{
    MaterialDocument doc{};
    doc.materialName = "LatePublishGhost";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());
    const auto realPid = mat->GetGraphicsPipelineId();
    const uint32_t version = mat->GetVersion();

    auto& variants = m_rs->Materials().Variants();
    const PipelineVariantCache::InstancedVariantKey key{
        mat, VertexAttributeFlags::StandardMesh | VertexAttributeFlags::HasColor,
        /*SampleCount=*/1u,
        MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced | MaterialKeyword::Shadows,
        PrimitiveTopology::TriangleList};

    // Unregister BEFORE the publish is queued: the pre-unregister purge finds
    // nothing, exactly like a worker whose publish lands after the purge ran.
    // `mat` dangles from here on — the key carries it as an opaque address only.
    m_rs->Materials().Registry().Unregister(guid);
    ASSERT_EQ(m_rs->Materials().Registry().Find(guid), nullptr);

    variants.EnqueueColorVariantPublishForTesting(key, realPid, version, guid);

    std::mutex publishedMutex;
    std::vector<GUID> published;
    EventSubscription publishedSub = variants.VariantPublished.Subscribe(
        [&](const GUID& publishedGuid)
        {
            std::lock_guard lock(publishedMutex);
            published.push_back(publishedGuid);
        });

    // Mirror BeginFrame's ordering: the unregister's eviction drains first,
    // then the publishes apply — the window the guard exists for.
    variants.DrainPendingEvictions();
    variants.ApplyPendingVariantPublishes();

    EXPECT_FALSE(variants.HasColorVariantForTesting(key))
        << "a late publish for an unregistered material must not re-insert a "
           "cache row keyed on the freed Material*";
    {
        std::lock_guard lock(publishedMutex);
        EXPECT_TRUE(published.empty())
            << "a dropped publish must not notify listeners for a dead GUID";
    }
    publishedSub.Reset();
}

// A variant republish (a material edit while the old unit is on screen) must
// not rewrite the live cache node outside the serial apply window: record
// threads read entry fields and hold stale-serve pointers without the lock, so
// a compile-side write would race them for the whole rebuild window. The
// compiled unit stays queued until ApplyPendingVariantPublishes runs; the
// apply then serves the new unit and retains the replaced one whole —
// pipeline, meta and set layouts — for coherent stale-serve.
TEST_F(MaterialPipelineCompilationTest, VariantRepublish_RewritesLiveEntryOnlyInSerialApply)
{
    MaterialDocument doc{};
    doc.materialName = "RepublishSerial";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    auto& variants = m_rs->Materials().Variants();
    const VertexAttributeFlags vf = VertexAttributeFlags::StandardMesh;
    const MaterialKeyword passKw = MaterialKeyword::ForwardPlus
        | MaterialKeyword::Instanced | MaterialKeyword::Shadows;

    // First publication: compile + apply — the entry serves unit A.
    const auto key = variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, passKw, /*sampleCount=*/1u,
        FrontFace::CounterClockwise);
    variants.SubmitPendingVariantCompiles();
    variants.ApplyPendingVariantPublishes();
    const auto unitA = variants.SnapshotColorVariantForTesting(key);
    ASSERT_TRUE(unitA.CurrentPipelineId.IsValid());

    // Hot-reload edit: re-register with a document whose variant compiles to a
    // DIFFERENT pipeline (in-place re-registration recompiles the base and
    // bumps the version); the record path's re-miss enqueues the same key at
    // the new version.
    MaterialDocument edited = doc;
    edited.lightingModel = "Unlit";
    edited.surfaceShader = "Surfaces/unlit_solid.glsl";
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, edited), mat)
        << "re-registration must refresh the material in place";
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, passKw, 1u,
        FrontFace::CounterClockwise);
    variants.SubmitPendingVariantCompiles(); // inline compile ran; publish queued

    const auto beforeApply = variants.SnapshotColorVariantForTesting(key);
    EXPECT_EQ(beforeApply.CurrentPipelineId.Value, unitA.CurrentPipelineId.Value)
        << "the compile path must not rewrite the live entry outside the serial window";
    EXPECT_EQ(beforeApply.MaterialVersion, unitA.MaterialVersion)
        << "the queued republish must stay invisible to record threads until the apply";

    variants.ApplyPendingVariantPublishes();
    const auto afterApply = variants.SnapshotColorVariantForTesting(key);
    EXPECT_NE(afterApply.CurrentPipelineId.Value, unitA.CurrentPipelineId.Value)
        << "the serial apply must publish the recompiled unit";
    EXPECT_GT(afterApply.MaterialVersion, unitA.MaterialVersion);
    EXPECT_EQ(afterApply.PreviousPipelineId.Value, unitA.CurrentPipelineId.Value)
        << "the replaced unit must be retained whole for coherent stale-serve";
}

// T19, the observable half: through a recompile window the cache serves the
// retained previous unit for the passes whose new pipeline is still cold, so
// the generation a draw renders is a property of the UNIT it bound, not of
// the entry. The previous unit must therefore keep naming the generation it
// was compiled against after the republish — otherwise a colour draw served
// from it and a depth draw served from the new unit would report the same
// generation and their disagreement would be invisible.
TEST_F(MaterialPipelineCompilationTest, VariantRepublish_PreviousUnitKeepsItsOwnGeneration)
{
    MaterialDocument doc{};
    doc.materialName = "RepublishGeneration";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->GetGraphicsPipelineId().IsValid());

    auto& variants = m_rs->Materials().Variants();
    const VertexAttributeFlags vf = VertexAttributeFlags::StandardMesh;
    const MaterialKeyword passKw = MaterialKeyword::ForwardPlus
        | MaterialKeyword::Instanced | MaterialKeyword::Shadows;

    const auto key = variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, passKw, /*sampleCount=*/1u,
        FrontFace::CounterClockwise);
    variants.SubmitPendingVariantCompiles();
    variants.ApplyPendingVariantPublishes();
    const auto unitA = variants.SnapshotColorVariantForTesting(key);
    ASSERT_TRUE(unitA.CurrentPipelineId.IsValid());
    EXPECT_EQ(unitA.MaterialVersion, mat->GetVersion())
        << "the first unit is compiled against the live version";
    EXPECT_EQ(unitA.PreviousMaterialVersion, 0u) << "no previous unit before a republish";

    MaterialDocument edited = doc;
    edited.lightingModel = "Unlit";
    edited.surfaceShader = "Surfaces/unlit_solid.glsl";
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guid, edited), mat);
    ASSERT_GT(mat->GetVersion(), unitA.MaterialVersion) << "precondition: the edit recompiled";

    variants.EnqueueColorVariantCompileForTesting(
        *mat, vf, PrimitiveTopology::TriangleList, passKw, 1u, FrontFace::CounterClockwise);
    variants.SubmitPendingVariantCompiles();
    variants.ApplyPendingVariantPublishes();

    const auto afterApply = variants.SnapshotColorVariantForTesting(key);
    ASSERT_EQ(afterApply.PreviousPipelineId.Value, unitA.CurrentPipelineId.Value);
    EXPECT_EQ(afterApply.MaterialVersion, mat->GetVersion())
        << "the new unit names the generation it was compiled against";
    EXPECT_EQ(afterApply.PreviousMaterialVersion, unitA.MaterialVersion)
        << "the retained unit still names ITS generation, not the replacement's";
    EXPECT_NE(afterApply.PreviousMaterialVersion, afterApply.MaterialVersion)
        << "the two units a recompile window can serve are distinguishable";
}

// ---- Runtime Mask demotion (alpha source provably cannot discard) ----
//
// RegisterMaterialFromDocument demotes a Mask document whose opacity is
// provably constant 1 to Opaque on its registration-local copy: the runtime
// material loses AlphaTest, while the authored document (passed by const&)
// is never mutated. Every gate failure keeps the authored Mask.

namespace {
MaterialDocument MakeProvablyOpaqueMaskDoc()
{
    MaterialDocument doc{};
    doc.materialName = "MaskDemotion";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.ignoreVertexColor = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
    doc.properties["alphaCutoff"] = 0.5f;
    return doc;
}
} // namespace

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_ProvablyOpaqueRegistersAsOpaque)
{
    const MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Opaque);
    // The demotion must reach the variant key too, or depth/shadow paths
    // would still compile the discard-mode variant.
    EXPECT_FALSE(HasKeyword(mat->GetVariantKey().materialKeywords, MaterialKeyword::AlphaTest));
    // The authored document is untouched — registration demotes a local copy.
    EXPECT_EQ(doc.alphaMode, MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_ReRegistrationStaysDemoted)
{
    const MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Opaque);
    Material* again = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    EXPECT_EQ(again, mat);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Opaque);
}

// Review blocker: a document carrying BOTH a builtin surfaceShader and a
// surface graph compiles the GRAPH (ResolveSurfaceShaderFromGraph overwrites
// surfaceShader), and graph opacity can be procedural. Coexistence must
// disqualify demotion regardless of what surfaceShader says.
TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_SurfaceGraphCoexistenceKeepsMask)
{
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.surfaceGraph = "Graphs/procedural_opacity.glsl";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
    EXPECT_TRUE(HasKeyword(mat->GetVariantKey().materialKeywords, MaterialKeyword::AlphaTest));
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_SurfaceGraphGuidCoexistenceKeepsMask)
{
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.surfaceGraphGuid = "12345678-1234-1234-1234-123456789abc";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_CustomSurfaceShaderKeepsMask)
{
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.surfaceShader = "Surfaces/my_custom_surface.glsl";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_VertexColorVariantKeepsMask)
{
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.ignoreVertexColor = false;
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_AttenuatingBaseColorAlphaKeepsMask)
{
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.5f};
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_OpacityPropertyKeepsMask)
{
    // "opacity" overwrites baseColor.a at registration
    // (SyncMaterialOpacityFromDocument), so it attenuates the alpha test.
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.properties["opacity"] = 0.5f;
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_NearOneCutoffKeepsMask)
{
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.properties["alphaCutoff"] = 0.995f;
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

TEST_F(MaterialBridgeIntegrationTest, MaskDemotion_UnresolvableAlbedoTextureKeepsMask)
{
    // An albedo slot that cannot be resolved to a probe-able file must keep
    // the authored Mask. In this headless harness (no EngineCore/AssetManager)
    // EVERY texture ref is unresolvable; the embedded-style ref form is used
    // because the registration bind loop's headless fallback
    // (TextureService::BindMaterialTextureRef -> GetOrUpload) AVs on unknown
    // raw GUIDs here — a pre-existing harness hazard unrelated to demotion.
    MaterialDocument doc = MakeProvablyOpaqueMaskDoc();
    doc.textures["albedoMap"] = "__embedded__:0";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
}

// ---- "This material can never draw" registration warning ----
//
// A Mask material whose opacity cannot reach its own cutoff discards every
// fragment. Registration reports it and NEVER repairs it: an automatic clamp or
// demotion would make invisible-and-free content indistinguishable from content
// that works, which is exactly how this class of bug survives.

namespace {
// The values of PolygonElven_Emissive_Fade_01 (SyntyDemos ElvenRealm), the real
// asset that renders nothing: authored tint alpha 18/255 against a 0.5 clip.
MaterialDocument MakeAlwaysDiscardingMaskDoc()
{
    MaterialDocument doc{};
    doc.materialName = "PolygonElven_Emissive_Fade_01";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.ignoreVertexColor = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.0705882f};
    doc.properties["alphaCutoff"] = 0.5f;
    return doc;
}

// Counts registration warnings that name a material as unable to draw.
class CannotDrawWarningCounter
{
  public:
    CannotDrawWarningCounter()
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        sink->RegisterCallback([count = m_Count](const Logger::LogMessage& msg) {
            if (msg.Message.find("can never draw") != Logger::String::npos)
                count->fetch_add(1);
        });
        Logger::Log::AddSink(std::move(sink));
    }

    int Count() const
    {
        Logger::Log::Flush();
        return m_Count->load();
    }

  private:
    std::shared_ptr<std::atomic<int>> m_Count = std::make_shared<std::atomic<int>>(0);
};
} // namespace

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_MaskBelowCutoffWarns)
{
    CannotDrawWarningCounter warnings;
    const MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(warnings.Count(), 1);
    // Warn, never mutate: the material must register exactly as authored so the
    // defect stays visible to the inspector and to the next reader of the asset.
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask);
    EXPECT_FLOAT_EQ(mat->GetFloat(HashStringId("alphaCutoff")), 0.5f);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_ExtendedSurfaceMaskBelowCutoffWarns)
{
    CannotDrawWarningCounter warnings;
    // standard_pbr_extended derives opacity exactly like standard_pbr
    // (albedo.a * uBaseColor.a * vertexColor.a), so the bound holds there too;
    // dropping it from the analysable set would silence real cannot-draw
    // content authored against the extended surface.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.surfaceShader = "Surfaces/standard_pbr_extended.glsl";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(warnings.Count(), 1);
    EXPECT_EQ(mat->GetAlphaMode(), MaterialAlphaMode::Mask) << "warn, never mutate";
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_WarnsOncePerMaterialNotPerRegistration)
{
    CannotDrawWarningCounter warnings;
    const MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    const GUID guid = GUID::Generate();
    // Runtime materials (particle emitters) re-register every frame; a per-frame
    // duplicate of this line would drown the log it is supposed to surface in.
    for (int i = 0; i < 5; ++i)
        ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), nullptr);
    EXPECT_EQ(warnings.Count(), 1);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_ReportsAgainAfterRepairAndRelapse)
{
    CannotDrawWarningCounter warnings;
    const GUID guid = GUID::Generate();
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(
                  guid, MakeAlwaysDiscardingMaskDoc()), nullptr);
    ASSERT_EQ(warnings.Count(), 1);

    MaterialDocument fixed = MakeAlwaysDiscardingMaskDoc();
    fixed.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, fixed), nullptr);
    EXPECT_EQ(warnings.Count(), 1) << "a repaired material must not re-report";

    // Relapse: the dedup entry has to clear on recovery, or the second breakage
    // is silent forever.
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(
                  guid, MakeAlwaysDiscardingMaskDoc()), nullptr);
    EXPECT_EQ(warnings.Count(), 2);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_VertexColorMaterialIsNotProvable)
{
    CannotDrawWarningCounter warnings;
    // Vertex colour is R32G32B32A32_FLOAT and therefore unbounded: without
    // ignoreVertexColor a mesh carrying COLOR_0 can multiply opacity back above
    // the cutoff, so nothing is proven and nothing may be reported.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.ignoreVertexColor = false;
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_CustomSurfaceShaderIsNotProvable)
{
    CannotDrawWarningCounter warnings;
    // A foreign surface may derive opacity from anything, so albedo.a *
    // uBaseColor.a stops bounding the product.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.surfaceShader = "Surfaces/my_custom_surface.glsl";
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_SurfaceGraphGuidMaterialIsNotProvable)
{
    CannotDrawWarningCounter warnings;
    // A surface graph overwrites surfaceShader at resolve and may derive
    // opacity from anything, so whatever surfaceShader says, nothing is proven
    // and nothing may be reported.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.surfaceGraphGuid = "12345678-1234-1234-1234-123456789abc";
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_NonMaskModesNeverWarn)
{
    CannotDrawWarningCounter warnings;
    for (const MaterialAlphaMode mode :
         {MaterialAlphaMode::Opaque, MaterialAlphaMode::Blend})
    {
        MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
        doc.alphaMode = mode;
        ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    }
    // No alpha test is compiled in, so a low alpha is a legitimate authoring
    // choice (Blend fades; Opaque ignores alpha outright).
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_ShadowOnlyLightingModelIsSilent)
{
    CannotDrawWarningCounter warnings;
    // ShadowOnly forces the EFFECTIVE alpha mode to Blend
    // (GetEffectiveMaterialAlphaMode), so no AlphaTest keyword and no discard
    // are ever compiled; the authored alphaMode=Mask must not be consulted
    // directly, or a working shadow catcher gets reported as unable to draw.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.lightingModel = "ShadowOnly";
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_OpacityAtCutoffDrawsAndIsSilent)
{
    CannotDrawWarningCounter warnings;
    // The fragment test is strict `<`, so opacity exactly AT the cutoff draws.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.5f};
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_NonFiniteCutoffIsNotProvable)
{
    CannotDrawWarningCounter warnings;
    // Every ordered comparison against NaN is false, so an unchecked NaN would
    // read as "draws fine" by accident rather than by decision.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.properties["alphaCutoff"] = std::numeric_limits<float>::quiet_NaN();
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);
}

TEST_F(MaterialBridgeIntegrationTest, CannotDraw_CutoffIsClampedLikeTheShader)
{
    CannotDrawWarningCounter warnings;
    // The fragment stage tests against clamp(alphaCutoff, 0, 1), so an authored
    // 2.0 tests at 1.0 and an opacity of 1.5 still draws. Comparing against the
    // raw 2.0 would report a material that renders perfectly well.
    MaterialDocument doc = MakeAlwaysDiscardingMaskDoc();
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
    doc.properties["opacity"] = 1.5f;
    doc.properties["alphaCutoff"] = 2.0f;
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc), nullptr);
    EXPECT_EQ(warnings.Count(), 0);

    // Below the clamped cutoff, the same authored 2.0 does report.
    MaterialDocument failing = doc;
    failing.properties["opacity"] = 0.25f;
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), failing), nullptr);
    EXPECT_EQ(warnings.Count(), 1);
}

// ---- Color-class map staleness across re-registration ----

TEST_F(MaterialBridgeIntegrationTest, ColorClass_ReRegistrationRetiresAStaleMerge)
{
    auto makeMergeableDoc = [](const char* name) {
        MaterialDocument doc{};
        doc.materialName = name;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/standard_pbr.glsl";
        doc.alphaMode = MaterialAlphaMode::Opaque;
        doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
        return doc;
    };

    const GUID guidA = GUID::Generate();
    const GUID guidB = GUID::Generate();
    Material* matA = m_rs->Materials().RegisterMaterialFromDocument(guidA, makeMergeableDoc("A"));
    Material* matB = m_rs->Materials().RegisterMaterialFromDocument(guidB, makeMergeableDoc("B"));
    ASSERT_NE(matA, nullptr);
    ASSERT_NE(matB, nullptr);
    const uint32_t idxA = matA->GetGpuSceneMaterialIndex();
    const uint32_t idxB = matB->GetGpuSceneMaterialIndex();

    {
        const std::span<const uint32_t> classes = m_rs->Materials().MaterialColorClassSpan();
        ASSERT_GT(classes.size(), std::max(idxA, idxB));
        ASSERT_EQ(classes[idxA], classes[idxB])
            << "identical PSO signatures must share one color class";
        ASSERT_NE(classes[idxA], idxA) << "a merged material does not keep its identity row";
    }

    // Flipping alphaMode makes A material-dependent, so it must fall out of the
    // merge. The cache is keyed on the index-mapping generation, which a
    // re-registration does not move, so without an explicit invalidation the span
    // still reports A as merged and the batch binds B's pipeline for A's draws.
    MaterialDocument masked = makeMergeableDoc("A");
    masked.alphaMode = MaterialAlphaMode::Mask;
    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guidA, masked), matA);
    ASSERT_EQ(matA->GetAlphaMode(), MaterialAlphaMode::Mask)
        << "the flip must reach the runtime material, or this asserts nothing";
    {
        const std::span<const uint32_t> classes = m_rs->Materials().MaterialColorClassSpan();
        EXPECT_EQ(classes[idxA], idxA)
            << "stale color class: Mask is material-dependent and owns its identity row";
    }
}

TEST_F(MaterialBridgeIntegrationTest, ColorClass_IgnoreVertexColorFlipRetiresAStaleMerge)
{
    // ignoreVertexColor rides the class signature (it strips HasColor, so a
    // merged batch would bind the representative's vertex-colour variant) but is
    // NOT one of the re-registration's pipeline-affecting fields, so an
    // invalidation derived from that flag set would miss this case entirely.
    auto makeDoc = [](const char* name, bool ignoreVertexColor) {
        MaterialDocument doc{};
        doc.materialName = name;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/standard_pbr.glsl";
        doc.alphaMode = MaterialAlphaMode::Opaque;
        doc.ignoreVertexColor = ignoreVertexColor;
        doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
        return doc;
    };

    const GUID guidA = GUID::Generate();
    const GUID guidB = GUID::Generate();
    Material* matA = m_rs->Materials().RegisterMaterialFromDocument(guidA, makeDoc("A", false));
    Material* matB = m_rs->Materials().RegisterMaterialFromDocument(guidB, makeDoc("B", false));
    ASSERT_NE(matA, nullptr);
    ASSERT_NE(matB, nullptr);
    const uint32_t idxA = matA->GetGpuSceneMaterialIndex();
    const uint32_t idxB = matB->GetGpuSceneMaterialIndex();

    {
        const std::span<const uint32_t> classes = m_rs->Materials().MaterialColorClassSpan();
        ASSERT_GT(classes.size(), std::max(idxA, idxB));
        ASSERT_EQ(classes[idxA], classes[idxB]);
    }

    ASSERT_EQ(m_rs->Materials().RegisterMaterialFromDocument(guidA, makeDoc("A", true)), matA);
    ASSERT_TRUE(matA->IgnoresVertexColor())
        << "the flip must reach the runtime material, or this asserts nothing";
    {
        const std::span<const uint32_t> classes = m_rs->Materials().MaterialColorClassSpan();
        EXPECT_NE(classes[idxA], classes[idxB])
            << "stale color class: the two materials no longer share a PSO signature";
    }
}

// ---- Unknown texture keys in .material documents ----
//
// A hand-edited document key that names no texture slot (neither a surface
// @texture declaration nor a well-known ladder name) must never abort the
// editor. The document paths skip the texture with one warning naming the key
// and the valid candidates; the material registers and renders with its
// remaining/default textures. Live crash shape this locks down: a document
// with textures{"baseColorMap"} on Surfaces/standard_pbr.glsl aborted at
// Material::SetBindlessTextureIndex ("Unknown texture slot name") on the main
// thread the moment the texture decode completed.

namespace
{
// Captures warnings about unknown document texture keys (same idiom as
// CannotDrawWarningCounter below: the sink outlives the capture, so state is
// shared_ptr-owned).
class UnknownKeyWarningCapture
{
  public:
    explicit UnknownKeyWarningCapture(const GUID& materialGuid)
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        sink->RegisterCallback([state = m_State, materialTag = "(" + materialGuid.ToString() + ")"](const Logger::LogMessage& msg) {
            if (msg.Message.find("unknown texture key") != Logger::String::npos &&
                msg.Message.find(materialTag) != Logger::String::npos)
            {
                std::lock_guard lock(state->Mutex);
                state->Messages.push_back(msg.Message);
            }
        });
        Logger::Log::AddSink(std::move(sink));
    }

    int Count() const
    {
        Logger::Log::Flush();
        std::lock_guard lock(m_State->Mutex);
        return static_cast<int>(m_State->Messages.size());
    }

    Logger::String First() const
    {
        Logger::Log::Flush();
        std::lock_guard lock(m_State->Mutex);
        return m_State->Messages.empty() ? Logger::String{} : m_State->Messages.front();
    }

  private:
    struct State
    {
        std::mutex Mutex;
        std::vector<Logger::String> Messages;
    };
    std::shared_ptr<State> m_State = std::make_shared<State>();
};
} // namespace

TEST_F(MaterialBridgeIntegrationTest, UnknownTextureKey_RegistersSkipsAndWarnsOnce)
{
    const GUID guid = GUID::Generate();
    UnknownKeyWarningCapture warnings(guid);
    MaterialDocument doc{};
    doc.materialName = "UVGateChecker";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.textures["baseColorMap"] = GUID::Generate().ToString();

    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr) << "the material itself must register";

    GUID tracked{};
    EXPECT_FALSE(m_rs->Textures().MaterialTextureRefForTesting(guid, "baseColorMap"_sid, tracked))
        << "an unknown key must not track a texture ref — the decode-complete "
           "rebind would replay the bad name";
    ASSERT_EQ(warnings.Count(), 1);
    // The warning states the fix: it names the key and the valid candidates.
    EXPECT_NE(warnings.First().find("baseColorMap"), Logger::String::npos);
    EXPECT_NE(warnings.First().find("albedoMap"), Logger::String::npos);

    // Runtime materials (particle emitters) re-register every frame; the
    // report must stay one line per material+key, not one per application.
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, doc), nullptr);
    EXPECT_EQ(warnings.Count(), 1);
}

TEST_F(MaterialBridgeIntegrationTest, UnknownTextureKey_WellKnownKeysStillBindWithoutWarning)
{
    const GUID guid = GUID::Generate();
    UnknownKeyWarningCapture warnings(guid);
    MaterialDocument doc{};
    doc.materialName = "WellKnownControl";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.textures["albedoMap"] = "__embedded__:0"; // headless-safe live ref

    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    const uint32_t defaultIdx = mat->GetBindlessTextureIndex("albedoMap"_sid);

    // The decode-complete route a real texture load takes.
    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "WellKnownBind";
    const TextureHandle tex = m_device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());
    m_rs->Textures().BindMaterialTexture(mat, "albedoMap"_sid, tex);

    EXPECT_EQ(mat->GetTexture("albedoMap"_sid), tex);
    EXPECT_NE(mat->GetBindlessTextureIndex("albedoMap"_sid), defaultIdx);
    EXPECT_EQ(warnings.Count(), 0);

    m_rs->Textures().DiscardTextureDeferred(tex);
    m_rs->Textures().FlushPendingUploads();
}

TEST_F(MaterialBridgeIntegrationTest, UnknownTextureKey_RelapseAfterDocumentRepairWarnsAgain)
{
    const GUID guid = GUID::Generate();
    UnknownKeyWarningCapture warnings(guid);
    MaterialDocument broken{};
    broken.materialName = "RepairRelapse";
    broken.lightingModel = "StandardPBR";
    broken.surfaceShader = "Surfaces/standard_pbr.glsl";
    broken.textures["baseColorMap"] = "__embedded__:0";

    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, broken), nullptr);
    ASSERT_EQ(warnings.Count(), 1);

    // Repaired by DOCUMENT EDIT: the wrong key is renamed away, so validation
    // never visits it again — the dedup entry must clear on this apply.
    MaterialDocument repaired = broken;
    repaired.textures.clear();
    repaired.textures["albedoMap"] = "__embedded__:0";
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, repaired), nullptr);
    EXPECT_EQ(warnings.Count(), 1) << "a repaired material must not re-report";

    // Relapse to the same wrong key: the second breakage must report, not be
    // silent forever (same contract as the cannot-draw warning).
    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, broken), nullptr);
    EXPECT_EQ(warnings.Count(), 2);
}

TEST_F(MaterialBridgeIntegrationTest, UnknownTextureKey_RelapseThroughInspectorApplyWarnsAgain)
{
    const GUID guid = GUID::Generate();
    UnknownKeyWarningCapture warnings(guid);
    MaterialDocument broken{};
    broken.materialName = "InspectorRepairRelapse";
    broken.lightingModel = "StandardPBR";
    broken.surfaceShader = "Surfaces/standard_pbr.glsl";
    broken.textures["baseColorMap"] = "__embedded__:0";

    ASSERT_NE(m_rs->Materials().RegisterMaterialFromDocument(guid, broken), nullptr);
    ASSERT_EQ(warnings.Count(), 1);

    // The editor texture-assign path re-applies the full document too; the
    // repair must prune there as well or the two appliers disagree.
    MaterialDocument repaired = broken;
    repaired.textures.clear();
    repaired.textures["albedoMap"] = "__embedded__:0";
    m_rs->Textures().UpdateMaterialTextures(guid, repaired);
    EXPECT_EQ(warnings.Count(), 1);

    m_rs->Textures().UpdateMaterialTextures(guid, broken);
    EXPECT_EQ(warnings.Count(), 2);
}

TEST_F(MaterialBridgeIntegrationTest, UnknownTextureKey_UpdateMaterialTexturesDegrades)
{
    const GUID guid = GUID::Generate();
    UnknownKeyWarningCapture warnings(guid);
    MaterialDocument doc{};
    doc.materialName = "InspectorApply";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);

    // The editor texture-assign path re-applies the whole document; a bad key
    // with an empty ref drove the setter directly and aborted.
    MaterialDocument edited = doc;
    edited.textures["baseColorMap"] = "";
    m_rs->Textures().UpdateMaterialTextures(guid, edited);

    EXPECT_EQ(mat->GetTextureBindings().count("baseColorMap"_sid), 0u)
        << "a skipped key must not leave a phantom binding entry";
    EXPECT_EQ(warnings.Count(), 1);
}

TEST_F(MaterialBridgeIntegrationTest, UnknownTextureKey_DecodeCompleteBindDegrades)
{
    MaterialDocument doc{};
    doc.materialName = "DecodeCompleteShape";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID guid = GUID::Generate();
    Material* mat = m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    ASSERT_NE(mat, nullptr);
    uint32_t snapshot[kTextureSlotArraySize];
    std::memcpy(snapshot, mat->GetBindlessTextureIndices(), sizeof snapshot);

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "UnknownSlotBind";
    const TextureHandle tex = m_device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    // The exact call the texture-decode completion makes with a stale or
    // unvalidated slot name. Must be a no-op on the GPU slot table.
    m_rs->Textures().BindMaterialTexture(mat, "baseColorMap"_sid, tex);
    EXPECT_EQ(std::memcmp(snapshot, mat->GetBindlessTextureIndices(), sizeof snapshot), 0)
        << "an unknown slot name must not repaint any bindless slot";

    m_rs->Textures().DiscardTextureDeferred(tex);
    m_rs->Textures().FlushPendingUploads();
}
