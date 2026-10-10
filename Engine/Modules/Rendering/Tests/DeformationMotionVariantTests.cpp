// The composed motion variant: one variant of a deforming material, generated from the same
// adapter every other pass composes, that evaluates the vertex modifier at BOTH deformation
// endpoints and writes the motion payload instead of shading.
//
// Three contracts, each with its own instrument:
//   - shape: the motion variant attaches exactly ONE colour target, and declares neither the
//     shading output nor the reflection G-buffer slices;
//   - coverage: it decides coverage from the same declared properties, and through the same
//     discard block, as the depth-only variant of the same material — a motion vector written
//     where the prepass wrote no depth hands the resolve an exact vector for a surface that was
//     never shaded;
//   - endpoints: the vertex stage fetches a previous InstanceData and runs the modifier against
//     it, so the delta is a second full evaluation rather than a frame-delta guess.
//
// Nothing draws this variant yet. That is deliberate: the producer is chosen by measurement in a
// later slice, and a compile regression in the variant surfaces here rather than as a wrong pixel.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <string>
#include <vector>

#ifndef RENDERING_SOURCE_DIR
#error "RENDERING_SOURCE_DIR must be defined by CMake (rendering_test_shader_paths)"
#endif
#ifndef EZTREE_PACKAGE_SHADERS_DIR
#error "EZTREE_PACKAGE_SHADERS_DIR must be defined by CMake (target_compile_definitions)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

// The world draw ORs Instanced onto the pass keywords and the shipped forward graphs declare
// ForwardPlus + Shadows for the world pass; the depth prepass composes the depth-only fragment.
// The motion variant carries neither lighting keyword — it shades nothing.
constexpr MaterialKeyword kWorldPass =
    MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
constexpr MaterialKeyword kDepthPass = MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment;
constexpr MaterialKeyword kMotionPass = MaterialKeyword::Instanced | MaterialKeyword::MotionVectors;

fs::path MakeTempCacheRoot()
{
    static std::atomic<uint32_t> counter{0};
    return fs::temp_directory_path() /
           ("ge_motion_variant_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(counter.fetch_add(1)));
}

MaterialBuildResult Build(const MaterialDocument& doc, MaterialKeyword keywords)
{
    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
    ctx.PackageShaderDirs = {fs::path(EZTREE_PACKAGE_SHADERS_DIR)};
    ctx.CacheRoot = MakeTempCacheRoot();

    auto result = BuildMaterialToShaderPackage(
        doc, TestPaths::StagedEngineAssetsDir() / "Materials" / "MotionVariantProbe.material",
        "MotionVariantProbe", ctx, ShaderSourceKind::SpirV, keywords);
    std::error_code ec;
    fs::remove_all(ctx.CacheRoot, ec);
    return result;
}

// Mirrors Packages/eztree/Assets/Materials/EZTree/EZTree_Bark.material: the shipped PBR
// surface with the package's wind modifier — the simple ModifyVertex form the motion variant
// composes.
MaterialDocument WindTreeDocument()
{
    MaterialDocument doc{};
    doc.materialName = "MotionVariantWindTree";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";
    return doc;
}

// A stock material on the engine's built-in surface: no vertex modifier, so no endpoint.
MaterialDocument StockDocument()
{
    MaterialDocument doc{};
    doc.materialName = "MotionVariantStock";
    doc.lightingModel = "StandardPBR";
    return doc;
}

#define SKIP_WITHOUT_SHADERC(result)                                     \
    do                                                                   \
    {                                                                    \
        for (const auto& e : (result).errors)                            \
            if (e.find("shaderc is not available") != std::string::npos) \
                GTEST_SKIP() << "shaderc not built into this target";    \
    } while (0)

const StageMeta* FragmentStage(const MaterialBuildResult& result)
{
    if (result.package == nullptr)
        return nullptr;
    const auto it = result.package->meta.Stages.find("fs");
    return it == result.package->meta.Stages.end() ? nullptr : &it->second;
}

// shaderc runs at optimization level zero, so OpName debug strings survive and a GLSL identifier
// that reached the compiled stage is searchable in its SPIR-V.
bool StageSpvNames(const MaterialBuildResult& result, const char* stage, const std::string& needle)
{
    if (result.package == nullptr)
        return false;
    const auto it = result.package->stageBytes.find(stage);
    if (it == result.package->stageBytes.end())
        return false;
    const auto& spv = it->second;
    return std::search(spv.begin(), spv.end(), needle.begin(), needle.end()) != spv.end();
}

MaterialBuildResult BuildOrFail(const MaterialDocument& doc, MaterialKeyword keywords,
                                const char* label)
{
    auto result = Build(doc, keywords);
    for (const auto& e : result.errors)
        ADD_FAILURE() << label << ": " << e;
    EXPECT_TRUE(result.success) << label;
    EXPECT_NE(result.package, nullptr) << label;
    return result;
}

// A varying interface slot. Several narrow variables share one location as long as their
// component spans are disjoint, so a location alone does not identify what the two stages have
// to agree on.
struct Slot
{
    uint32_t Location = 0;
    uint32_t Component = 0;

    bool operator==(const Slot& other) const
    {
        return Location == other.Location && Component == other.Component;
    }
};

std::ostream& operator<<(std::ostream& os, const Slot& slot)
{
    return os << "location " << slot.Location << ", component " << slot.Component;
}

// Distinct from every real slot, so "this variable is gone" is an equality assertion like the
// others rather than a separate spelling.
constexpr Slot kAbsentSlot{0xFFFFFFFFu, 0xFFFFFFFFu};

Slot SlotOf(const std::vector<StageIO>& io, const char* name)
{
    for (const auto& v : io)
        if (v.Name == name)
            return Slot{v.Location, v.Component};
    return kAbsentSlot;
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

std::vector<std::string> DeclaredPropertyNames(const MaterialBuildResult& result)
{
    std::vector<std::string> names;
    if (result.package == nullptr)
        return names;
    names.reserve(result.package->meta.DeclaredProperties.size());
    for (const auto& p : result.package->meta.DeclaredProperties)
        names.push_back(p.Name);
    std::sort(names.begin(), names.end());
    return names;
}

// The compatibility profile is process-global state (the variant hash forks on it), so a row that
// composes the compat form restores whatever the profile was.
struct ScopedCompatShaderProfile
{
    bool Previous;
    ScopedCompatShaderProfile() : Previous(IsCompatShaderProfile()) { SetCompatShaderProfile(true); }
    ~ScopedCompatShaderProfile() { SetCompatShaderProfile(Previous); }
};

} // namespace

// The shape contract. A motion variant is a depth-family variant carrying one colour
// attachment: one output at location 0, and none of the shading interface. A second output
// would be a silently unwritten attachment (a pipeline interned with fewer outputs than the
// pass attaches is legal and produces no error), and a missing one writes nowhere at all.
TEST(DeformationMotionVariant, MotionVariantDeclaresExactlyOneColourOutput)
{
    const auto motion = Build(WindTreeDocument(), kMotionPass);
    SKIP_WITHOUT_SHADERC(motion);
    for (const auto& e : motion.errors)
        ADD_FAILURE() << "motion variant: " << e;
    ASSERT_TRUE(motion.success);
    ASSERT_NE(motion.package, nullptr);

    const StageMeta* fs = FragmentStage(motion);
    ASSERT_NE(fs, nullptr) << "the motion variant composed no fragment stage";
    ASSERT_EQ(fs->Outputs.size(), 1u)
        << "the motion variant must attach exactly one colour target; it declares "
        << fs->Outputs.size();
    EXPECT_EQ(fs->Outputs[0].Location, 0u);
    EXPECT_EQ(fs->Outputs[0].Name, "oMotion")
        << "the single output is the motion payload, not a shading output";
    // A declared output that nothing writes is still a declared output, so the count above
    // cannot see an emit that went missing. This is what does.
    EXPECT_TRUE(StageSpvNames(motion, "fs", "GE_MotionVectorPayload"))
        << "the motion fragment attaches a target but exports no payload into it";
}

// The positive controls for the row above, so its count is read against the two variants of the
// same material whose output counts are already settled: the depth-only variant attaches none,
// the world variant attaches the shading output.
TEST(DeformationMotionVariant, DepthOnlyAttachesNoColourTargetAndTheWorldVariantAttachesOne)
{
    const auto depth = Build(WindTreeDocument(), kDepthPass);
    SKIP_WITHOUT_SHADERC(depth);
    ASSERT_TRUE(depth.success);
    const StageMeta* depthFs = FragmentStage(depth);
    ASSERT_NE(depthFs, nullptr);
    EXPECT_EQ(depthFs->Outputs.size(), 0u) << "the depth-only variant attaches no colour target";

    const auto world = BuildOrFail(WindTreeDocument(), kWorldPass, "world variant");
    const StageMeta* worldFs = FragmentStage(world);
    ASSERT_NE(worldFs, nullptr);
    ASSERT_EQ(worldFs->Outputs.size(), 1u);
    EXPECT_EQ(worldFs->Outputs[0].Name, "oColor");
}

// The two stages are compiled separately, so nothing but this check pairs them before a pipeline
// is created: the endpoint the vertex stage emits must arrive at the slots the fragment stage
// reads it from. Slot, not location: the previous endpoint rides the four smooth components the
// 0..15 block leaves free, so an interface slot here is a (location, component) pair and a
// location alone does not identify one.
TEST(DeformationMotionVariant, TheEndpointVaryingsMatchAcrossTheTwoStages)
{
    const auto motion = Build(WindTreeDocument(), kMotionPass);
    SKIP_WITHOUT_SHADERC(motion);
    ASSERT_TRUE(motion.success);
    ASSERT_NE(motion.package, nullptr);

    const auto vsIt = motion.package->meta.Stages.find("vs");
    ASSERT_NE(vsIt, motion.package->meta.Stages.end());
    const StageMeta* fs = FragmentStage(motion);
    ASSERT_NE(fs, nullptr);

    const Slot kPacked[] = {
        {0, 2},  // vMotionPrevClipXY beside vUV0 (vec2 at components 0..1)
        {2, 3},  // vMotionPrevClipZ  beside vPosWS (vec3 at components 0..2)
        {15, 3}, // vMotionPrevClipW  beside vPosRel (vec3 at components 0..2)
    };
    const char* const kNames[] = {"vMotionPrevClipXY", "vMotionPrevClipZ", "vMotionPrevClipW"};

    for (size_t i = 0; i < std::size(kNames); ++i)
    {
        EXPECT_EQ(SlotOf(vsIt->second.Outputs, kNames[i]), kPacked[i])
            << kNames[i] << " does not leave the vertex stage in the slot it is packed into";
        EXPECT_EQ(SlotOf(fs->Inputs, kNames[i]), kPacked[i])
            << "the fragment stage does not read " << kNames[i]
            << " where the vertex stage writes it";
    }

    // The current endpoint is not a varying at all: the fragment recovers it from gl_FragCoord,
    // which is what freed the two locations the packing needed.
    EXPECT_EQ(SlotOf(vsIt->second.Outputs, "vMotionCurrClip"), kAbsentSlot)
        << "the motion variant still emits a current-endpoint varying";
    EXPECT_EQ(SlotOf(fs->Inputs, "vMotionCurrClip"), kAbsentSlot);

    // The hosts stay where they were; the packing is only legal because the components it takes
    // sit beside them.
    EXPECT_EQ(SlotOf(vsIt->second.Outputs, "vUV0"), (Slot{0, 0}));
    EXPECT_EQ(SlotOf(vsIt->second.Outputs, "vPosWS"), (Slot{2, 0}));
    EXPECT_EQ(SlotOf(vsIt->second.Outputs, "vPosRel"), (Slot{15, 0}));
}

// The varying budget, which is the whole point of the packing. A varying interface costs
// (highest location + 1) * 4 components on both sides, so one location above 15 would put the
// motion variant at >= 72 against the 64 Vulkan guarantees — a device floor raised by a variant
// nothing else pays for. Every keyword combination that can reach the variant is checked, because
// the free components are only free in all of them: HAS_UV2..7 fill locations 8..13, the
// crossfade code shares location 14, and the alpha-test and compatibility profiles change the
// interpolation qualifier the packed components have to match.
TEST(DeformationMotionVariant, TheMotionVariantDeclaresNoVaryingAboveLocation15)
{
    struct Case
    {
        MaterialKeyword Keywords;
        bool Compat;
        const char* Label;
    };
    const Case cases[] = {
        {kMotionPass, false, "opaque"},
        {kMotionPass | MaterialKeyword::AlphaTest, false, "masked"},
        {kMotionPass | MaterialKeyword::LodCrossfade, false, "opaque + crossfade"},
        {kMotionPass | MaterialKeyword::AlphaTest | MaterialKeyword::LodCrossfade, false,
         "masked + crossfade"},
        {MaterialKeyword::MotionVectors, false, "push-constant fetch form"},
        {kMotionPass, true, "compatibility profile"},
    };
    for (const Case& c : cases)
    {
        MaterialBuildResult motion;
        if (c.Compat)
        {
            ScopedCompatShaderProfile compat;
            motion = Build(WindTreeDocument(), c.Keywords);
        }
        else
        {
            motion = Build(WindTreeDocument(), c.Keywords);
        }
        SKIP_WITHOUT_SHADERC(motion);
        for (const auto& e : motion.errors)
            ADD_FAILURE() << c.Label << ": " << e;
        ASSERT_TRUE(motion.success) << c.Label;
        ASSERT_NE(motion.package, nullptr) << c.Label;

        const auto vsIt = motion.package->meta.Stages.find("vs");
        ASSERT_NE(vsIt, motion.package->meta.Stages.end()) << c.Label;
        const StageMeta* fs = FragmentStage(motion);
        ASSERT_NE(fs, nullptr) << c.Label;
        ASSERT_FALSE(vsIt->second.Outputs.empty()) << c.Label << ": the vertex stage reflects no outputs";
        ASSERT_FALSE(fs->Inputs.empty()) << c.Label << ": the fragment stage reflects no inputs";

        // The vertex stage's INPUTS are vertex-attribute locations, not varyings, and the mesh
        // layout puts aUV7 at 15 — so only the outputs and the fragment inputs are the interface
        // this bounds.
        for (const auto& v : vsIt->second.Outputs)
            EXPECT_LE(v.Location, 15u)
                << c.Label << ": vertex output " << v.Name << " sits at location " << v.Location;
        for (const auto& v : fs->Inputs)
            EXPECT_LE(v.Location, 15u)
                << c.Label << ": fragment input " << v.Name << " sits at location " << v.Location;
    }
}

// The per-view block is the producer's upload contract: S5a-2 mirrors it in C++, and nothing else
// pins where each member sits or which stages bind it. std140 puts the mat4 at 0 and the three
// vec4s at 64, 80 and 96, so the block is 112 bytes; both stages declare it, because the vertex
// stage projects the previous endpoint through it and the fragment stage places the current
// endpoint with it. A member that moved, grew or dropped a stage would otherwise be found by the
// first producer upload that lands in the wrong lane.
TEST(DeformationMotionVariant, TheMotionBlockLayoutIsTheProducersUploadContract)
{
    const auto motion = Build(WindTreeDocument(), kMotionPass);
    SKIP_WITHOUT_SHADERC(motion);
    for (const auto& e : motion.errors)
        ADD_FAILURE() << "motion variant: " << e;
    ASSERT_TRUE(motion.success);
    ASSERT_NE(motion.package, nullptr);

    constexpr uint32_t kMotionBlockBinding = 45;
    const DescriptorBindingMeta* block = nullptr;
    for (const auto& set : motion.package->meta.Sets)
        if (set.Set == 0)
            for (const auto& binding : set.Bindings)
                if (binding.Binding == kMotionBlockBinding)
                    block = &binding;
    ASSERT_NE(block, nullptr) << "the motion variant declares no set-0 binding " << kMotionBlockBinding;
    // Reflection names a block by its INSTANCE name, and the world pass's set-0 resolution keys
    // on that reflected name (Cam, Light, MaterialParams) - so this, not the type name
    // DeformationMotionParams, is what the producer registers its provider under.
    EXPECT_EQ(block->Name, "MotionParams");
    EXPECT_EQ(block->Type, ShaderMetaBindingType::kUniformBuffer);
    // The meta's stage mask is reflection's own encoding (vertex = bit 0, fragment = bit 1), the
    // one ShaderMetaJson serialises; it does not share Device.h's pipeline stage bits.
    constexpr uint32_t kMetaStageVertex = 1u << 0;
    constexpr uint32_t kMetaStageFragment = 1u << 1;
    constexpr uint32_t kBothStages = kMetaStageVertex | kMetaStageFragment;
    EXPECT_EQ(block->StagesMask & kBothStages, kBothStages)
        << "the block is not bound by both stages (mask " << block->StagesMask << ")";

    ASSERT_TRUE(block->Block.has_value());
    constexpr uint32_t kBlockBytes = 112;
    EXPECT_EQ(block->Block->Size, kBlockBytes);
    struct ExpectedMember
    {
        const char* Name;
        uint32_t Offset;
    };
    const ExpectedMember kMembers[] = {
        {"uMotionPrevViewProj", 0},
        {"uMotionPrevTimeParams", 64},
        {"uMotionViewportRect", 80},
        {"uMotionJitterUv", 96},
    };
    ASSERT_EQ(block->Block->Members.size(), std::size(kMembers));
    for (size_t i = 0; i < std::size(kMembers); ++i)
    {
        EXPECT_EQ(block->Block->Members[i].Name, kMembers[i].Name) << "member " << i;
        EXPECT_EQ(block->Block->Members[i].Offset, kMembers[i].Offset) << kMembers[i].Name;
    }
}

// The coverage contract. The motion fragment runs the identical discard the prepass runs, over
// the identical declared properties — same surface evaluation, same property, same block in the
// same file. A motion vector written where the prepass wrote no depth describes a surface the
// frame never shaded.
TEST(DeformationMotionVariant, MotionVariantDeclaresTheSameCoveragePropertiesAsTheDepthVariant)
{
    const auto motion = Build(WindTreeDocument(), kMotionPass | MaterialKeyword::AlphaTest);
    SKIP_WITHOUT_SHADERC(motion);
    ASSERT_TRUE(motion.success);
    ASSERT_NE(motion.package, nullptr);
    const auto depth = BuildOrFail(WindTreeDocument(), kDepthPass | MaterialKeyword::AlphaTest,
                                   "masked depth-only variant");
    ASSERT_NE(depth.package, nullptr);

    const auto motionProps = DeclaredPropertyNames(motion);
    ASSERT_FALSE(motionProps.empty())
        << "the shipped surface declares no properties - this contract's premise has changed";
    EXPECT_EQ(motionProps, DeclaredPropertyNames(depth))
        << "the motion variant and the depth-only variant of one material disagree about the "
           "properties their coverage is decided from";

    // The cutoff the block reads by name, in both composed fragments: the instrument above
    // compares declared sets, this one pins the expression that consumes the one that matters.
    const std::string cutoff = "so.opacity < clamp(Props.alphaCutoff, 0.0, 1.0)";
    EXPECT_NE(motion.composedFragmentSource.find(cutoff), std::string::npos)
        << "the masked motion fragment does not run the alpha cutoff";
    EXPECT_NE(depth.composedFragmentSource.find(cutoff), std::string::npos)
        << "the masked depth-only fragment does not run the alpha cutoff";
}

// The endpoint contract. The variant's value is that the modifier is evaluated twice against two
// complete InstanceData values; a variant that compiled the previous fetch away, or never called
// the modifier against it, would produce a camera-only motion vector for a deforming surface and
// look plausible while ghosting every leaf.
//
// The second endpoint is the first with two fields moved back in time, so it costs two reads and
// not a second instance fetch: the instance row and its indirection word are read ONCE per
// vertex. The source counts below are what holds that shape — a previous endpoint that fetched
// its own would still pass every value assertion here while paying twice per vertex on a canopy.
TEST(DeformationMotionVariant, MotionVertexStageEvaluatesTheModifierAtBothEndpoints)
{
    const auto motion = Build(WindTreeDocument(), kMotionPass);
    SKIP_WITHOUT_SHADERC(motion);
    ASSERT_TRUE(motion.success);
    ASSERT_NE(motion.package, nullptr);

    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_PreviousEndpoint"))
        << "the motion vertex stage does not build a previous endpoint";
    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_FetchPreviousModelMatrix"))
        << "the previous endpoint does not swap in the transform the instance carried when this "
           "view last rendered";
    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_StampPreviousDeformationClock"))
        << "the previous endpoint does not carry the clock the previous frame deformed at";
    EXPECT_TRUE(StageSpvNames(motion, "vs", "uMotionPrevViewProj"))
        << "the previous endpoint is not projected through its own unjittered view-projection";
    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_EmitPreviousDeformationEndpoint"))
        << "the motion vertex stage never emits the endpoint it evaluated; the fragment stage "
           "would interpolate uninitialized varyings";

    // Two calls into the modifier, one per endpoint. The composed source is the exact text the
    // SPIR-V came from.
    EXPECT_EQ(CountOccurrences(motion.composedVertexSource, "ModifyVertex(localPos,"), 2u)
        << "the motion vertex stage must evaluate the vertex modifier once per endpoint";

    // One instance fetch for both endpoints.
    EXPECT_EQ(CountOccurrences(motion.composedVertexSource, "ge_FetchInstanceData();"), 1u)
        << "the motion vertex stage fetches the instance row more than once per vertex; the "
           "previous endpoint is this frame's row with two fields moved back in time";
}

// The absence gate: every variant that does not carry the keyword is untouched by any of it. No
// define, no output rename, no previous fetch, no motion block.
TEST(DeformationMotionVariant, VariantsWithoutTheKeywordCarryNoneOfTheMotionInterface)
{
    struct Case
    {
        MaterialDocument Document;
        MaterialKeyword Keywords;
        const char* Label;
    };
    const Case cases[] = {
        {WindTreeDocument(), kWorldPass, "deforming world pass"},
        {WindTreeDocument(), kDepthPass, "deforming depth-only"},
        {StockDocument(), kWorldPass, "rigid world pass"},
    };
    for (const Case& c : cases)
    {
        const auto result = Build(c.Document, c.Keywords);
        SKIP_WITHOUT_SHADERC(result);
        for (const auto& e : result.errors)
            ADD_FAILURE() << c.Label << ": " << e;
        ASSERT_TRUE(result.success) << c.Label;
        ASSERT_NE(result.package, nullptr) << c.Label;

        EXPECT_EQ(std::find(result.composedDefines.begin(), result.composedDefines.end(),
                            "GE_MOTION_VECTORS"),
                  result.composedDefines.end())
            << c.Label << " composed GE_MOTION_VECTORS without the keyword";
        EXPECT_FALSE(StageSpvNames(result, "vs", "ge_PreviousEndpoint"))
            << c.Label << ": the vertex SPIR-V carries the previous-endpoint build";
        EXPECT_FALSE(StageSpvNames(result, "vs", "uMotionPrevViewProj"))
            << c.Label << ": the vertex SPIR-V declares the motion parameter block";

        const StageMeta* fs = FragmentStage(result);
        ASSERT_NE(fs, nullptr) << c.Label;
        for (const auto& out : fs->Outputs)
            EXPECT_NE(out.Name, "oMotion") << c.Label << " declares the motion output";
    }
}

// An opaque motion draw must cost the two endpoints and nothing else: no material fetch, no
// surface evaluation. Only a masked surface needs opacity to decide coverage, and the deforming
// set is mostly foliage — a bark trunk paying a full material fetch per motion fragment would
// make the producer comparison measure the adapter's waste rather than the producer's cost.
// The motion variant shades nothing, so it reads none of the view's exposure the standard surface's
// emissive exposure weight needs (set 0, binding 47); the same material's shading variant does.
static bool FragmentReadsTheViewsExposure(const MaterialBuildResult& result)
{
    constexpr uint32_t kFragmentStageBit = 1u << 1;
    if (result.package == nullptr)
        return false;
    for (const DescriptorSetMeta& set : result.package->meta.Sets)
        for (const DescriptorBindingMeta& binding : set.Bindings)
            if (set.Set == 0u && binding.Name == "ExposureHistory" && (binding.StagesMask & kFragmentStageBit) != 0u)
                return true;
    return false;
}

TEST(DeformationMotionVariant, OnlyTheShadingVariantReadsTheViewsExposure)
{
    const MaterialBuildResult shading = Build(WindTreeDocument(), kWorldPass);
    SKIP_WITHOUT_SHADERC(shading);
    ASSERT_TRUE(shading.success);
    EXPECT_TRUE(FragmentReadsTheViewsExposure(shading)) << "positive control: the shading variant reads the exposure";
    const MaterialBuildResult motion = Build(WindTreeDocument(), kMotionPass);
    ASSERT_TRUE(motion.success);
    EXPECT_FALSE(FragmentReadsTheViewsExposure(motion));
}

TEST(DeformationMotionVariant, AnOpaqueMotionFragmentEvaluatesNoSurface)
{
    const fs::path adapterPath =
        fs::path(RENDERING_SOURCE_DIR) / "Shaders" / "Adapters" / "adapter_forward.glsl";
    std::ifstream in(adapterPath, std::ios::binary);
    ASSERT_TRUE(in.good()) << "could not open " << adapterPath.string();
    const std::string adapter((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const size_t guard = adapter.find("#if defined(GE_MOTION_VECTORS) && !defined(ALPHA_TEST)");
    ASSERT_NE(guard, std::string::npos)
        << "the opaque motion early-out must be guarded on exactly that pair";
    const size_t endGuard = adapter.find("\n#endif", guard);
    ASSERT_NE(endGuard, std::string::npos);
    const std::string block = adapter.substr(guard, endGuard - guard);

    EXPECT_NE(block.find("oMotion = ge_DeformationMotionPayload();"), std::string::npos)
        << "the early-out must export the payload before it returns";
    EXPECT_NE(block.find("return;"), std::string::npos)
        << "the early-out must return before the material fetch";
    EXPECT_EQ(block.find("EvaluateSurface"), std::string::npos);
    EXPECT_EQ(block.find("MaterialParams"), std::string::npos);
    const size_t materialFetch = adapter.find("ge_MatData = MaterialParams");
    ASSERT_NE(materialFetch, std::string::npos);
    EXPECT_LT(guard, materialFetch) << "the early-out must precede the material SSBO fetch";
}

// The keyword refuses the combinations that have no previous endpoint, loudly, at compose time
// rather than by rendering a plausible wrong vector. A rigid surface's motion is an instance
// transform delta the per-instance mover pass already writes exactly.
TEST(DeformationMotionVariant, MotionKeywordOnAMaterialWithNoVertexModifierFailsToCompose)
{
    const auto result = Build(StockDocument(), kMotionPass);
    SKIP_WITHOUT_SHADERC(result);

    EXPECT_FALSE(result.success)
        << "a material with no vertex modifier composed a motion variant; its two endpoints are "
           "the same geometry and the mover pass already covers its transform delta";
    bool named = false;
    for (const auto& e : result.errors)
        named = named || e.find("GE_MOTION_VECTORS needs a vertex modifier") != std::string::npos;
    EXPECT_TRUE(named) << "the refusal does not say what is wrong or how to fix it";
}

// The previous-endpoint fetch has one definition per fetch form. Every row above composes the
// instanced buffer-reference form; these two compose the other two - the non-instanced push-constant
// block, whose previous endpoint keeps this endpoint's transform, and the compatibility profile's
// set-0 instance SSBO - so a previous fetch that fails to compose on either is found here rather than
// by the first producer draw that takes that path.
TEST(DeformationMotionVariant, MotionVariantComposesOnThePushConstantFetchForm)
{
    const auto motion = Build(WindTreeDocument(), MaterialKeyword::MotionVectors);
    SKIP_WITHOUT_SHADERC(motion);
    for (const auto& e : motion.errors)
        ADD_FAILURE() << "push-constant motion variant: " << e;
    ASSERT_TRUE(motion.success);
    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_PreviousEndpoint"))
        << "the push-constant motion vertex stage does not build a previous endpoint";
    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_EmitPreviousDeformationEndpoint"))
        << "the push-constant motion vertex stage never emits the endpoint it evaluated";
    const StageMeta* fs = FragmentStage(motion);
    ASSERT_NE(fs, nullptr);
    ASSERT_EQ(fs->Outputs.size(), 1u);
    EXPECT_EQ(fs->Outputs[0].Name, "oMotion");
}

TEST(DeformationMotionVariant, MotionVariantComposesOnTheCompatibilityFetchForm)
{
    MaterialBuildResult motion;
    {
        ScopedCompatShaderProfile compat;
        motion = Build(WindTreeDocument(), kMotionPass);
    }
    SKIP_WITHOUT_SHADERC(motion);
    for (const auto& e : motion.errors)
        ADD_FAILURE() << "compatibility-profile motion variant: " << e;
    ASSERT_TRUE(motion.success);
    EXPECT_NE(std::find(motion.composedDefines.begin(), motion.composedDefines.end(),
                        "GE_COMPAT_PROFILE"),
              motion.composedDefines.end())
        << "the compatibility profile did not reach the composed defines; this row composed the "
           "instanced form twice";
    EXPECT_TRUE(StageSpvNames(motion, "vs", "ge_PreviousEndpoint"))
        << "the compatibility-profile motion vertex stage does not build a previous endpoint";
    const StageMeta* fs = FragmentStage(motion);
    ASSERT_NE(fs, nullptr);
    ASSERT_EQ(fs->Outputs.size(), 1u);
    EXPECT_EQ(fs->Outputs[0].Name, "oMotion");
}

// The keyword combinations a producer can reach for, and what each one composes.
//
// The motion variant is one of three mutually exclusive fragment shapes — no colour target
// (depth/shadow coverage), the glass tint into a cascade, and the motion payload — and the other
// two both claim the single output at location 0. A producer that ORs the motion keyword onto a
// pass key it did not build (the depth prepass's, a cascade's) composes one of those collisions,
// and until the refusals below it composed into whichever write happened to win, or failed on an
// undeclared identifier that named neither keyword.
TEST(DeformationMotionVariant, MotionKeywordWithAnotherFragmentShapeFailsByName)
{
    struct Case
    {
        MaterialKeyword Keywords;
        const char* NamedError;
        const char* Label;
    };
    const Case cases[] = {
        {kMotionPass | MaterialKeyword::DepthOnlyFragment | MaterialKeyword::AlphaTest,
         "GE_MOTION_VECTORS and GE_DEPTH_ONLY_FRAGMENT are different attachment shapes",
         "motion + depth-only"},
        {kMotionPass | MaterialKeyword::DepthOnlyTransmissionColor | MaterialKeyword::Transmission,
         "GE_MOTION_VECTORS and GE_GLASS_SHADOW_COLOR both claim the single colour output",
         "motion + glass shadow colour"},
    };
    for (const Case& c : cases)
    {
        const auto result = Build(WindTreeDocument(), c.Keywords);
        SKIP_WITHOUT_SHADERC(result);

        EXPECT_FALSE(result.success) << c.Label << " composed a variant with two colour shapes";
        bool named = false;
        for (const auto& e : result.errors)
            named = named || e.find(c.NamedError) != std::string::npos;
        EXPECT_TRUE(named) << c.Label
                           << ": the refusal does not say what is wrong or how to fix it";
    }
}

// The reflection G-buffer keyword is the other case, and it is not a collision: it describes
// three outputs beside a shading output the motion variant does not have. The world pass ORs it
// onto every key for a view with reflections active, so a producer deriving the motion key from
// the view's pass set carries it without meaning to. It is dropped from the composed program
// instead of refused, which is what lets such a key compose the variant it meant.
TEST(DeformationMotionVariant, TheReflectionGBufferKeywordIsDroppedFromTheMotionVariant)
{
    const auto plain = Build(WindTreeDocument(), kMotionPass);
    SKIP_WITHOUT_SHADERC(plain);
    for (const auto& e : plain.errors)
        ADD_FAILURE() << "motion variant: " << e;
    ASSERT_TRUE(plain.success);
    ASSERT_NE(plain.package, nullptr);

    const auto withSssr = Build(WindTreeDocument(), kMotionPass | MaterialKeyword::SSSRNormalRoughness);
    for (const auto& e : withSssr.errors)
        ADD_FAILURE() << "motion + reflection G-buffer variant: " << e;
    ASSERT_TRUE(withSssr.success)
        << "the motion variant does not compose when the view's reflection keyword rides along";
    ASSERT_NE(withSssr.package, nullptr);

    EXPECT_EQ(std::find(withSssr.composedDefines.begin(), withSssr.composedDefines.end(),
                        "GE_SSSR_NORMAL_ROUGHNESS"),
              withSssr.composedDefines.end())
        << "the reflection G-buffer define reached a program that declares no shading output";
    EXPECT_EQ(withSssr.composedDefines, plain.composedDefines);
    EXPECT_EQ(withSssr.composedVertexSource, plain.composedVertexSource);
    EXPECT_EQ(withSssr.composedFragmentSource, plain.composedFragmentSource);

    for (const char* stage : {"vs", "fs"})
    {
        const auto plainIt = plain.package->stageBytes.find(stage);
        const auto sssrIt = withSssr.package->stageBytes.find(stage);
        ASSERT_NE(plainIt, plain.package->stageBytes.end()) << stage;
        ASSERT_NE(sssrIt, withSssr.package->stageBytes.end()) << stage;
        EXPECT_EQ(plainIt->second, sssrIt->second)
            << "the " << stage << " bytes differ between the motion variant and the same variant "
            << "with the reflection keyword; the drop is not what it claims to be";
    }

    const StageMeta* fs = FragmentStage(withSssr);
    ASSERT_NE(fs, nullptr);
    ASSERT_EQ(fs->Outputs.size(), 1u) << "the motion variant grew the reflection G-buffer slices";
    EXPECT_EQ(fs->Outputs[0].Name, "oMotion");
}

// The same drop on the depth/shadow coverage variant, which attaches no colour target at all.
// No producer sets the pair today; the rule is one predicate over both shapes because the reason
// is one — a program with no shading output cannot emit the slices that sit beside it — and a
// second shape reaching the same undeclared identifier is not a second defect.
TEST(DeformationMotionVariant, TheReflectionGBufferKeywordIsDroppedFromTheDepthOnlyVariant)
{
    const auto plain = Build(WindTreeDocument(), kDepthPass | MaterialKeyword::AlphaTest);
    SKIP_WITHOUT_SHADERC(plain);
    for (const auto& e : plain.errors)
        ADD_FAILURE() << "masked depth-only variant: " << e;
    ASSERT_TRUE(plain.success);
    ASSERT_NE(plain.package, nullptr);

    const auto withSssr = Build(
        WindTreeDocument(),
        kDepthPass | MaterialKeyword::AlphaTest | MaterialKeyword::SSSRNormalRoughness);
    for (const auto& e : withSssr.errors)
        ADD_FAILURE() << "masked depth-only + reflection G-buffer variant: " << e;
    ASSERT_TRUE(withSssr.success);
    ASSERT_NE(withSssr.package, nullptr);

    EXPECT_EQ(std::find(withSssr.composedDefines.begin(), withSssr.composedDefines.end(),
                        "GE_SSSR_NORMAL_ROUGHNESS"),
              withSssr.composedDefines.end())
        << "the reflection G-buffer define reached a variant that attaches no colour target";
    for (const char* stage : {"vs", "fs"})
    {
        const auto plainIt = plain.package->stageBytes.find(stage);
        const auto sssrIt = withSssr.package->stageBytes.find(stage);
        ASSERT_NE(plainIt, plain.package->stageBytes.end()) << stage;
        ASSERT_NE(sssrIt, withSssr.package->stageBytes.end()) << stage;
        EXPECT_EQ(plainIt->second, sssrIt->second) << stage;
    }
}

// The control for both rows above: on the shading path the keyword still does what it says, so
// the drop is scoped to the programs that cannot carry it rather than switching the feature off.
TEST(DeformationMotionVariant, TheReflectionGBufferKeywordStillReachesTheShadingVariant)
{
    const auto world = Build(WindTreeDocument(), kWorldPass | MaterialKeyword::SSSRNormalRoughness);
    SKIP_WITHOUT_SHADERC(world);
    for (const auto& e : world.errors)
        ADD_FAILURE() << "world + reflection G-buffer variant: " << e;
    ASSERT_TRUE(world.success);
    ASSERT_NE(world.package, nullptr);

    EXPECT_NE(std::find(world.composedDefines.begin(), world.composedDefines.end(),
                        "GE_SSSR_NORMAL_ROUGHNESS"),
              world.composedDefines.end())
        << "the reflection G-buffer define no longer reaches the shading path";
    const StageMeta* fs = FragmentStage(world);
    ASSERT_NE(fs, nullptr);
    EXPECT_EQ(fs->Outputs.size(), 4u)
        << "the shading variant must still declare the colour output plus the three G-buffer slices";
}
