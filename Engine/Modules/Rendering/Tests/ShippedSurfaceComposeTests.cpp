// The shipped default materials (Unlit White (M0), Shadow Only, World Debug)
// all author Surfaces/unlit_solid.glsl. That surface must compose + compile
// through the real pipeline (compose + shaderc to SPIR-V): a contract
// violation in shipped content fails those materials at every editor boot and
// buries the author's own errors under engine rows in the Shader Errors panel.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestUtils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

fs::path MakeTempCacheRoot()
{
    static std::atomic<uint32_t> counter{0};
    return fs::temp_directory_path() /
           ("ge_shipped_surface_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(counter.fetch_add(1)));
}

std::string ReadWholeFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A surface names itself and resolves "Includes/..." against the shader ROOT, not against its own
// directory, so composing one out of its module source tree needs a root shaped like the staged
// Assets/Shaders mount: the module's shaders under `mountName`, the engine Includes beside them.
bool StageShaderRoot(const fs::path& moduleShaders, const std::string& mountName,
                     const fs::path& shaderRoot, std::vector<std::string>& errors)
{
    std::error_code ec;
    fs::create_directories(shaderRoot, ec);
    fs::copy(moduleShaders, shaderRoot / mountName, fs::copy_options::recursive, ec);
    if (ec)
    {
        errors.push_back("staging " + mountName + " shaders: " + ec.message());
        return false;
    }
    fs::copy(GameEngine::Rendering::Tests::GetAdapterShaderDir() / "Includes",
             shaderRoot / "Includes", fs::copy_options::recursive, ec);
    if (ec)
    {
        errors.push_back("staging shader includes: " + ec.message());
        return false;
    }
    return true;
}

} // namespace

// Mirrors Assets/Materials/UnlitWhite.material (World Debug and Shadow Only
// author the same surface).
TEST(ShippedSurfaceCompose, UnlitSolidComposesThroughRealPipeline)
{
    const fs::path materialsDir = TestPaths::StagedEngineAssetsDir() / "Materials";
    ASSERT_TRUE(fs::exists(materialsDir / "Surfaces" / "unlit_solid.glsl"))
        << "shipped surface not found under " << materialsDir.string();

    MaterialDocument doc{};
    doc.materialName = "Unlit White (M0)";
    doc.lightingModel = "Unlit";
    doc.surfaceShader = "Surfaces/unlit_solid.glsl";
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.CacheRoot = MakeTempCacheRoot();

    const auto result = BuildMaterialToShaderPackage(doc, materialsDir / "UnlitWhite.material",
                                                     "UnlitWhite", ctx, ShaderSourceKind::SpirV, MaterialKeyword::Instanced);

    std::error_code ec;
    fs::remove_all(ctx.CacheRoot, ec);

    for (const auto& e : result.errors)
    {
        if (e.find("shaderc is not available") != std::string::npos)
            GTEST_SKIP() << "shaderc not built into this target";
        ADD_FAILURE() << "Build error: " << e;
    }
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
}

// The CBT terrain surface, composed and compiled through the real pipeline.
//
// It reaches no shipped-asset scan: CBTRenderFeature::EnsureMaterial builds its MaterialDocument in
// C++, so nothing on disk names cbt_surface.glsl and no other suite compiles it. That left the
// engine's largest surface shader with a GLSL syntax/semantic gate of "launch the editor and see" —
// a terrain that fails to compile renders as the fallback material, which reads as a content
// problem rather than a broken shader.
//
// The document below MIRRORS CBTRenderFeature::EnsureMaterial; keep the two in step.
struct CbtComposeOutcome
{
    bool shadercMissing = false;
    bool blendWidthForced = false; // the staged copy really was rewritten (see below)
    bool success = false;
    bool vsEmpty = true;
    bool fsEmpty = true;
    std::vector<std::string> errors;
};

// forcedBlendMaterials > 0 rewrites CBT_MAX_BLEND_MATERIALS in the STAGED COPY of the shared width
// include before compiling, which is how a blend width other than the shipped one is proven to
// build without editing anything in the tree. 0 compiles the source exactly as it ships.
CbtComposeOutcome ComposeCbtTerrainSurface(int forcedBlendMaterials)
{
    CbtComposeOutcome out;
    const fs::path cbtShaders = fs::path(CBT_SHADER_SOURCE_DIR);
    if (!fs::exists(cbtShaders / "cbt_surface.glsl"))
    {
        out.errors.push_back("cbt_surface.glsl not found under " + cbtShaders.string());
        return out;
    }

    const fs::path shaderRoot = MakeTempCacheRoot() / "Shaders";
    if (!StageShaderRoot(cbtShaders, "CBT", shaderRoot, out.errors))
        return out;
    std::error_code ec;

    if (forcedBlendMaterials > 0)
    {
        // The width lives in the shared include both terrain surfaces take, not in the surface —
        // rewriting the surface here would silently match nothing and compile the shipped width.
        const fs::path staged = shaderRoot / "Includes" / "terrain_blend_width.glsl";
        const std::regex define(R"(#define[ \t]+CBT_MAX_BLEND_MATERIALS[ \t]+\d+)");
        const std::string src = ReadWholeFile(staged);
        // Reported rather than assumed: a pattern miss would compile the SHIPPED width and report
        // the result as the forced one — a gate that passes without ever testing what it claims.
        out.blendWidthForced = std::regex_search(src, define);
        if (out.blendWidthForced)
        {
            std::ofstream(staged, std::ios::binary | std::ios::trunc)
                << std::regex_replace(src, define,
                                      "#define CBT_MAX_BLEND_MATERIALS " +
                                          std::to_string(forcedBlendMaterials));
        }
    }

    MaterialDocument doc{};
    doc.materialName = "CBTTerrain/Default";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "CBT/cbt_surface.glsl";
    doc.vertexModifier = "CBT/cbt_vertex_modifier.glsl";
    doc.customVertexShader = true;
    doc.doubleSided = true;
    doc.alphaMode = MaterialAlphaMode::Opaque;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.PackageShaderDirs = {shaderRoot};
    ctx.CacheRoot = MakeTempCacheRoot();

    const auto result = BuildMaterialToShaderPackage(
        doc, shaderRoot / "CBTTerrain.material", "CBTTerrain", ctx, ShaderSourceKind::SpirV,
        MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL);

    fs::remove_all(ctx.CacheRoot, ec);
    fs::remove_all(shaderRoot.parent_path(), ec);

    for (const auto& e : result.errors)
    {
        if (e.find("shaderc is not available") != std::string::npos)
        {
            out.shadercMissing = true;
            return out;
        }
        out.errors.push_back(e);
    }
    out.success = result.success && result.package != nullptr;
    if (result.package != nullptr)
    {
        out.vsEmpty = result.package->stageBytes.at("vs").empty();
        out.fsEmpty = result.package->stageBytes.at("fs").empty();
    }
    return out;
}

TEST(ShippedSurfaceCompose, CbtTerrainSurfaceComposesThroughRealPipeline)
{
    const CbtComposeOutcome out = ComposeCbtTerrainSurface(0);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    EXPECT_FALSE(out.vsEmpty);
    EXPECT_FALSE(out.fsEmpty);
}

// CBT_MAX_BLEND_MATERIALS is a tuning knob, and a knob only ever compiled at its shipped value is
// decorative: the selection loop is bounded by it, so a width nobody has built at can fail to
// compile the moment someone tunes it — in the editor, as a terrain that silently falls back to the
// error material. Compile the widths a tuner would actually reach for.
//
// 4 is the documented escape hatch (selection disabled, every channel above the floor blends), so
// this also holds that claim in the shader's own comment to something executable.
TEST(ShippedSurfaceCompose, CbtTerrainSurfaceComposesAtEveryBlendWidth)
{
    for (const int blendMaterials : {2, 3, 4})
    {
        const CbtComposeOutcome out = ComposeCbtTerrainSurface(blendMaterials);
        if (out.shadercMissing)
            GTEST_SKIP() << "shaderc not built into this target";
        EXPECT_TRUE(out.blendWidthForced)
            << "K=" << blendMaterials
            << ": no CBT_MAX_BLEND_MATERIALS define found in the staged shader, so this compiled "
               "the shipped width instead of the one under test";
        for (const auto& e : out.errors)
            ADD_FAILURE() << "K=" << blendMaterials << " build error: " << e;
        EXPECT_TRUE(out.success) << "K=" << blendMaterials;
        EXPECT_FALSE(out.fsEmpty) << "K=" << blendMaterials;
    }
}

// The terrain grass surface, composed and compiled through the real pipeline.
//
// Same blind spot the CBT gate above closes, and for the same reason: TerrainGrassRenderFeature
// builds its MaterialDocument in C++, so no shipped .material names terrain_grass_surface.glsl and
// no asset scan reaches it. A GLSL error there falls back to the error material at runtime, which
// reads as missing grass rather than as a broken shader.
//
// It earns its own gate rather than riding the CBT one because the two surfaces are separate
// translation units that only share an include: the ground compiling proves nothing about whether
// the grass surface's own declarations agree with what that include expects of them — which is
// exactly the seam this suite should hold, now that the blend resolve crosses it.
//
// The document below MIRRORS TerrainGrassRenderFeature::EnsureMaterial; keep the two in step.
struct GrassComposeOutcome
{
    bool shadercMissing = false;
    bool success = false;
    bool vsEmpty = true;
    bool fsEmpty = true;
    std::vector<uint8_t> fsSpv;
    std::vector<std::string> errors;
};

// --- Fragment-kill scan over compiled SPIR-V ---------------------------------------------------
//
// A discard is a STATIC property of a fragment module. The hardware cannot retire the depth test
// ahead of a shader that might still kill the fragment, so a kill that is never TAKEN at runtime
// costs early-Z exactly as much as one that is: branching around it buys nothing back. That makes
// the compiled module — not the GLSL source, and not the runtime branch — the only place a
// "this variant earns early-Z" claim can be settled.
constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint32_t kSpvOpKill = 252u;                  // `discard`, SPIR-V <= 1.5
constexpr uint32_t kSpvOpTerminateInvocation = 4416u;  // `discard`, SPIR-V >= 1.6

struct SpvKillScan
{
    bool Valid = false;   // the module parsed as SPIR-V from magic to last instruction
    bool HasKill = false; // ... and carried OpKill / OpTerminateInvocation
    std::string Error;
};

// Walks the instruction stream rather than searching raw bytes: an opcode's numeric value can
// occur inside a literal, a result id or a debug string, and a substring hit on one of those would
// be a false positive reported as "the discard is still compiled in".
//
// Valid is reported separately from HasKill on purpose. A scanner that failed to parse would
// otherwise answer "no kill found" and turn the absence gate green without having looked — the
// exact false instrument this gate exists to rule out.
SpvKillScan ScanFragmentKills(const std::vector<uint8_t>& spv)
{
    SpvKillScan scan;
    if (spv.size() < 20u || (spv.size() % 4u) != 0u)
    {
        scan.Error = "not a whole number of SPIR-V words (" + std::to_string(spv.size()) + " bytes)";
        return scan;
    }
    std::vector<uint32_t> words(spv.size() / 4u);
    std::memcpy(words.data(), spv.data(), spv.size());
    if (words[0] != kSpvMagic)
    {
        scan.Error = "bad SPIR-V magic word";
        return scan;
    }
    // Words 0-4 are the header (magic, version, generator, id bound, schema).
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t opcode = words[i] & 0xFFFFu;
        const uint32_t wordCount = words[i] >> 16;
        if (wordCount == 0u || i + wordCount > words.size())
        {
            scan.Error = "malformed instruction stream at word " + std::to_string(i);
            return scan;
        }
        if (opcode == kSpvOpKill || opcode == kSpvOpTerminateInvocation)
        {
            scan.Valid = true;
            scan.HasKill = true;
            return scan;
        }
        i += wordCount;
    }
    scan.Valid = true;
    return scan;
}

// withVertexModifier = false composes the SURFACE alone. That is not the shipped document, and it
// is the only way to reach the fragment stage while the vertex defect above stands: the composer
// compiles vs first and bails, so a full-document gate cannot see a fragment-side error at all.
//
// userKeyword/alphaMode select which of the shipped grass documents is mirrored: the Blend
// default (nullptr keyword), or the GRASS_DITHER / GRASS_A2C Opaque documents — each compiles a
// different surface alpha path, so each needs its own gate.
GrassComposeOutcome ComposeTerrainGrassSurface(bool withVertexModifier, const char* userKeyword,
                                               MaterialAlphaMode alphaMode)
{
    GrassComposeOutcome out;
    const fs::path grassShaders = fs::path(TERRAIN_GRASS_SHADER_SOURCE_DIR);
    if (!fs::exists(grassShaders / "terrain_grass_surface.glsl"))
    {
        out.errors.push_back("terrain_grass_surface.glsl not found under " + grassShaders.string());
        return out;
    }

    // The surface names itself "TerrainGrass/terrain_grass_surface.glsl", so the module's shader
    // directory mounts under that name — the same shape Apps/Editor stages it at.
    const fs::path shaderRoot = MakeTempCacheRoot() / "Shaders";
    if (!StageShaderRoot(grassShaders, "TerrainGrass", shaderRoot, out.errors))
        return out;

    // The surface resolves the ground splat through TerrainGrass/grass_atlas_splat.glsl, which is
    // built on CBT/cbt_atlas.glsl — so the CBT mount has to be staged beside the grass one, exactly
    // as Apps/Editor stages both under Assets/Shaders. Without it this gate would fail on a missing
    // include and report it as a broken shader.
    {
        std::error_code cbtEc;
        fs::copy(fs::path(CBT_SHADER_SOURCE_DIR), shaderRoot / "CBT",
                 fs::copy_options::recursive, cbtEc);
        if (cbtEc)
        {
            out.errors.push_back("staging CBT shaders: " + cbtEc.message());
            return out;
        }
    }

    MaterialDocument doc{};
    doc.materialName = "Terrain/Grass";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "TerrainGrass/terrain_grass_surface.glsl";
    if (withVertexModifier)
        doc.vertexModifier = "TerrainGrass/terrain_grass_vertex_modifier.glsl";
    doc.alphaMode = alphaMode;
    doc.doubleSided = true;
    doc.properties["roughness"] = 0.9f;
    if (userKeyword)
        doc.keywords = {userKeyword};

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.PackageShaderDirs = {shaderRoot};
    ctx.CacheRoot = MakeTempCacheRoot();

    MaterialKeyword keywords = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
                               MaterialKeyword::Instanced | MaterialKeyword::IBL;
    if (withVertexModifier)
        keywords = keywords | MaterialKeyword::ProceduralVertexOutput;

    const auto result = BuildMaterialToShaderPackage(
        doc, shaderRoot / "TerrainGrass.material", "TerrainGrass", ctx, ShaderSourceKind::SpirV, keywords);

    std::error_code ec;
    fs::remove_all(ctx.CacheRoot, ec);
    fs::remove_all(shaderRoot.parent_path(), ec);

    for (const auto& e : result.errors)
    {
        if (e.find("shaderc is not available") != std::string::npos)
        {
            out.shadercMissing = true;
            return out;
        }
        out.errors.push_back(e);
    }
    out.success = result.success && result.package != nullptr;
    if (result.package != nullptr)
    {
        out.vsEmpty = result.package->stageBytes.at("vs").empty();
        const auto& fsSpv = result.package->stageBytes.at("fs");
        out.fsEmpty = fsSpv.empty();
        out.fsSpv = fsSpv; // the package dies with `result`; the kill scan runs on this copy
    }
    return out;
}

// The gate that covers the grass FRAGMENT surface, which is where its terrain blend lives. The
// surface composes without its vertex modifier for the reason given above — with the modifier
// attached the vertex stage fails first and the fragment stage is never compiled, so this is what
// actually holds terrain_grass_surface.glsl and the shared blend resolve it includes to compiling.
TEST(ShippedSurfaceCompose, TerrainGrassSurfaceFragmentComposesThroughRealPipeline)
{
    const GrassComposeOutcome out = ComposeTerrainGrassSurface(
        /*withVertexModifier=*/false, /*userKeyword=*/nullptr, MaterialAlphaMode::Blend);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    EXPECT_FALSE(out.fsEmpty) << "the grass surface produced no fragment SPIR-V";
}

// The dither-mode grass documents (TerrainGrassRenderFeature registers them beside the Blend
// one): each user keyword compiles a different surface alpha path — GRASS_DITHER's screen-door
// discard and GRASS_A2C's coverage alpha — so each path gets its own fragment gate.
TEST(ShippedSurfaceCompose, TerrainGrassDitherSurfaceFragmentComposesThroughRealPipeline)
{
    const GrassComposeOutcome out = ComposeTerrainGrassSurface(
        /*withVertexModifier=*/false, "GRASS_DITHER", MaterialAlphaMode::Opaque);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    EXPECT_FALSE(out.fsEmpty) << "the dither grass surface produced no fragment SPIR-V";
}

TEST(ShippedSurfaceCompose, TerrainGrassA2CSurfaceFragmentComposesThroughRealPipeline)
{
    const GrassComposeOutcome out = ComposeTerrainGrassSurface(
        /*withVertexModifier=*/false, "GRASS_A2C", MaterialAlphaMode::Opaque);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    EXPECT_FALSE(out.fsEmpty) << "the A2C grass surface produced no fragment SPIR-V";
}

// --- Early-Z: the opaque variant must compile no fragment kill at all --------------------------

// GRASS_OPAQUE exists for exactly one reason, and it is this assertion. A fragment module that
// carries a kill is put on late depth testing whether or not the kill is reachable, so the
// runtime-gated card cutout cost early-Z in every variant while it was compiled unconditionally —
// including the opaque one, where it is unreachable by construction (the mode is only resolved
// when no active grass row carries soft alpha, which leaves textureAlpha at 1.0 and no cutoff in
// [0,1] can exceed it). Only the instruction's ABSENCE restores early-Z, so only the compiled
// module can settle the claim; the GLSL source and the runtime branch both read the same either way.
TEST(ShippedSurfaceCompose, TerrainGrassOpaqueSurfaceCompilesNoFragmentKill)
{
    const GrassComposeOutcome out = ComposeTerrainGrassSurface(
        /*withVertexModifier=*/false, "GRASS_OPAQUE", MaterialAlphaMode::Opaque);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    ASSERT_FALSE(out.fsEmpty) << "the opaque grass surface produced no fragment SPIR-V";

    const SpvKillScan scan = ScanFragmentKills(out.fsSpv);
    ASSERT_TRUE(scan.Valid) << "could not parse the fragment module: " << scan.Error;
    EXPECT_FALSE(scan.HasKill)
        << "the GRASS_OPAQUE fragment module still carries OpKill/OpTerminateInvocation, so the "
           "hardware keeps it on late-Z and the opaque draw mode earns no early-Z";
}

// The positive control for the gate above, and what keeps it from being vacuous: the SAME compose
// harness and the SAME scanner over the keyword-free variant, whose only discard is the card
// cutout that GRASS_OPAQUE compiles out. If the scanner stopped recognising a kill, or the harness
// stopped applying user keywords at all, this test fails — where the absence gate above would
// quietly pass.
TEST(ShippedSurfaceCompose, TerrainGrassDefaultSurfaceStillCompilesTheCardCutoutKill)
{
    const GrassComposeOutcome out = ComposeTerrainGrassSurface(
        /*withVertexModifier=*/false, /*userKeyword=*/nullptr, MaterialAlphaMode::Blend);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    ASSERT_FALSE(out.fsEmpty) << "the default grass surface produced no fragment SPIR-V";

    const SpvKillScan scan = ScanFragmentKills(out.fsSpv);
    ASSERT_TRUE(scan.Valid) << "could not parse the fragment module: " << scan.Error;
    EXPECT_TRUE(scan.HasKill)
        << "the keyword-free grass surface compiled no fragment kill: either the card cutout has "
           "left every variant, or this scan is not detecting one";
}

// The shipped document, vertex modifier included — the variant TerrainGrassRenderFeature actually
// registers. This is the only suite that compiles the shipped grass vertex modifier, so any compose
// or compile error is a failure: the modifier reads its wind phase from inst.deformationTimeSeconds,
// which adapter_vertex.glsl stamps from the LightUBO it declares for both modifier forms, so the
// extended form it composes as can reach it.
TEST(ShippedSurfaceCompose, TerrainGrassMaterialComposesThroughRealPipeline)
{
    const GrassComposeOutcome out = ComposeTerrainGrassSurface(
        /*withVertexModifier=*/true, /*userKeyword=*/nullptr, MaterialAlphaMode::Blend);
    if (out.shadercMissing)
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : out.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(out.success);
    EXPECT_FALSE(out.vsEmpty);
    EXPECT_FALSE(out.fsEmpty);
}

// unlit_solid.glsl is staged from two roots (project-side Assets/Materials and
// the engine shader tree); which copy resolves depends on the resolution root.
// The 2026-07 boot failure was exactly these drifting apart — one copy fixed,
// the other still declaring its own layout() binding. Pin them byte-identical.
TEST(ShippedSurfaceCompose, UnlitSolidCopiesAreIdentical)
{
    const fs::path assetsCopy =
        TestPaths::StagedEngineAssetsDir() / "Materials" / "Surfaces" / "unlit_solid.glsl";
    const fs::path engineCopy =
        TestPaths::StagedRenderingShadersDir() / "Surfaces" / "unlit_solid.glsl";
    ASSERT_TRUE(fs::exists(assetsCopy));
    ASSERT_TRUE(fs::exists(engineCopy));
    EXPECT_EQ(ReadWholeFile(assetsCopy), ReadWholeFile(engineCopy))
        << "the two staged unlit_solid.glsl copies have drifted";
}

// The placement compute was the one grass shader NOTHING compiled until an editor booted. The two
// suites on this module's map row reach the other two: this file composes the surface and the
// vertex modifier through the real pipeline, and CBTLayoutTests PARSES all three for std430 parity
// — parsing a struct out of a file is not compiling the file. So a syntax error, an undeclared
// identifier or a bad include in terrain_grass_place.comp was a green build and a runtime
// "placement shader compile failed" warning, after which the field silently has no grass.
//
// The request below mirrors TerrainGrassRenderFeature's own, including the three include roots it
// passes: the staged Assets/Shaders root (for "TerrainGrass/..." and "Includes/..."), the CBT
// directory (cbt_atlas.glsl is included BARE, which is why it needs its own root), and the grass
// directory itself (grass_placement.glsl, likewise bare).
TEST(ShippedSurfaceCompose, TerrainGrassPlacementComputeCompilesThroughRealPipeline)
{
    const fs::path grassShaders = fs::path(TERRAIN_GRASS_SHADER_SOURCE_DIR);
    ASSERT_TRUE(fs::exists(grassShaders / "terrain_grass_place.comp"))
        << "placement compute not found under " << grassShaders.string();

    std::vector<std::string> errors;
    const fs::path shaderRoot = MakeTempCacheRoot() / "Shaders";
    ASSERT_TRUE(StageShaderRoot(grassShaders, "TerrainGrass", shaderRoot, errors))
        << (errors.empty() ? std::string("staging failed") : errors.front());

    ShaderProgramCompileRequest req{};
    req.debugName = "terrain_grass_place";
    req.baseDirectory = shaderRoot / "TerrainGrass";
    req.cacheRoot = MakeTempCacheRoot();
    req.includeDirs = {shaderRoot, fs::path(CBT_SHADER_SOURCE_DIR), shaderRoot / "TerrainGrass"};
    ShaderStageCompileSpec stage{};
    stage.stage = "cs";
    stage.sourcePath = "terrain_grass_place.comp";
    stage.entryPoint = "main";
    req.stages.push_back(std::move(stage));

    ShaderProgramCompileResult result{};
    std::string compileError;
    const bool ok = ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &compileError);

    std::error_code ec;
    fs::remove_all(req.cacheRoot, ec);
    fs::remove_all(shaderRoot.parent_path(), ec);

    if (!ok && compileError.find("shaderc is not available") != std::string::npos)
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(ok) << "placement compute failed to compile: " << compileError;
    const auto it = result.stageBytes.find("cs");
    ASSERT_NE(it, result.stageBytes.end()) << "no compute stage in the compiled program";
    EXPECT_FALSE(it->second.empty()) << "the placement compute produced no SPIR-V";
}
