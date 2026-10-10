#include <gtest/gtest.h>

#include "../Source/Core/EngineShaderPath.h"
#include "Assets/RenderPipelineAsset.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"
#include "CBTTerrainECS/Systems/RegisterCBTSystems.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Pipeline/ExpressionEvaluator.h"
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Ocean/Systems/RegisterOceanSystems.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainGrass/RegisterTerrainGrass.h"
#include "EngineLogCapture.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
// The registry the fleet gate compiles against is the runtime's: the engine's
// own node types through the function FrameOrchestrator::Initialize calls, and
// the engine modules' node types through the entry points
// EngineCore::EnableRenderingLoop calls. Nothing here names a pass type, so a
// node type registered by the runtime is known to the compiler by construction.
// Plugin registrations are not replayed: no plugin registers a node type, and
// no repo blueprint names one.
RenderPipelineNodeRegistry BuildRegistry()
{
    RenderPipelineNodeRegistry reg;
    Nodes::RegisterEngineNodeTypes(reg);
    TerrainECS::RegisterTerrainPipelineNodes(reg);
    CBTTerrainECS::RegisterCBTPipelineNodes(reg);
    TerrainGrass::RegisterTerrainGrassPipelineNodes(reg);
    Ocean::RegisterOceanPipelineNodes(reg);
    return reg;
}

RenderPipelineBlueprint CompileText(const std::string& json, const RenderPipelineNodeRegistry& reg)
{
    RenderPipelineAsset asset(GUID::Null(), std::filesystem::path("test.rendergraph"));
    Vector<uint8> data(json.begin(), json.end());
    asset.LoadFromData(data);
    RenderPipelineCompiler compiler;
    return compiler.Compile(asset, reg);
}

RenderPipelineBlueprint CompileText(const std::string& json)
{
    return CompileText(json, BuildRegistry());
}

int CountSeverity(const RenderPipelineBlueprint& bp, PipelineIssueSeverity sev)
{
    int n = 0;
    for (const auto& i : bp.issues)
        if (i.severity == sev)
            ++n;
    return n;
}

bool HasIssue(const RenderPipelineBlueprint& bp, PipelineIssueSeverity sev, const std::string& substr)
{
    for (const auto& i : bp.issues)
        if (i.severity == sev && i.message.find(substr) != std::string::npos)
            return true;
    return false;
}

std::string IssuesToString(const RenderPipelineBlueprint& bp)
{
    std::ostringstream os;
    for (const auto& i : bp.issues)
        os << "\n  [" << static_cast<int>(i.severity) << "] " << i.message
           << (i.nodeId.empty() ? "" : (" (node " + i.nodeId + ")"));
    return os.str();
}

// Reproduces MaterializeBuffer's size-expression seeding order (constants ->
// this view's extent -> resource-local variables), so a fixture can assert the
// evaluated byte size a materializer WOULD produce without spinning up a device.
double EvalBufferSize(const RenderPipelineBlueprint& bp, const std::string& name, uint32_t w,
                      uint32_t h)
{
    for (const auto& r : bp.resources)
    {
        if (r.name != name)
            continue;
        const nlohmann::json jr = nlohmann::json::parse(r.resourceJson);
        const auto& sz = jr.at("size");
        std::unordered_map<std::string, double> vars;
        for (const auto& kv : bp.constants)
            vars[kv.first] = kv.second;
        vars["renderWidth"] = static_cast<double>(w);
        vars["renderHeight"] = static_cast<double>(h);
        if (sz.contains("variables"))
            Expr::BuildVariables(sz["variables"], vars, nullptr);
        double out = 0.0;
        Expr::EvalExpression(sz["expression"].get<std::string>(), vars, out, nullptr);
        return out;
    }
    return -1.0;
}
} // namespace

// ── Ref resolution ──────────────────────────────────────────────────────────

TEST(RenderPipelineCompiler, TypoedResourceRefIsErrorWithSuggestion)
{
    const std::string json = R"json({
      "schemaVersion": 2, "pipelineName": "T",
      "resources": { "SceneColor": { "kind": "Texture", "format": "RGBA8_UNORM" } },
      "passes": [
        { "id": "World", "type": "WorldRender", "colorResolveTarget": "SceneColor" },
        { "id": "Copy",  "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
          "inputs": { "uSrc": "SceneColorr" }, "output": "View.Resolve" }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "SceneColorr")) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "did you mean 'SceneColor'"))
        << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, ResolvableRefsProduceNoError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "resources": { "SceneColor": { "kind": "Texture", "format": "RGBA8_UNORM" } },
      "passes": [
        { "id": "Sky",   "type": "SkyRender", "output": "View.Color" },
        { "id": "World", "type": "WorldRender", "colorResolveTarget": "SceneColor" },
        { "id": "Copy",  "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
          "inputs": { "uSrc": "SceneColor", "uSky": "View.Color" }, "output": "View.Resolve" }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, PassthroughInputMustNameAnOwnInput)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "Combine", "type": "FullscreenShader", "shaderPkg": "combine.shaderpkg",
          "inputs": { "uHDR": "View.Color" }, "output": "View.Resolve",
          "passthroughInput": "uMissing" }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "passthroughInput")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, UnknownPipelineOutputRefIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [ { "id": "World", "type": "WorldRender" } ],
      "outputs": { "FinalColor": "View.Nonexistent" }
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "View.Nonexistent")) << IssuesToString(bp);
}

// ── Resource blob enums ───────────────────────────────────────────────────────

TEST(RenderPipelineCompiler, BadTextureFormatIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "resources": { "Foo": { "kind": "Texture", "format": "RGBA8_BOGUS" } },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "unknown texture format")) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "RGBA8_BOGUS")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, BadBufferUsageIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "resources": { "Buf": { "kind": "Buffer", "memoryUsage": "DeviceLocal", "usage": ["Nonsense"],
                              "size": { "bytes": 256 } } },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "unknown buffer usage")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, BadMemoryUsageIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "resources": { "Buf": { "kind": "Buffer", "memoryUsage": "Wherever", "usage": ["Storage"],
                              "size": { "bytes": 256 } } },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "unknown memoryUsage")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, BadSizeExpressionIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "resources": { "Buf": { "kind": "Buffer", "memoryUsage": "DeviceLocal", "usage": ["Storage"],
                              "size": { "expression": "renderWidth * unknownVar" } } },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "size expression")) << IssuesToString(bp);
}

// ── shaderPkg ─────────────────────────────────────────────────────────────────

TEST(RenderPipelineCompiler, EmptyRequiredShaderPkgIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [ { "id": "FX", "type": "FullscreenShader", "output": "View.Resolve" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "requires a non-empty 'shaderPkg'"))
        << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, MissingShaderPkgFileIsError)
{
    // Absolute path to a file that does not exist takes ResolveShaderPath's
    // absolute branch (no throw), so this exercises the "resolver configured,
    // pkg not found -> Error" path without mutating the global resolver.
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "ge_s2_no_such_pkg.shaderpkg";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    std::string json = R"json({
      "schemaVersion": 2,
      "passes": [ { "id": "FX", "type": "FullscreenShader", "shaderPkg": ")json";
    json += missing.generic_string();
    json += R"json(", "output": "View.Resolve" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "could not be loaded")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, ReflectionSkippedWhenResolverUnconfigured)
{
    // A relative pkg name with no shader path resolver configured (this
    // lightweight test exe never configures one) makes ResolveShaderPath throw;
    // the compiler must then skip all pkg checks rather than Error, and produce
    // no reflection warnings.
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "FX", "type": "FullscreenShader", "shaderPkg": "relative.shaderpkg",
          "output": "View.Resolve",
          "pushConstants": { "definitelyNotAMember": 1.0 },
          "inputs": { "uNotABinding": "View.Color" } }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "pushConstant")) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "set-0 binding")) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Error, "could not be loaded")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, BogusPushConstantAndBindingAreWarnings)
{
    // Stage a tiny shaderpkg with one push-constant member and one set-0
    // binding, referenced by an absolute path (bypasses the resolver), then
    // assert the reflection name checks flag the bogus keys as Warnings.
    using namespace GameEngine::Rendering;
    ShaderMeta meta;
    meta.Version = 1;
    PushConstantRangeMeta pc;
    pc.Name = "pc";
    Member goodMember;
    goodMember.Name = "goodKey";
    goodMember.Size = 4;
    pc.Block.Members.push_back(goodMember);
    pc.Block.Size = 4;
    pc.Size = 4;
    meta.PushConstants.push_back(pc);
    DescriptorSetMeta set0;
    set0.Set = 0;
    DescriptorBindingMeta binding;
    binding.Binding = 0;
    binding.Name = "uGood";
    binding.Type = ShaderMetaBindingType::kCombinedImageSampler;
    set0.Bindings.push_back(binding);
    meta.Sets.push_back(set0);

    const std::filesystem::path pkgPath =
        std::filesystem::temp_directory_path() / "ge_s2_reflect_test.shaderpkg";
    std::unordered_map<std::string, std::vector<uint8_t>> stages;
    stages["vs"] = {1, 2, 3, 4};
    stages["fs"] = {5, 6, 7, 8};
    std::string saveErr;
    ASSERT_TRUE(SaveShaderPkg(pkgPath, meta, stages, std::nullopt, &saveErr)) << saveErr;

    // Guard: if the just-written pkg can't be loaded back in this environment,
    // the reflection path can't be exercised — skip rather than false-fail.
    ShaderPackage probe;
    std::string loadErr;
    if (!LoadShaderPkg(pkgPath.generic_string(), ShaderSourceKind::SpirV, probe, &loadErr))
        GTEST_SKIP() << "staged shaderpkg not loadable in this environment: " << loadErr;

    std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "FX", "type": "FullscreenShader", "shaderPkg": ")json";
    json += pkgPath.generic_string();
    json += R"json(", "output": "View.Resolve",
          "pushConstants": { "goodKey": 1.0, "badKey": 2.0 },
          "inputs": { "uGood": "View.Color", "uBad": "View.Color" } }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Warning, "badKey")) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Warning, "uBad")) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "goodKey")) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "'uGood'")) << IssuesToString(bp);

    std::error_code ec;
    std::filesystem::remove(pkgPath, ec);
}

namespace
{
std::string RebuildActionWithoutStagedTools(const std::string& packageName)
{
    return GameEngine::Detail::ShaderPackageRebuildAction(packageName, {});
}

// Installs the engine's rebuild action for one scope and removes it on every exit
// path, a failed assertion included, so no later test in the binary loads with it.
class ScopedShaderPackageRebuildAction
{
  public:
    ScopedShaderPackageRebuildAction()
    {
        GameEngine::Rendering::SetShaderPackageRebuildAction(&RebuildActionWithoutStagedTools);
    }
    ~ScopedShaderPackageRebuildAction() { GameEngine::Rendering::SetShaderPackageRebuildAction(nullptr); }
    ScopedShaderPackageRebuildAction(const ScopedShaderPackageRebuildAction&) = delete;
    ScopedShaderPackageRebuildAction& operator=(const ScopedShaderPackageRebuildAction&) = delete;
};
} // namespace

TEST(RenderPipelineCompiler, AStalePackageVersionNamesTheRebuild)
{
    // An explicitly addressed stale package is refused, and the compile error
    // carries the loader's rebuild action: a package addressed by absolute path is
    // not one of the engine build's, so the action is the source cook tool.
    using namespace GameEngine::Rendering;
    const std::filesystem::path pkgPath =
        std::filesystem::temp_directory_path() / "ge_stale_version_test.shaderpkg";
    std::string saveErr;
    ASSERT_TRUE(SaveShaderPkg(pkgPath, ShaderMeta{}, {{"fs", {1, 2, 3, 4}}}, std::nullopt, &saveErr)) << saveErr;
    {
        std::fstream file(pkgPath, std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t staleVersion = 1;
        file.seekp(8);
        file.write(reinterpret_cast<const char*>(&staleVersion), sizeof(staleVersion));
    }

    std::string json = R"json({
      "schemaVersion": 2,
      "passes": [ { "id": "FX", "type": "FullscreenShader", "shaderPkg": ")json";
    json += pkgPath.generic_string();
    json += R"json(", "output": "View.Resolve" } ]
    })json";
    const ScopedShaderPackageRebuildAction rebuildAction;
    const auto bp = CompileText(json);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "could not be loaded")) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "ShaderReflect")) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Error, "CompileShaderPkgs")) << IssuesToString(bp);

    std::error_code ec;
    std::filesystem::remove(pkgPath, ec);
}

// ── skipWhen + duplicate output (Warnings) ────────────────────────────────────

TEST(RenderPipelineCompiler, UnknownSkipWhenFieldIsWarning)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "Bloom", "type": "FullscreenShader", "shaderPkg": "bloom.shaderpkg",
          "output": "View.Resolve", "skipWhen": { "bogusGate": 0.0 } }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Warning, "bogusGate")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, KnownSkipWhenFieldIsAccepted)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "Bloom", "type": "FullscreenShader", "shaderPkg": "bloom.shaderpkg",
          "output": "View.Resolve", "skipWhen": { "bloomActive": 0.0, "heightFogActive": 0.0 } }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "skipWhen")) << IssuesToString(bp);
}

// Two ENABLED passes inventing the same name that has NO declared target and is
// not a builtin — the genuine clobber the warning exists to catch.
TEST(RenderPipelineCompiler, DuplicateOutputNameIsWarning)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "A", "type": "FullscreenShader", "shaderPkg": "a.shaderpkg", "output": "DupTarget" },
        { "id": "B", "type": "FullscreenShader", "shaderPkg": "b.shaderpkg", "output": "DupTarget" }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Warning, "DupTarget")) << IssuesToString(bp);
}

// A disabled pass declares nothing at runtime, so it cannot collide: the second
// publisher being off leaves a single live writer — no warning (an off-by-default
// debug overlay re-using a name is the fleet's most common false positive).
TEST(RenderPipelineCompiler, DuplicateOutputFromDisabledPassIsNotWarning)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "A", "type": "FullscreenShader", "shaderPkg": "a.shaderpkg", "output": "DupTarget" },
        { "id": "B", "type": "FullscreenShader", "shaderPkg": "b.shaderpkg", "output": "DupTarget", "enabled": false }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "DupTarget")) << IssuesToString(bp);
}

// A name DECLARED in "resources" is a sanctioned shared target: a ping-pong /
// in-place composite chain writes it from multiple enabled passes by design.
TEST(RenderPipelineCompiler, RepublishingDeclaredResourceIsNotWarning)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "resources": {
        "PingPong": { "kind": "Texture", "format": "R16G16B16A16_FLOAT", "scope": "PerView",
                      "usage": ["RenderTarget", "ShaderResource"] }
      },
      "passes": [
        { "id": "A", "type": "FullscreenShader", "shaderPkg": "a.shaderpkg", "output": "PingPong" },
        { "id": "B", "type": "FullscreenShader", "shaderPkg": "b.shaderpkg", "inputs": { "uSrc": "PingPong" },
          "output": "PingPong", "blendMode": "Alpha" }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "PingPong")) << IssuesToString(bp);
}

// A per-view builtin (View.Resolve) is republishable by design — the final
// resolve target is written by whichever post-FX pass runs last.
TEST(RenderPipelineCompiler, RepublishingViewBuiltinIsNotWarning)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "A", "type": "FullscreenShader", "shaderPkg": "a.shaderpkg", "output": "View.Resolve" },
        { "id": "B", "type": "FullscreenShader", "shaderPkg": "b.shaderpkg", "output": "View.Resolve" }
      ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_FALSE(HasIssue(bp, PipelineIssueSeverity::Warning, "published by")) << IssuesToString(bp);
}

// ── Clean baseline ────────────────────────────────────────────────────────────

TEST(RenderPipelineCompiler, ValidForwardPlusShapedDocHasNoIssues)
{
    const std::string json = R"json({
      "schemaVersion": 2, "pipelineName": "Mini",
      "resources": {
        "SceneColor": { "kind": "Texture", "format": "R16G16B16A16_FLOAT", "scope": "PerView",
                        "usage": ["RenderTarget", "ShaderResource"] },
        "ViewParams": { "kind": "Buffer", "memoryUsage": "Upload", "usage": ["ConstantBuffer"],
                        "size": { "bytes": 160 } }
      },
      "passes": [
        { "id": "ViewUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "Depth", "type": "DepthPrepass" },
        { "id": "Sky",   "type": "SkyRender", "output": "View.Color" },
        { "id": "World", "type": "WorldRender", "colorResolveTarget": "SceneColor" },
        { "id": "Tonemap", "type": "FullscreenShader", "shaderPkg": "tonemap.shaderpkg",
          "inputs": { "uHDR": "SceneColor" }, "output": "View.Resolve",
          "skipWhen": { "bloomActive": 0.0 } }
      ],
      "outputs": { "FinalColor": "View.Resolve" }
    })json";
    const auto bp = CompileText(json);
    EXPECT_EQ(bp.issues.size(), 0u) << IssuesToString(bp);
}

// A node type registered outside the engine (a module or package) declares its
// resource keys at registration: its references are validated, and the names it
// publishes are valid targets for downstream passes.
TEST(RenderPipelineCompiler, RegisteredResourceFieldsValidateModuleNodeTypes)
{
    RenderPipelineNodeRegistry reg = BuildRegistry();
    ASSERT_TRUE(reg.Register(
        "ModuleProducer", [] { return std::unique_ptr<IRenderPipelineNode>{}; }, true,
        RenderPipelineNodeResourceFields{.PublishKeys = {{"output", "ModuleOutput"}}}));
    ASSERT_TRUE(reg.Register(
        "ModuleConsumer", [] { return std::unique_ptr<IRenderPipelineNode>{}; }, true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input"}}));

    const auto resolved = CompileText(R"json({
      "schemaVersion": 2, "pipelineName": "ModuleFields",
      "passes": [
        { "id": "Producer", "type": "ModuleProducer" },
        { "id": "Consumer", "type": "ModuleConsumer", "input": "ModuleOutput" }
      ]
    })json", reg);
    EXPECT_EQ(CountSeverity(resolved, PipelineIssueSeverity::Error), 0) << IssuesToString(resolved);

    const auto unresolved = CompileText(R"json({
      "schemaVersion": 2, "pipelineName": "ModuleFields",
      "passes": [
        { "id": "Consumer", "type": "ModuleConsumer", "input": "MissingOutput" }
      ]
    })json", reg);
    EXPECT_TRUE(HasIssue(unresolved, PipelineIssueSeverity::Error,
                         "unresolvable resource reference 'MissingOutput'"))
        << IssuesToString(unresolved);
}

// ── Q4 shadow-node key rename: "ShadowMap" canonical, "CascadedShadowMap" alias ──
//
// The two keys must compile to the same blueprint (same accepted passes, same
// self-published ShadowData ref resolving cleanly, zero Errors); the ONLY
// difference is the legacy key earns a deprecation Warning nudging migration.

namespace
{
// A one-shadow-node doc parameterized by the type key under test. The node
// self-publishes "ShadowData" via its "buffer" field, so the ref resolves with
// no declared resource — isolating the type-key behavior from everything else.
std::string ShadowNodeDoc(const char* typeKey)
{
    return std::string(R"json({
      "schemaVersion": 2, "pipelineName": "ShadowKeyTest",
      "passes": [
        { "id": "Shadows", "type": ")json") +
           typeKey + R"json(", "buffer": "ShadowData" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
}
} // namespace

TEST(RenderPipelineCompiler, CanonicalShadowMapKeyCompilesCleanly)
{
    const auto bp = CompileText(ShadowNodeDoc("ShadowMap"));
    EXPECT_EQ(CountSeverity(bp, PipelineIssueSeverity::Error), 0) << IssuesToString(bp);
    EXPECT_EQ(bp.issues.size(), 0u) << IssuesToString(bp);
    ASSERT_EQ(bp.passes.size(), 2u) << IssuesToString(bp);
    EXPECT_EQ(bp.passes[0].type, "ShadowMap");
}

TEST(RenderPipelineCompiler, LegacyCascadedShadowMapKeyParsesIdenticallyButWarns)
{
    const auto canonical = CompileText(ShadowNodeDoc("ShadowMap"));
    const auto legacy = CompileText(ShadowNodeDoc("CascadedShadowMap"));

    // Alias parses + compiles like the canonical key: same accepted passes, no
    // "unknown pass type" and no unresolvable-ref Errors from the self-published
    // ShadowData buffer.
    EXPECT_EQ(CountSeverity(legacy, PipelineIssueSeverity::Error), 0) << IssuesToString(legacy);
    ASSERT_EQ(legacy.passes.size(), canonical.passes.size()) << IssuesToString(legacy);
    EXPECT_EQ(legacy.passes[0].type, "CascadedShadowMap");
    EXPECT_EQ(legacy.passes[1].type, canonical.passes[1].type);

    // The one and only difference is the deprecation nudge.
    EXPECT_TRUE(HasIssue(legacy, PipelineIssueSeverity::Warning,
                         "legacy node type key 'CascadedShadowMap'"))
        << IssuesToString(legacy);
    EXPECT_TRUE(HasIssue(legacy, PipelineIssueSeverity::Warning, "rename to 'ShadowMap'"))
        << IssuesToString(legacy);
    EXPECT_EQ(legacy.issues.size(), canonical.issues.size() + 1)
        << "alias should differ from canonical only by the deprecation warning"
        << IssuesToString(legacy);
    EXPECT_FALSE(HasIssue(canonical, PipelineIssueSeverity::Warning, "legacy node type key"))
        << IssuesToString(canonical);
}

// ── S2.2 constants block ──────────────────────────────────────────────────────

TEST(RenderPipelineCompiler, ConstantsParseCleanlyAndAreStored)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "constants": { "clusterTileSize": 32, "depthSlices": 24, "maxLightsPerCluster": 32 },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_EQ(bp.constants.size(), 3u);
    EXPECT_DOUBLE_EQ(bp.constants.at("clusterTileSize"), 32.0);
    EXPECT_DOUBLE_EQ(bp.constants.at("maxLightsPerCluster"), 32.0);
}

TEST(RenderPipelineCompiler, NonNumberConstantIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "constants": { "clusterTileSize": "thirtytwo" },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "must be a number")) << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, ReservedConstantNameIsError)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "constants": { "renderWidth": 100 },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_TRUE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error, "reserved extent identifier"))
        << IssuesToString(bp);
}

TEST(RenderPipelineCompiler, LocalSizeVariableShadowingConstantIsWarning)
{
    const std::string json = R"json({
      "schemaVersion": 2,
      "constants": { "depthSlices": 24 },
      "resources": {
        "Buf": { "kind": "Buffer", "memoryUsage": "DeviceLocal", "usage": ["Storage"],
                 "size": { "expression": "depthSlices * 4", "variables": { "depthSlices": 24 } } }
      },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bp = CompileText(json);
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Warning, "shadows pipeline constant"))
        << IssuesToString(bp);
}

// The migration invariant: a size expression rewritten to reference constants
// evaluates to the same byte count the pre-migration literal form produced.
TEST(RenderPipelineCompiler, ConstantsFeedSizeExpressionsIdenticalToLiterals)
{
    const std::string literalJson = R"json({
      "schemaVersion": 2,
      "resources": {
        "ClusterBuffer": { "kind": "Buffer", "memoryUsage": "DeviceLocal", "usage": ["Storage"],
          "size": { "expression": "16 + clustersX * clustersY * depthSlices * clusterDataBytes",
            "variables": { "clusterDataBytes": 8, "clustersX": "ceil(renderWidth / 32)",
              "clustersY": "ceil(renderHeight / 32)", "depthSlices": 24 } } }
      },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const std::string constJson = R"json({
      "schemaVersion": 2,
      "constants": { "clusterTileSize": 32, "depthSlices": 24 },
      "resources": {
        "ClusterBuffer": { "kind": "Buffer", "memoryUsage": "DeviceLocal", "usage": ["Storage"],
          "size": { "expression": "16 + clustersX * clustersY * depthSlices * clusterDataBytes",
            "variables": { "clusterDataBytes": 8, "clustersX": "ceil(renderWidth / clusterTileSize)",
              "clustersY": "ceil(renderHeight / clusterTileSize)" } } }
      },
      "passes": [ { "id": "World", "type": "WorldRender" } ]
    })json";
    const auto bpLit = CompileText(literalJson);
    const auto bpConst = CompileText(constJson);
    EXPECT_FALSE(bpLit.HasErrors()) << IssuesToString(bpLit);
    EXPECT_FALSE(bpConst.HasErrors()) << IssuesToString(bpConst);
    // Hand value at 1920x1080: 16 + 60*34*24*8 = 391696.
    EXPECT_DOUBLE_EQ(EvalBufferSize(bpLit, "ClusterBuffer", 1920, 1080), 391696.0);
    EXPECT_DOUBLE_EQ(EvalBufferSize(bpConst, "ClusterBuffer", 1920, 1080), 391696.0);
}

// The compiler copies each pass's scope from the registry entry. Frame-scope
// nodes (IBLGen, DDGIGen declare once per frame, not per view) pin that the
// registry the gate compiles against carries the runtime's registration data,
// not a stand-in that only knows the type names.
TEST(RenderPipelineCompilerFleet, PassScopeComesFromTheRuntimeRegistration)
{
    const std::string json = R"json({
      "schemaVersion": 2, "pipelineName": "Scope",
      "passes": [
        { "id": "IBL",   "type": "IBLGen" },
        { "id": "DDGI",  "type": "DDGIGen" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const auto bp = CompileText(json);
    ASSERT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    ASSERT_EQ(bp.passes.size(), 3u);
    EXPECT_FALSE(bp.passes[0].perView) << "IBLGen registers perView=false";
    EXPECT_FALSE(bp.passes[1].perView) << "DDGIGen registers perView=false";
    EXPECT_TRUE(bp.passes[2].perView) << "WorldRender registers perView=true";
}

// A depth prepass or world pass declared before ViewParamsUpload builds its binding table before the
// upload exists and binds the fallback view parameters (an identity projection, no TAAU mip bias)
// silently; the compiler refuses the order and names the fix. The fleet gate below holds every
// committed blueprint to it.
TEST(RenderPipelineCompiler, APassDeclaredBeforeViewParamsUploadIsRejected)
{
    const std::string misordered = R"json({
      "schemaVersion": 2, "pipelineName": "Order",
      "passes": [
        { "id": "DepthPrepass", "type": "DepthPrepass" },
        { "id": "ViewParamsUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const auto bad = CompileText(misordered);
    EXPECT_TRUE(HasIssue(bad, PipelineIssueSeverity::Error, "move 'ViewParamsUpload' above it")) << IssuesToString(bad);

    const std::string ordered = R"json({
      "schemaVersion": 2, "pipelineName": "Order",
      "passes": [
        { "id": "ViewParamsUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "DepthPrepass", "type": "DepthPrepass" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const auto good = CompileText(ordered);
    EXPECT_FALSE(HasIssue(good, PipelineIssueSeverity::Error, "ViewParamsUpload")) << IssuesToString(good);
}

// The passes that feed the camera depth prepass (the terrain upload, CBT terrain and the grass, whose
// depth-only heads the prepass draws from buffers they fill this frame) are declared ahead of the first
// DepthPrepass wherever a blueprint lists them. A game pipeline that lists them after the prepass
// still loads: the compiler moves them, keeps their order, and reports each move as a Warning that
// names the order to author. Nothing else moves.
TEST(RenderPipelineCompiler, PassesFeedingTheDepthPrepassAreDeclaredAheadOfIt)
{
    const std::string listedAfter = R"json({
      "schemaVersion": 2, "pipelineName": "Order",
      "passes": [
        { "id": "ViewParamsUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "DepthPrepass", "type": "DepthPrepass" },
        { "id": "LightUpload", "type": "LightUpload" },
        { "id": "TerrainUpload", "type": "TerrainUpload" },
        { "id": "CBTTerrain", "type": "CBTRender" },
        { "id": "TerrainGrass", "type": "TerrainGrass" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const auto moved = CompileText(listedAfter);
    EXPECT_EQ(CountSeverity(moved, PipelineIssueSeverity::Error), 0) << IssuesToString(moved);
    std::vector<std::string> order;
    for (const auto& pass : moved.passes)
        order.push_back(pass.id);
    const std::vector<std::string> expected = {"ViewParamsUpload", "TerrainUpload", "CBTTerrain", "TerrainGrass",
                                               "DepthPrepass",     "LightUpload",   "World"};
    EXPECT_EQ(order, expected);
    for (const char* feeder : {"'TerrainUpload'", "'CBTTerrain'", "'TerrainGrass'"})
        EXPECT_TRUE(HasIssue(moved, PipelineIssueSeverity::Warning, std::string(feeder) + " (")) << feeder;
    // Each warning names the place to list the pass: between the pass authored above the prepass and the prepass.
    EXPECT_EQ(std::count_if(moved.issues.begin(), moved.issues.end(),
                            [](const PipelineIssue& issue)
                            {
                                return issue.severity == PipelineIssueSeverity::Warning &&
                                       issue.message.find("feeds the depth prepass") != std::string::npos;
                            }),
              3)
        << IssuesToString(moved);
    EXPECT_TRUE(HasIssue(moved, PipelineIssueSeverity::Warning,
                         "pass 'CBTTerrain' (CBTRender) feeds the depth prepass but is listed after DepthPrepass "
                         "'DepthPrepass', so the compiler declares it above 'DepthPrepass'. List it between "
                         "'ViewParamsUpload' and 'DepthPrepass' to clear this warning"))
        << IssuesToString(moved);

    const std::string listedAhead = R"json({
      "schemaVersion": 2, "pipelineName": "Order",
      "passes": [
        { "id": "ViewParamsUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "TerrainUpload", "type": "TerrainUpload" },
        { "id": "CBTTerrain", "type": "CBTRender" },
        { "id": "TerrainGrass", "type": "TerrainGrass" },
        { "id": "DepthPrepass", "type": "DepthPrepass" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const auto ahead = CompileText(listedAhead);
    EXPECT_FALSE(HasIssue(ahead, PipelineIssueSeverity::Warning, "feeds the depth prepass")) << IssuesToString(ahead);
}

// ── The fleet gate: every repo blueprint compiles without Errors ──────────────

TEST(RenderPipelineCompilerFleet, AllRepoBlueprintsCompileWithoutErrors)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined";
#else
    namespace fs = std::filesystem;
    const fs::path root = GE_RENDERER_REPO_ROOT;
    const std::vector<fs::path> roots = {
        root / "Assets",
        root / "Tests" / "Projects",
        root / "Tests" / "EditorHarness",
        root / "Tools" / "Web",
    };

    const RenderPipelineNodeRegistry reg = BuildRegistry();
    RenderPipelineCompiler compiler;

    int fileCount = 0;
    int errorFiles = 0;
    int totalWarnings = 0;
    int legacyKeyWarnings = 0;
    int duplicatePublishWarnings = 0;

    for (const auto& r : roots)
    {
        std::error_code ec;
        if (!fs::exists(r, ec))
            continue;
        for (auto it = fs::recursive_directory_iterator(r, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".rendergraph")
                continue;

            // Skip gitignored local snapshots: the integration-test harness writes
            // timestamped .rendergraph backups under a "backups" dir. The fleet gate
            // validates repo blueprints, not local cruft (which may carry pass types
            // already deleted from the repo).
            {
                bool inBackups = false;
                for (const auto& seg : it->path())
                    if (seg == "backups") { inBackups = true; break; }
                if (inBackups)
                    continue;
            }

            std::ifstream file(it->path(), std::ios::binary);
            std::ostringstream ss;
            ss << file.rdbuf();
            const std::string text = ss.str();
            if (text.empty())
                continue;

            ++fileCount;
            RenderPipelineAsset asset(GUID::Null(), it->path());
            Vector<uint8> data(text.begin(), text.end());
            asset.LoadFromData(data);
            const auto bp = compiler.Compile(asset, reg);

            const int errs = CountSeverity(bp, PipelineIssueSeverity::Error);
            const int warns = CountSeverity(bp, PipelineIssueSeverity::Warning);
            totalWarnings += warns;
            for (const auto& iss : bp.issues)
            {
                if (iss.severity != PipelineIssueSeverity::Warning)
                    continue;
                if (iss.message.find("legacy node type key") != std::string::npos)
                    ++legacyKeyWarnings;
                if (iss.message.find("is published by") != std::string::npos)
                    ++duplicatePublishWarnings;
            }

            const fs::path rel = fs::relative(it->path(), root, ec);
            std::cout << "[fleet] " << rel.generic_string() << " -> " << errs << " error(s), "
                      << warns << " warning(s)\n";
            for (const auto& iss : bp.issues)
                std::cout << "        [" << (iss.severity == PipelineIssueSeverity::Error ? "ERROR" : "warn")
                          << "] " << iss.message << (iss.nodeId.empty() ? "" : (" (node " + iss.nodeId + ")"))
                          << "\n";

            if (errs > 0)
            {
                ++errorFiles;
                ADD_FAILURE() << rel.generic_string() << " produced " << errs
                              << " Error(s):" << IssuesToString(bp);
            }
        }
    }

    std::cout << "[fleet] compiled " << fileCount << " blueprint(s); " << errorFiles
              << " with Errors; " << totalWarnings << " total warning(s); " << legacyKeyWarnings
              << " legacy-key warning(s); " << duplicatePublishWarnings
              << " duplicate-publish warning(s)\n";
    EXPECT_GT(fileCount, 0) << "no .rendergraph files found under " << root.generic_string();
    EXPECT_EQ(errorFiles, 0);
    // Q4: every in-repo blueprint must be on the new "ShadowMap" key — no blueprint
    // may still trip the "CascadedShadowMap" legacy-alias deprecation warning.
    EXPECT_EQ(legacyKeyWarnings, 0)
        << legacyKeyWarnings << " blueprint(s) still use the legacy 'CascadedShadowMap' type key";
    // Q8: the fleet's duplicate-publish warnings were all disabled-pass or
    // ping-pong false positives, now narrowed out at the validator. A nonzero
    // count here means a blueprint has a genuine undeclared-name collision to fix.
    EXPECT_EQ(duplicatePublishWarnings, 0)
        << duplicatePublishWarnings << " blueprint(s) publish an undeclared name from >1 enabled pass";
#endif
}

// ── S2.5 PipelineCompileReport retention ──────────────────────────────────────
//
// These exercise the recording logic FrameOrchestrator::EnsureActiveRenderPipeline
// Blueprint drives at its compile/load sites. Driving the orchestrator's swap
// decision itself needs a live EngineCore + AssetManager fixture (not reachable
// here without an editor boot), so the "active pipeline stays last-good on
// reject" behavior is asserted indirectly: report.Rejected is the exact flag the
// orchestrator gates its blueprint swap on (m_ActiveRenderPipelineLastCompile
// HadErrors = bp.HasErrors(), unchanged by this work), and the whole-pipeline
// path is runtime-verified in the S2.5 editor gate. The recording logic itself
// is fully covered below.
namespace
{
using GameEngine::Engine::Renderer::PipelineCompileReport;

RenderPipelineBlueprint CompileWarningDoc()
{
    // Duplicate output name => a Warning, no Error (accept-with-warnings).
    const std::string json = R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "A", "type": "FullscreenShader", "shaderPkg": "a.shaderpkg", "output": "DupTarget" },
        { "id": "B", "type": "FullscreenShader", "shaderPkg": "b.shaderpkg", "output": "DupTarget" }
      ]
    })json";
    return CompileText(json);
}

RenderPipelineBlueprint CompileErrorDoc()
{
    // Unknown pipeline output ref => an Error (rejected compile).
    const std::string json = R"json({
      "schemaVersion": 2, "pipelineName": "T",
      "passes": [ { "id": "World", "type": "WorldRender" } ],
      "outputs": { "FinalColor": "View.Nonexistent" }
    })json";
    return CompileText(json);
}
} // namespace

TEST(PipelineCompileReport, AcceptWithWarningsKeepsWarningsNotRejected)
{
    const RenderPipelineBlueprint bp = CompileWarningDoc();
    ASSERT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    ASSERT_GT(bp.issues.size(), 0u) << "expected at least one warning";

    PipelineCompileReport report;
    report.RecordCompile(bp, std::filesystem::path("Test/Warn.rendergraph"), 0x1234u,
                         /*fallback=*/false, /*awaitsModules=*/false);

    EXPECT_FALSE(report.Rejected);
    EXPECT_FALSE(report.FallbackCompile);
    EXPECT_TRUE(report.LoadFailureReason.empty());
    EXPECT_EQ(report.Issues.size(), bp.issues.size());
    EXPECT_EQ(report.SourceHash, 0x1234u);
    EXPECT_GT(report.Generation, 0u);
}

TEST(PipelineCompileReport, RejectSetsRejectedFlag)
{
    const RenderPipelineBlueprint bp = CompileErrorDoc();
    ASSERT_TRUE(bp.HasErrors()) << IssuesToString(bp);

    PipelineCompileReport report;
    report.RecordCompile(bp, std::filesystem::path("Test/Bad.rendergraph"), 0x9999u,
                         /*fallback=*/false, /*awaitsModules=*/false);

    // Rejected == true is the flag the orchestrator gates its blueprint swap on,
    // so recording it maps 1:1 to "the last-good pipeline is kept".
    EXPECT_TRUE(report.Rejected);
    EXPECT_FALSE(report.FallbackCompile);
    EXPECT_TRUE(report.LoadFailureReason.empty());
    EXPECT_GT(report.Generation, 0u);
}

TEST(PipelineCompileReport, FallbackCompileErrorMarksFallback)
{
    const RenderPipelineBlueprint bp = CompileErrorDoc();
    ASSERT_TRUE(bp.HasErrors()) << IssuesToString(bp);

    PipelineCompileReport report;
    report.RecordCompile(bp, std::filesystem::path("RenderPipelines/ForwardPlus.rendergraph"),
                         0x55u, /*fallback=*/true, /*awaitsModules=*/false);

    EXPECT_TRUE(report.Rejected);
    EXPECT_TRUE(report.FallbackCompile);
    EXPECT_GT(report.Generation, 0u);
}

TEST(PipelineCompileReport, RejectionIsLoggedOnceWhileAFallbackIsRecordedBetweenAttempts)
{
    // While a fallback stands in, the frame loop retries the rejected request
    // every frame and records the fallback's compile in between.
    RenderPipelineBlueprint requested;
    requested.issues.push_back({PipelineIssueSeverity::Error,
                                "shaderPkg 'project:Shaders/refused_version.shaderpkg' has version 1",
                                "Custom"});
    for (const bool fallbackRejected : {false, true})
    {
        RenderPipelineBlueprint fallback;
        if (fallbackRejected)
            fallback.issues.push_back({PipelineIssueSeverity::Error,
                                       "shaderPkg 'Shaders/refused_fallback.shaderpkg' has version 1",
                                       "Tonemap"});
        PipelineCompileReport report;
        std::vector<std::string> errors;
        GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
        for (int frame = 0; frame < 5; ++frame)
        {
            report.RecordCompile(requested, "RenderPipelines/RejectedVersion.rendergraph", 17, false, false);
            report.RecordCompile(fallback, "RenderPipelines/ForwardPlus.rendergraph", 42, true, false);
        }
        Logger::Log::Flush();
        EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(errors, "refused_version.shaderpkg"), 1u)
            << "fallback rejected: " << fallbackRejected;
        const std::string requestedLine =
            GameEngine::TestLog::FirstLineContaining(errors, "refused_version.shaderpkg");
        EXPECT_NE(requestedLine.find("'RenderPipelines/RejectedVersion.rendergraph' was rejected:"),
                  std::string::npos) << requestedLine;
        EXPECT_NE(requestedLine.find("(node 'Custom')"), std::string::npos) << requestedLine;
        EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(errors, "refused_fallback.shaderpkg"),
                  fallbackRejected ? 1u : 0u);
        if (fallbackRejected)
        {
            const std::string fallbackLine =
                GameEngine::TestLog::FirstLineContaining(errors, "refused_fallback.shaderpkg");
            EXPECT_NE(fallbackLine.find("'RenderPipelines/ForwardPlus.rendergraph' was rejected as a fallback:"),
                      std::string::npos) << fallbackLine;
            EXPECT_NE(fallbackLine.find("(node 'Tonemap')"), std::string::npos) << fallbackLine;
        }

        // A compile in between is a new outcome, so the same rejection afterwards logs again.
        report.RecordCompile(RenderPipelineBlueprint{}, "RenderPipelines/RejectedVersion.rendergraph", 18, false, false);
        report.RecordCompile(requested, "RenderPipelines/RejectedVersion.rendergraph", 17, false, false);
        Logger::Log::Flush();
        EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(errors, "refused_version.shaderpkg"), 2u);
    }
}

TEST(PipelineCompileReport, FallbackRejectionLogsAgainAfterTheRequestCompiled)
{
    // X is requested and rejected, and the ForwardPlus fallback is rejected too, so
    // the built-in default runs. The user selects Y, which compiles, then selects X
    // again: both rejections are news again, the fallback's included.
    RenderPipelineBlueprint requested;
    requested.issues.push_back({PipelineIssueSeverity::Error,
                                "shaderPkg 'project:Shaders/refused_version.shaderpkg' has version 1",
                                "Custom"});
    RenderPipelineBlueprint fallback;
    fallback.issues.push_back({PipelineIssueSeverity::Error,
                               "shaderPkg 'Shaders/refused_fallback.shaderpkg' has version 1",
                               "Tonemap"});
    PipelineCompileReport report;
    std::vector<std::string> errors;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
    report.RecordCompile(requested, "RenderPipelines/X.rendergraph", 17, false, /*awaitsModules=*/false);
    report.RecordCompile(fallback, "RenderPipelines/ForwardPlus.rendergraph", 42, true, /*awaitsModules=*/false);
    report.RecordCompile(RenderPipelineBlueprint{}, "RenderPipelines/Y.rendergraph", 5, false, /*awaitsModules=*/false);
    report.RecordCompile(requested, "RenderPipelines/X.rendergraph", 17, false, /*awaitsModules=*/false);
    report.RecordCompile(fallback, "RenderPipelines/ForwardPlus.rendergraph", 42, true, /*awaitsModules=*/false);
    Logger::Log::Flush();
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(errors, "refused_version.shaderpkg"), 2u);
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(errors, "refused_fallback.shaderpkg"), 2u)
        << "the second episode's fallback rejection must be named";
}

TEST(PipelineCompileReport, RejectionLogsAgainAfterTheRequestFailedToLoad)
{
    RenderPipelineBlueprint requested;
    requested.issues.push_back({PipelineIssueSeverity::Error,
                                "shaderPkg 'project:Shaders/refused_version.shaderpkg' has version 1",
                                "Custom"});
    PipelineCompileReport report;
    std::vector<std::string> errors;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
    report.RecordCompile(requested, "RenderPipelines/X.rendergraph", 17, false, /*awaitsModules=*/false);
    report.RecordLoadFailure("RenderPipelines/X.rendergraph", "failed to parse/load");
    report.RecordCompile(requested, "RenderPipelines/X.rendergraph", 17, false, /*awaitsModules=*/false);
    Logger::Log::Flush();
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(errors, "refused_version.shaderpkg"), 2u)
        << "a load failure in between is a new outcome";
}

// The warning for a pass that feeds the depth prepass but is listed after it is logged, so a Player (which
// has no Render Graph panel) reports it too, and it is logged once per graph: not again when the same graph's
// outcome changes around it (another issue comes and goes while the project's scripts load, a fallback is
// recorded in between, the file is saved with the same order), but once for each other graph that lists a
// feeder late, and again for a graph whose order was fixed and then broken.
TEST(PipelineCompileReport, TheDepthPrepassFeederWarningIsLoggedOncePerSource)
{
    const std::string listedAfter = R"json({
      "schemaVersion": 2, "pipelineName": "Order",
      "passes": [
        { "id": "ViewParamsUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "DepthPrepass", "type": "DepthPrepass" },
        { "id": "TerrainUpload", "type": "TerrainUpload" },
        { "id": "CBTTerrain", "type": "CBTRender" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const std::string listedAhead = R"json({
      "schemaVersion": 2, "pipelineName": "Order",
      "passes": [
        { "id": "ViewParamsUpload", "type": "ViewParamsUpload", "buffer": "ViewParams" },
        { "id": "TerrainUpload", "type": "TerrainUpload" },
        { "id": "CBTTerrain", "type": "CBTRender" },
        { "id": "DepthPrepass", "type": "DepthPrepass" },
        { "id": "World", "type": "WorldRender" }
      ]
    })json";
    const RenderPipelineBlueprint moved = CompileText(listedAfter);
    const RenderPipelineBlueprint ahead = CompileText(listedAhead);
    ASSERT_FALSE(moved.HasErrors()) << IssuesToString(moved);
    ASSERT_FALSE(ahead.HasErrors()) << IssuesToString(ahead);
    ASSERT_TRUE(HasIssue(moved, PipelineIssueSeverity::Warning, "feeds the depth prepass")) << IssuesToString(moved);
    ASSERT_FALSE(HasIssue(ahead, PipelineIssueSeverity::Warning, "feeds the depth prepass")) << IssuesToString(ahead);
    // The same graph while a script pass waits: one more issue, so a new outcome for the report.
    RenderPipelineBlueprint movedWhileScriptsLoad = moved;
    movedWhileScriptsLoad.issues.push_back(
        {PipelineIssueSeverity::Info, "pass 'Custom' waits for the project's scripts", "Custom"});
    const RenderPipelineBlueprint fallback = CompileWarningDoc();

    PipelineCompileReport report;
    std::vector<std::string> warnings;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&warnings, Logger::LogLevel::Warning);
    Logger::Log::Warning("feeder-warning capture is live");
    const auto feederLines = [&warnings](std::string_view graph)
    {
        Logger::Log::Flush();
        return std::count_if(warnings.begin(), warnings.end(),
                             [graph](const std::string& line)
                             {
                                 return line.find("feeds the depth prepass") != std::string::npos &&
                                        line.find(graph) != std::string::npos;
                             });
    };

    for (int frame = 0; frame < 3; ++frame)
    {
        report.RecordCompile(moved, "RenderPipelines/Game.rendergraph", 7, false, /*awaitsModules=*/false);
        report.RecordCompile(movedWhileScriptsLoad, "RenderPipelines/Game.rendergraph", 7, false, /*awaitsModules=*/false);
        report.RecordCompile(fallback, "RenderPipelines/ForwardPlus.rendergraph", 42, true, /*awaitsModules=*/false);
    }
    report.RecordCompile(moved, "RenderPipelines/Game.rendergraph", 8, false, /*awaitsModules=*/false);
    Logger::Log::Flush();
    ASSERT_EQ(GameEngine::TestLog::CountLinesContaining(warnings, "feeder-warning capture is live"), 1u);
    EXPECT_EQ(feederLines("'RenderPipelines/Game.rendergraph'"), 2) << "one line for each of the two moved passes";
    const std::string cbtLine = GameEngine::TestLog::FirstLineContaining(warnings, "pass 'CBTTerrain' (CBTRender)");
    EXPECT_NE(cbtLine.find("RenderPipeline 'RenderPipelines/Game.rendergraph': pass 'CBTTerrain' (CBTRender) feeds "
                           "the depth prepass but is listed after DepthPrepass 'DepthPrepass', so the compiler "
                           "declares it above 'DepthPrepass'. List it between 'ViewParamsUpload' and "
                           "'DepthPrepass' to clear this warning"),
              std::string::npos)
        << cbtLine;
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(warnings, "ForwardPlus.rendergraph"), 0u)
        << "the stand-in's other warnings are not logged";

    report.RecordCompile(moved, "RenderPipelines/Other.rendergraph", 9, false, /*awaitsModules=*/false);
    EXPECT_EQ(feederLines("'RenderPipelines/Other.rendergraph'"), 2) << "another graph is another source";

    report.RecordCompile(ahead, "RenderPipelines/Game.rendergraph", 10, false, /*awaitsModules=*/false);
    report.RecordCompile(moved, "RenderPipelines/Game.rendergraph", 11, false, /*awaitsModules=*/false);
    EXPECT_EQ(feederLines("'RenderPipelines/Game.rendergraph'"), 4) << "fixed, then listed late again";
}

TEST(PipelineCompileReport, LoadFailureSetsReasonNotRejected)
{
    PipelineCompileReport report;
    report.RecordLoadFailure(std::filesystem::path("Test/Missing.rendergraph"),
                             "failed to parse/load");

    EXPECT_FALSE(report.Rejected);
    EXPECT_FALSE(report.FallbackCompile);
    EXPECT_TRUE(report.Issues.empty());
    EXPECT_EQ(report.LoadFailureReason, "failed to parse/load");
    EXPECT_GT(report.Generation, 0u);
}

TEST(PipelineCompileReport, RecordingIsIdempotentOnGeneration)
{
    const RenderPipelineBlueprint bp = CompileWarningDoc();
    PipelineCompileReport report;
    report.RecordCompile(bp, std::filesystem::path("Test/Warn.rendergraph"), 0x1234u, false, /*awaitsModules=*/false);
    const uint64_t gen = report.Generation;
    // The same outcome recorded again (a transient fallback recompiles every
    // frame) must not advance the panel's refresh key.
    report.RecordCompile(bp, std::filesystem::path("Test/Warn.rendergraph"), 0x1234u, false, /*awaitsModules=*/false);
    EXPECT_EQ(report.Generation, gen);
}

TEST(PipelineCompileReport, RecompileClearsPriorLoadFailure)
{
    PipelineCompileReport report;
    report.RecordLoadFailure(std::filesystem::path("Test/Missing.rendergraph"), "asset returned null");
    ASSERT_FALSE(report.LoadFailureReason.empty());

    const RenderPipelineBlueprint bp = CompileWarningDoc();
    report.RecordCompile(bp, std::filesystem::path("Test/Missing.rendergraph"), 0x1u, false, /*awaitsModules=*/false);
    EXPECT_TRUE(report.LoadFailureReason.empty());
    EXPECT_FALSE(report.Rejected);
}


namespace
{
RenderPipelineBlueprint CompileForShaderSource(const nlohmann::json& document,
                                               Rendering::ShaderSourceKind sourceKind)
{
    const std::string text = document.dump();
    RenderPipelineAsset asset(GUID::Null(), std::filesystem::path("source-variants.rendergraph"));
    const Vector<uint8> bytes(text.begin(), text.end());
    asset.LoadFromData(bytes);
    return RenderPipelineCompiler{}.Compile(asset, BuildRegistry(), sourceKind);
}
}

TEST(RenderPipelineCompiler, ShaderSourceOverridesResolveBeforeValidation)
{
    const auto document = nlohmann::json::parse(R"json({
      "schemaVersion": 2,
      "passes": [
        { "id": "NativeBake", "type": "ComputeShader", "shaderPkg": "native-only.shaderpkg",
          "shaderSourceOverrides": { "wgsl": null } },
        { "id": "Copy", "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
          "inputs": { "uSrc": "View.Color" }, "output": "View.Resolve",
          "shaderSourceOverrides": { "wgsl": { "inputs": { "uSrc": "View.Unknown" } } } }
      ]
    })json");
    const auto native = CompileForShaderSource(document, Rendering::ShaderSourceKind::SpirV);
    const auto web = CompileForShaderSource(document, Rendering::ShaderSourceKind::Wgsl);
    ASSERT_EQ(native.passes.size(), 2u);
    ASSERT_EQ(web.passes.size(), 1u);
    EXPECT_EQ(web.passes.front().id, "Copy");
    EXPECT_FALSE(HasIssue(native, PipelineIssueSeverity::Error, "View.Unknown"));
    EXPECT_TRUE(HasIssue(web, PipelineIssueSeverity::Error, "View.Unknown"));
    EXPECT_FALSE(HasIssue(web, PipelineIssueSeverity::Error, "native-only.shaderpkg"));
    EXPECT_NE(native.contentHash, web.contentHash);
    const auto pass = nlohmann::json::parse(web.passes.front().passJson);
    EXPECT_FALSE(pass.contains("shaderSourceOverrides"));
    EXPECT_EQ(pass.at("output"), "View.Resolve");
}

TEST(RenderPipelineCompiler, ShaderSourceOverridesRejectMalformedVariants)
{
    for (const auto& invalid : {R"(false)", R"({"web": {}})", R"({"wgsl": []})",
                                R"({"spirv": "wrong"})"})
    {
        nlohmann::json document = {
            {"schemaVersion", 2},
            {"passes", {{{"id", "World"}, {"type", "WorldRender"},
                          {"shaderSourceOverrides", nlohmann::json::parse(invalid)}}}}};
        for (auto source : {Rendering::ShaderSourceKind::SpirV, Rendering::ShaderSourceKind::Wgsl})
        {
            const auto blueprint = CompileForShaderSource(document, source);
            EXPECT_TRUE(HasIssue(blueprint, PipelineIssueSeverity::Error, "shaderSourceOverrides"))
                << invalid;
        }
    }
}

TEST(RenderPipelineCompiler, InputGatesWarnUnlessAnInputIsGatedOnASettingsGate)
{
    struct Case
    {
        const char* Label;
        nlohmann::json Gates;
        bool Warns;
    };
    const Case cases[] = {
        {"a settings gate on a declared input", {{"uSrc", "bloomScatteringActive"}}, false},
        {"misspelt gate", {{"uSrc", "bloomScaterringActive"}}, true},
        {"value field as gate", {{"uSrc", "bloomOctaves"}}, true},
        {"misspelt input", {{"uSrcc", "bloomScatteringActive"}}, true},
        {"non-string gate", {{"uSrc", 5}}, true},
    };
    for (const Case& c : cases)
    {
        nlohmann::json document = nlohmann::json::parse(R"json({
          "schemaVersion": 2,
          "passes": [
            { "id": "Combine", "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
              "inputs": { "uSrc": "View.Color" }, "output": "View.Resolve" }
          ]
        })json");
        document["passes"][0]["inputGates"] = c.Gates;
        const auto blueprint = CompileForShaderSource(document, Rendering::ShaderSourceKind::SpirV);
        EXPECT_EQ(HasIssue(blueprint, PipelineIssueSeverity::Warning, "inputGates"), c.Warns)
            << c.Label << ": " << IssuesToString(blueprint);
    }
}

TEST(RenderPipelineCompiler, ForwardPlusCloudPassesMatchCookedShaderSource)
{
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "Assets/RenderPipelines/ForwardPlus.rendergraph";
    std::ifstream stream(path);
    ASSERT_TRUE(stream.is_open()) << path;
    const auto document = nlohmann::json::parse(stream);
    for (auto source : {Rendering::ShaderSourceKind::SpirV, Rendering::ShaderSourceKind::Wgsl})
    {
        const auto blueprint = CompileForShaderSource(document, source);
        size_t normalizers = 0;
        bool foundMarch = false;
        for (const auto& pass : blueprint.passes)
        {
            if (pass.id == "CloudShapeNoiseNormalize" || pass.id == "CloudDetailNoiseNormalize")
                ++normalizers;
            if (pass.id == "VolumetricCloudsMarch")
            {
                foundMarch = true;
                const auto node = nlohmann::json::parse(pass.passJson);
                EXPECT_EQ(node.at("buffers").contains("ShapeNoiseMinMax"),
                          source == Rendering::ShaderSourceKind::Wgsl);
                EXPECT_EQ(node.at("buffers").contains("DetailNoiseMinMax"),
                          source == Rendering::ShaderSourceKind::Wgsl);
                EXPECT_EQ(node.at("buffers").at("ViewParams"), "ViewParams");
            }
        }
        EXPECT_TRUE(foundMarch);
        EXPECT_EQ(normalizers, source == Rendering::ShaderSourceKind::SpirV ? 2u : 0u);
    }
}

// A pass type no loaded scripts register is refused with the fix: build the scripts, install the
// package, or remove the node.
TEST(RenderPipelineCompiler, AnUnknownPassTypeIsRefusedWithTheFix)
{
    const auto bp = CompileText(ShadowNodeDoc("MissionFog"));
    EXPECT_TRUE(HasIssue(bp, PipelineIssueSeverity::Error,
                         "unknown pass type 'MissionFog' (node 'Shadows'): no loaded scripts register it; build the "
                         "scripts or install the package that provides it, or remove the node."))
        << IssuesToString(bp);
}

// While the project's scripts build, a pass of an unknown type waits: the rest of the graph
// compiles without it, the pass is listed as pending, and the blueprint differs from the full one.
TEST(RenderPipelineCompiler, AnUnknownPassTypeWaitsForTheScriptsWhileTheyBuild)
{
    const std::string json = ShadowNodeDoc("MissionFog");
    RenderPipelineAsset asset(GUID::Null(), std::filesystem::path("test.rendergraph"));
    Vector<uint8> data(json.begin(), json.end());
    asset.LoadFromData(data);
    const RenderPipelineNodeRegistry registry = BuildRegistry();
    const auto waiting = RenderPipelineCompiler{}.Compile(asset, registry, Rendering::ShaderSourceKind::SpirV,
                                                          UnknownPassTypes::PendingScripts);
    EXPECT_FALSE(waiting.HasErrors()) << IssuesToString(waiting);
    ASSERT_EQ(waiting.passes.size(), 1u) << IssuesToString(waiting);
    EXPECT_EQ(waiting.passes[0].id, "World");
    ASSERT_EQ(waiting.issues.size(), 1u) << IssuesToString(waiting);
    EXPECT_EQ(waiting.issues[0].severity, PipelineIssueSeverity::Info);
    EXPECT_EQ(waiting.issues[0].code, PipelineIssueCode::PendingScriptPass);
    EXPECT_EQ(waiting.issues[0].nodeId, "Shadows");

    const auto refused = RenderPipelineCompiler{}.Compile(asset, registry);
    EXPECT_TRUE(refused.HasErrors());
    EXPECT_NE(waiting.contentHash, refused.contentHash) << "the graph without its waiting pass hashes as the full one";
}

namespace
{
RenderPipelineBlueprint CompileWhileScriptsBuild(const std::string& json)
{
    RenderPipelineAsset asset(GUID::Null(), std::filesystem::path("test.rendergraph"));
    Vector<uint8> data(json.begin(), json.end());
    asset.LoadFromData(data);
    return RenderPipelineCompiler{}.Compile(asset, BuildRegistry(), Rendering::ShaderSourceKind::SpirV,
                                            UnknownPassTypes::PendingScripts);
}

const PipelineIssue* IssueFor(const RenderPipelineBlueprint& bp, PipelineIssueCode code, const std::string& nodeId)
{
    for (const auto& issue : bp.issues)
        if (issue.code == code && issue.nodeId == nodeId)
            return &issue;
    return nullptr;
}
} // namespace

// A pass that reads a resource only a waiting script pass writes waits with it: drawn, it would read
// a texture nothing wrote. The report names it and the pass it waits on; the rest still compiles.
TEST(RenderPipelineCompiler, APassThatReadsAWaitingPassWaitsWithIt)
{
    const auto bp = CompileWhileScriptsBuild(R"json({
      "schemaVersion": 2,
      "resources": { "FogColor": { "kind": "Texture", "format": "RGBA8_UNORM" } },
      "passes": [
        { "id": "World", "type": "WorldRender" },
        { "id": "Fog",  "type": "MissionFog", "output": "FogColor" },
        { "id": "Composite", "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
          "inputs": { "uFog": "FogColor" }, "output": "View.Resolve" }
      ]
    })json");
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    ASSERT_EQ(bp.passes.size(), 1u) << IssuesToString(bp);
    EXPECT_EQ(bp.passes[0].id, "World");
    EXPECT_NE(IssueFor(bp, PipelineIssueCode::PendingScriptPass, "Fog"), nullptr) << IssuesToString(bp);
    const PipelineIssue* reader = IssueFor(bp, PipelineIssueCode::PendingScriptReader, "Composite");
    ASSERT_NE(reader, nullptr) << "the reader of the waiting pass's output still draws: " << IssuesToString(bp);
    EXPECT_NE(reader->message.find("'Fog'"), std::string::npos) << reader->message;
    EXPECT_EQ(IssueFor(bp, PipelineIssueCode::PendingScriptOutput, "Fog"), nullptr);
}

// A kept pass that writes the resource before the reader reads it supplies it this frame, so the
// reader draws; a kept writer after the reader does not, so the reader waits.
TEST(RenderPipelineCompiler, AKeptWriterBeforeTheReaderLetsItDraw)
{
    const auto writeFirst = CompileWhileScriptsBuild(R"json({
      "schemaVersion": 2,
      "resources": { "FogColor": { "kind": "Texture", "format": "RGBA8_UNORM" } },
      "passes": [
        { "id": "World", "type": "WorldRender", "colorResolveTarget": "FogColor" },
        { "id": "Fog",  "type": "MissionFog", "output": "FogColor" },
        { "id": "Composite", "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
          "inputs": { "uFog": "FogColor" }, "output": "View.Resolve" }
      ]
    })json");
    EXPECT_EQ(IssueFor(writeFirst, PipelineIssueCode::PendingScriptReader, "Composite"), nullptr)
        << IssuesToString(writeFirst);

    const auto writeAfter = CompileWhileScriptsBuild(R"json({
      "schemaVersion": 2,
      "resources": { "FogColor": { "kind": "Texture", "format": "RGBA8_UNORM" } },
      "passes": [
        { "id": "Fog",  "type": "MissionFog", "output": "FogColor" },
        { "id": "Composite", "type": "FullscreenShader", "shaderPkg": "copy.shaderpkg",
          "inputs": { "uFog": "FogColor" }, "output": "View.Resolve" },
        { "id": "World", "type": "WorldRender", "colorResolveTarget": "FogColor" }
      ]
    })json");
    EXPECT_NE(IssueFor(writeAfter, PipelineIssueCode::PendingScriptReader, "Composite"), nullptr)
        << IssuesToString(writeAfter);
}

// When the pipeline's output itself comes from a waiting pass, nothing of the request can draw:
// the compile says so, and the request is not applied while the scripts build.
TEST(RenderPipelineCompiler, AnOutputFromAWaitingPassLeavesNothingToDraw)
{
    const auto bp = CompileWhileScriptsBuild(R"json({
      "schemaVersion": 2,
      "resources": { "FogColor": { "kind": "Texture", "format": "RGBA8_UNORM" } },
      "passes": [
        { "id": "World", "type": "WorldRender" },
        { "id": "Fog",  "type": "MissionFog", "output": "FogColor" }
      ],
      "outputs": { "FinalColor": "FogColor" }
    })json");
    EXPECT_FALSE(bp.HasErrors()) << IssuesToString(bp);
    EXPECT_NE(IssueFor(bp, PipelineIssueCode::PendingScriptOutput, "Fog"), nullptr) << IssuesToString(bp);
}

