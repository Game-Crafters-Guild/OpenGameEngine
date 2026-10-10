// Named texture slots (extensibility slice K): ShaderComposer::ResolveTextureSlots
// scans a surface's `// @texture <name>` declarations, assigns each an ordinal
// (well-known names keep their canonical slot; user names pack into the lowest
// free ordinal), and rejects a surface that mixes user names with raw-ordinal
// access. Compose emits the `#define GE_TEXSLOT_<name> <ordinal>` macros into the
// fragment source.

#include <gtest/gtest.h>

#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "TestUtils.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

int OrdinalOf(const TextureSlotResolution& r, const std::string& name)
{
    for (const auto& [n, o] : r.DeclaredSlots)
        if (n == name)
            return static_cast<int>(o);
    return -1;
}

void WriteFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

fs::path UniqueTempDir(const char* prefix)
{
    static std::atomic<uint32_t> counter{0};
    fs::path root = fs::temp_directory_path() /
                    (std::string(prefix) + "_" +
                     std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                     std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    return root;
}
} // namespace

// ---- ResolveTextureSlots (pure function) ----

TEST(ResolveTextureSlots, WellKnownNamesKeepCanonicalOrdinals)
{
    const std::string src =
        "// @texture albedoMap srgb\n"
        "// @texture normalMap linear\n"
        "// @texture metallicRoughnessMap linear\n"
        "void EvaluateSurface() {}\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_TRUE(r.HasDeclarations);
    EXPECT_FALSE(r.Rejected);
    EXPECT_EQ(OrdinalOf(r, "albedoMap"), 0);
    EXPECT_EQ(OrdinalOf(r, "normalMap"), 1);
    EXPECT_EQ(OrdinalOf(r, "metallicRoughnessMap"), 2);
}

// The water example: a user name packs into the lowest ordinal not claimed by a
// declared well-known name (albedoMap=0, normalMap=1 => flowMask=2).
TEST(ResolveTextureSlots, UserNamePacksIntoLowestFreeOrdinal)
{
    const std::string src =
        "// @texture albedoMap srgb\n"
        "// @texture normalMap linear\n"
        "// @texture flowMask linear\n"
        "vec4 f() { return texture(GE_USER_TEXTURE(flowMask), vec2(0)); }\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_FALSE(r.Rejected);
    EXPECT_EQ(OrdinalOf(r, "albedoMap"), 0);
    EXPECT_EQ(OrdinalOf(r, "normalMap"), 1);
    EXPECT_EQ(OrdinalOf(r, "flowMask"), 2);
}

// The shipped standard surface declares its whole texture set. The six ladder names keep their
// canonical ordinals, and heightMap, the surface's own name, packs onto the first free one: a
// surface declaring heightMap alone would pack it onto ordinal 0 and alias the albedo.
TEST(ResolveTextureSlots, StandardSurfaceDeclaresHeightMapOnOrdinalSix)
{
    const fs::path surface = fs::path(RENDERING_SOURCE_DIR) / "Shaders" / "Surfaces" / "standard_pbr.glsl";
    std::ifstream in(surface, std::ios::binary);
    ASSERT_TRUE(in.good()) << "cannot read " << surface.string();
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const auto r = ShaderComposer::ResolveTextureSlots(source);
    ASSERT_FALSE(r.Rejected) << r.RejectReason;
    ASSERT_TRUE(r.HasDeclarations);
    EXPECT_EQ(r.DeclaredSlots.size(), 7u);
    EXPECT_EQ(OrdinalOf(r, "albedoMap"), 0);
    EXPECT_EQ(OrdinalOf(r, "normalMap"), 1);
    EXPECT_EQ(OrdinalOf(r, "metallicRoughnessMap"), 2);
    EXPECT_EQ(OrdinalOf(r, "emissiveMap"), 3);
    EXPECT_EQ(OrdinalOf(r, "aoMap"), 4);
    EXPECT_EQ(OrdinalOf(r, "coatNormalMap"), 5);
    EXPECT_EQ(OrdinalOf(r, "heightMap"), 6);
}

// User names sort lexicographically into the free ordinals, deterministically.
TEST(ResolveTextureSlots, MultipleUserNamesPackLexicographically)
{
    const std::string src =
        "// @texture albedoMap srgb\n" // claims ordinal 0
        "// @texture zebra linear\n"
        "// @texture alpha linear\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_FALSE(r.Rejected);
    EXPECT_EQ(OrdinalOf(r, "albedoMap"), 0);
    EXPECT_EQ(OrdinalOf(r, "alpha"), 1); // lexicographically first user name
    EXPECT_EQ(OrdinalOf(r, "zebra"), 2);
}

// A surface with no @texture tags is a legacy surface: no declarations, no
// rejection — the fixed ladder handles it downstream unchanged.
TEST(ResolveTextureSlots, LegacySurfaceHasNoDeclarations)
{
    const std::string src = "vec4 EvaluateSurface() { return texture(albedoMap, vec2(0)); }\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_FALSE(r.HasDeclarations);
    EXPECT_FALSE(r.Rejected);
    EXPECT_TRUE(r.DeclaredSlots.empty());
}

// A user @texture name alongside the triplanar collapse helper would silently
// alias a raw ordinal — hard rejection.
TEST(ResolveTextureSlots, RejectsUserNameMixedWithTriplanarHelper)
{
    const std::string src =
        "// @texture flowMask linear\n"
        "vec4 f() { return GE_SampleTriplanarSlot(2u, vec2(0)); }\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_TRUE(r.Rejected);
    EXPECT_FALSE(r.RejectReason.empty());
}

// A numeric GE_SLOT_TEX(<n>) is raw-ordinal access; mixing it with a user name
// is rejected the same way.
TEST(ResolveTextureSlots, RejectsUserNameMixedWithNumericSlotTex)
{
    const std::string src =
        "// @texture flowMask linear\n"
        "vec4 f() { return texture(sampler2D(GE_SLOT_TEX(2), GE_SLOT_SAMPLER(2)), vec2(0)); }\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_TRUE(r.Rejected);
}

// The self-contained GE_USER_TEXTURE(name) form expands to a macro argument, not
// a digit, so it is NOT flagged as raw-ordinal access.
TEST(ResolveTextureSlots, SelfContainedUserSurfaceNotRejected)
{
    const std::string src =
        "// @texture flowMask linear\n"
        "vec4 f() { return texture(GE_USER_TEXTURE(flowMask), vec2(0)); }\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_FALSE(r.Rejected);
    EXPECT_EQ(OrdinalOf(r, "flowMask"), 0); // only slot -> lowest free ordinal
}

// More declared user slots than the 8-slot window can hold is a hard rejection
// (the 8->16 lift is a separate slice).
TEST(ResolveTextureSlots, RejectsWhenExceedingSlotWindow)
{
    std::string src;
    for (int i = 0; i < 9; ++i)
        src += "// @texture userTex" + std::to_string(i) + " linear\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_TRUE(r.Rejected);
}

// A declared name is deduped (only its first declaration counts).
TEST(ResolveTextureSlots, DuplicateDeclarationIsDeduped)
{
    const std::string src =
        "// @texture flowMask linear\n"
        "// @texture flowMask srgb\n";
    const auto r = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_FALSE(r.Rejected);
    EXPECT_EQ(r.DeclaredSlots.size(), 1u);
    EXPECT_EQ(OrdinalOf(r, "flowMask"), 0);
}

// ---- Compose: GE_TEXSLOT_* emission + rejection ----

TEST(ComposeTextureSlots, EmitsGeTexslotDefinesIntoFragmentSource)
{
    const auto adapterDir = Tests::GetAdapterShaderDir();
    const fs::path materialDir = UniqueTempDir("ge_kn_texslot_emit");
    WriteFile(materialDir / "Surfaces" / "kn_tex_surface.glsl",
              "// @texture albedoMap srgb\n"
              "// @texture normalMap linear\n"
              "// @texture flowMask linear\n"
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
              "    SurfaceOutput o = DefaultSurfaceOutput();\n"
              "    o.baseColor = texture(GE_USER_TEXTURE(flowMask), sIn.uv0).rgb;\n"
              "    return o;\n"
              "}\n");

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/kn_tex_surface.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;

    std::vector<std::string> errors;
    const auto result = ShaderComposer::Compose(doc, key, materialDir, ctx, &errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());

    EXPECT_NE(result.fragmentSource.find("#define GE_TEXSLOT_albedoMap 0"), std::string::npos);
    EXPECT_NE(result.fragmentSource.find("#define GE_TEXSLOT_normalMap 1"), std::string::npos);
    EXPECT_NE(result.fragmentSource.find("#define GE_TEXSLOT_flowMask 2"), std::string::npos);

    std::error_code ec;
    fs::remove_all(materialDir, ec);
}

TEST(ComposeTextureSlots, RejectedSurfaceFailsCompose)
{
    const auto adapterDir = Tests::GetAdapterShaderDir();
    const fs::path materialDir = UniqueTempDir("ge_kn_texslot_reject");
    WriteFile(materialDir / "Surfaces" / "kn_reject_surface.glsl",
              "// @texture flowMask linear\n"
              "vec4 f() { return GE_SampleTriplanarSlot(2u, vec2(0)); }\n"
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n");

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/kn_reject_surface.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;

    std::vector<std::string> errors;
    const auto result = ShaderComposer::Compose(doc, key, materialDir, ctx, &errors);
    EXPECT_FALSE(result.IsValid());
    EXPECT_FALSE(errors.empty());

    std::error_code ec;
    fs::remove_all(materialDir, ec);
}

// ---- User keyword emission (slice N): GE_USER_<NAME> into the source ----

TEST(ComposeUserKeywords, EmitsNamespacedUserDefines)
{
    const auto adapterDir = Tests::GetAdapterShaderDir();
    const fs::path materialDir = UniqueTempDir("ge_n_kw_emit");
    WriteFile(materialDir / "Surfaces" / "n_kw_surface.glsl",
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
              "    SurfaceOutput o = DefaultSurfaceOutput();\n"
              "#ifdef GE_USER_FLOW_MODE\n"
              "    o.baseColor = vec3(1.0, 0.0, 0.0);\n"
              "#endif\n"
              "    return o;\n"
              "}\n");

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/n_kw_surface.glsl";
    doc.keywords = {"FLOW_MODE", "HIGH_DETAIL"};

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;

    std::vector<std::string> errors;
    const auto result = ShaderComposer::Compose(doc, key, materialDir, ctx, &errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());

    // Namespaced defines emitted; the raw (unprefixed) names never are.
    EXPECT_NE(result.fragmentSource.find("#define GE_USER_FLOW_MODE"), std::string::npos);
    EXPECT_NE(result.fragmentSource.find("#define GE_USER_HIGH_DETAIL"), std::string::npos);
    EXPECT_EQ(result.fragmentSource.find("#define FLOW_MODE\n"), std::string::npos);

    std::error_code ec;
    fs::remove_all(materialDir, ec);
}

// Regression: a doc comment QUOTING the tag mid-prose (the shipped water surface
// cites "e.g. `// @texture flowMask linear`") must not mint a phantom user name.
// The unanchored scan did, which combined with the prose triplanar mention below
// to hard-reject every ER water compose.
TEST(ResolveTextureSlots, ProseMentionOfTagMintsNothing)
{
    const std::string src =
        "// Declares its inputs via named slots, e.g. `// @texture flowMask linear`\n"
        "// @texture albedoMap srgb\n"
        "// @texture normalMap linear\n"
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n";
    const auto res = ShaderComposer::ResolveTextureSlots(src);
    ASSERT_FALSE(res.Rejected) << res.RejectReason;
    ASSERT_EQ(res.DeclaredSlots.size(), 2u); // albedoMap + normalMap only
    EXPECT_EQ(res.DeclaredSlots[0].first, "albedoMap");
    EXPECT_EQ(res.DeclaredSlots[1].first, "normalMap");
}

// Regression: prose mentions of triplanar_pbr / GE_SLOT_TEX(0) in comments are
// not raw-ordinal USAGE — the scan runs on comment-stripped source.
TEST(ResolveTextureSlots, CommentMentionsOfRawOrdinalsDoNotReject)
{
    const std::string src =
        "// See triplanar_pbr.glsl for the slot conventions; don't use\n"
        "// GE_SLOT_TEX(0) directly in user surfaces.\n"
        "/* also fine in block comments: triplanar_pbr, GE_SLOT_TEX(2) */\n"
        "// @texture flowMask linear\n"
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n";
    const auto res = ShaderComposer::ResolveTextureSlots(src);
    ASSERT_FALSE(res.Rejected) << res.RejectReason;
    ASSERT_EQ(res.DeclaredSlots.size(), 1u);
    EXPECT_EQ(res.DeclaredSlots[0].first, "flowMask");
}

// Real raw-ordinal usage outside comments still rejects a user-name surface.
TEST(ResolveTextureSlots, RealRawOrdinalUsageStillRejects)
{
    const std::string src =
        "// @texture flowMask linear\n"
        "#include \"Surfaces/triplanar_pbr.glsl\"\n"
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n";
    const auto res = ShaderComposer::ResolveTextureSlots(src);
    EXPECT_TRUE(res.Rejected);
}
