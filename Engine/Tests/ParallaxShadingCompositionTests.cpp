// What a height-mapped standard material compiles into beyond the march itself: the relief's
// self-shadow reaching the primary light in both lighting paths, the relief footprint taken at the
// pixel centre where the device allows it (and a pipeline a validating device accepts), and the
// Parallax steps debug view.
//
// Both are properties of the composed program, so these tests compose and compile the real
// material through BuildMaterialToShaderPackage and read the fragment SPIR-V. The self-shadow is
// read by data flow: SurfaceOutput.primaryLightOcclusion exists only in the marching shapes, the
// surface writes it, and only the lighting reads it, so a fragment that LOADS the member is one
// whose lighting applies the relief's shadow. The clustered path serves the ForwardPlus keys and
// the single-light fallback the others, so the two colour key sets below reach one path each.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialPrewarmVariants.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Types/StringId.h"

#include "../Modules/Rendering/Tests/ScopedEnvVar.h"
#include "ScopedCompatShaderProfile.h"
#include "ScopedInterpolationFunctions.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

// ---- A minimal SPIR-V reader ------------------------------------------------------------------

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kSpirvHeaderWords = 5;
constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpMemberName = 6;
constexpr uint32_t kOpExtInstImport = 11;
constexpr uint32_t kOpExtInst = 12;
constexpr uint32_t kOpConstant = 43;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpFunctionParameter = 55;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpLoad = 61;
constexpr uint32_t kOpAccessChain = 65;
constexpr uint32_t kOpInBoundsAccessChain = 66;

std::string LiteralString(const uint32_t* words, size_t count)
{
    std::string text(reinterpret_cast<const char*>(words), count * sizeof(uint32_t));
    return text.substr(0, text.find('\0'));
}

// The words of a SPIR-V module, or none when the bytes are not one.
std::vector<uint32_t> SpirvWords(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() % sizeof(uint32_t) != 0 || bytes.size() < kSpirvHeaderWords * sizeof(uint32_t))
        return {};
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] != kSpirvMagic)
        return {};
    return words;
}

struct MemberLoads
{
    bool MemberDeclared = false; // the struct has a member of that name
    size_t Loads = 0;            // OpLoads through an access chain to it
};

// Counts the loads of `structName.memberName` in a SPIR-V module that keeps its debug names. Debug
// names precede every type and function in a module, so the member's index is known before the
// first access chain is read.
MemberLoads CountMemberLoads(const std::vector<uint8_t>& bytes, std::string_view structName,
                             std::string_view memberName)
{
    MemberLoads result;
    const std::vector<uint32_t> words = SpirvWords(bytes);
    if (words.empty())
        return result;

    std::optional<uint32_t> structType;
    std::optional<uint32_t> memberIndex;
    std::unordered_map<uint32_t, uint32_t> constants; // constant id -> 32-bit value
    std::unordered_set<uint32_t> pointerTypes;        // pointer-to-struct type ids
    std::unordered_set<uint32_t> structValues;        // variables and parameters of that type
    std::unordered_set<uint32_t> memberPointers;      // access chains to the member
    for (size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size())
            return result;
        const uint32_t* operands = &words[at + 1];
        switch (opcode)
        {
        case kOpName:
            if (!structType && LiteralString(operands + 1, count - 2) == structName)
                structType = operands[0];
            break;
        case kOpMemberName:
            if (structType && operands[0] == *structType && LiteralString(operands + 2, count - 3) == memberName)
                memberIndex = operands[1];
            break;
        case kOpConstant:
            if (count == 4)
                constants[operands[1]] = operands[2];
            break;
        case kOpTypePointer:
            if (structType && operands[2] == *structType)
                pointerTypes.insert(operands[0]);
            break;
        case kOpVariable:
        case kOpFunctionParameter:
            if (pointerTypes.count(operands[0]) != 0)
                structValues.insert(operands[1]);
            break;
        case kOpAccessChain:
        case kOpInBoundsAccessChain:
            if (memberIndex && count >= 5 && structValues.count(operands[2]) != 0)
            {
                const auto index = constants.find(operands[3]);
                if (index != constants.end() && index->second == *memberIndex)
                    memberPointers.insert(operands[1]);
            }
            break;
        case kOpLoad:
            if (memberPointers.count(operands[2]) != 0)
                ++result.Loads;
            break;
        default:
            break;
        }
        at += count;
    }
    result.MemberDeclared = memberIndex.has_value();
    return result;
}

// GLSL.std.450 InterpolateAtOffset, the instruction interpolateAtOffset compiles to.
constexpr uint32_t kGlslStd450InterpolateAtOffset = 78;

// Counts the GLSL.std.450 extended instructions numbered `instruction` in a SPIR-V module.
size_t CountGlslStd450Instructions(const std::vector<uint8_t>& bytes, uint32_t instruction)
{
    const std::vector<uint32_t> words = SpirvWords(bytes);
    std::optional<uint32_t> glslStd450;
    size_t result = 0;
    for (size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size())
            return result;
        const uint32_t* operands = &words[at + 1];
        if (opcode == kOpExtInstImport && LiteralString(operands + 1, count - 2) == "GLSL.std.450")
            glslStd450 = operands[0];
        else if (opcode == kOpExtInst && count >= 5 && glslStd450 && operands[2] == *glslStd450 &&
                 operands[3] == instruction)
            ++result;
        at += count;
    }
    return result;
}

// Whether a SPIR-V module that keeps its debug names names `name`.
bool Names(const std::vector<uint8_t>& spirv, std::string_view name)
{
    return std::search(spirv.begin(), spirv.end(), name.begin(), name.end()) != spirv.end();
}

// ---- Composition ------------------------------------------------------------------------------

class ParallaxShadingComposition : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
        ASSERT_TRUE(std::filesystem::exists(shaderDir)) << "Staged engine shader tree not found: " << shaderDir.string();
        ASSERT_TRUE(ShaderCompileService::IsCompilerAvailable()) << "no shader compiler in this build";
        m_CacheRoot = std::filesystem::temp_directory_path() / ("ge_parallax_shading_" + GUID::Generate().ToString());
        m_Context.AdapterShaderDir = shaderDir;
        m_Context.CacheRoot = m_CacheRoot / "Shaders";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_CacheRoot, ec);
    }

    static MaterialDocument StandardDocument(bool heightMapped)
    {
        MaterialDocument doc{};
        doc.materialName = heightMapped ? "ParallaxShadingProbe" : "FlatShadingProbe";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/standard_pbr.glsl";
        if (heightMapped)
            doc.textures["heightMap"] = "__embedded__:0";
        return doc;
    }

    std::vector<uint8_t> Fragment(const MaterialDocument& doc, MaterialKeyword keywords, VertexAttributeFlags flags)
    {
        const MaterialBuildResult result =
            BuildMaterialToShaderPackage(doc, m_CacheRoot / (doc.materialName + ".material"), doc.materialName,
                                         m_Context, ShaderSourceKind::SpirV, keywords, flags);
        EXPECT_TRUE(result.success) << Label(keywords) << ": "
                                    << (result.errors.empty() ? std::string{} : result.errors.front());
        if (!result.package)
            return {};
        const auto it = result.package->stageBytes.find("fs");
        return it != result.package->stageBytes.end() ? it->second : std::vector<uint8_t>{};
    }

    static std::string Label(MaterialKeyword keywords)
    {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "keywords 0x%llx", static_cast<unsigned long long>(keywords));
        return buffer;
    }

    static bool ShadesNothing(MaterialKeyword keywords)
    {
        return HasKeyword(keywords, MaterialKeyword::DepthOnlyFragment) ||
               HasKeyword(keywords, MaterialKeyword::MotionVectors) ||
               HasKeyword(keywords, MaterialKeyword::DepthOnlyTransmissionColor);
    }

    // Every shading key the warm-up requests for a height-mapped material under `colorKeywords`
    // loads the relief's occlusion into its lighting; the shapes that shade nothing have no such
    // field at all.
    void ExpectEveryShadingKeyShadowsThePrimaryLight(MaterialKeyword colorKeywords)
    {
        const MaterialDocument doc = StandardDocument(true);
        ShaderVariantKey base{};
        base.vertexFlags = VertexAttributeFlags::StandardMesh;
        base.lightingModel = HashStringId("StandardPBR");
        size_t shading = 0;
        for (const ShaderVariantKey& key : MaterialPrewarmVariantKeys(base, colorKeywords, false, MaterialAlphaMode::Opaque))
        {
            const MemberLoads loads = CountMemberLoads(Fragment(doc, key.materialKeywords, key.vertexFlags),
                                                       "SurfaceOutput", "primaryLightOcclusion");
            if (ShadesNothing(key.materialKeywords))
            {
                EXPECT_FALSE(loads.MemberDeclared) << Label(key.materialKeywords);
                continue;
            }
            ++shading;
            ASSERT_TRUE(loads.MemberDeclared) << Label(key.materialKeywords) << ": no occlusion field in a marching shape";
            EXPECT_GE(loads.Loads, 1u) << Label(key.materialKeywords)
                                       << ": the lighting never reads the relief's shadow";
        }
        EXPECT_GT(shading, 0u) << "no shading key was composed";
    }

    std::filesystem::path m_CacheRoot;
    MaterialBuildContext m_Context{};
};

constexpr MaterialKeyword kClusteredColour = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
                                             MaterialKeyword::IBL | MaterialKeyword::Instanced;
constexpr MaterialKeyword kSingleLightColour = MaterialKeyword::Shadows | MaterialKeyword::Instanced;

TEST_F(ParallaxShadingComposition, TheClusteredPathShadowsThePrimaryLightByTheRelief)
{
    ExpectEveryShadingKeyShadowsThePrimaryLight(kClusteredColour);
}

TEST_F(ParallaxShadingComposition, TheSingleLightPathShadowsThePrimaryLightByTheRelief)
{
    ExpectEveryShadingKeyShadowsThePrimaryLight(kSingleLightColour);
}

TEST_F(ParallaxShadingComposition, TheCompatArmComposesBothLightingPaths)
{
    // The compat arm's march takes no self-shadow steps (the host tests pin that the function
    // returns 1 there); what this pins is that both lighting paths still compose with the field.
    TestSupport::ScopedCompatShaderProfile compat;
    ExpectEveryShadingKeyShadowsThePrimaryLight(kClusteredColour);
    ExpectEveryShadingKeyShadowsThePrimaryLight(kSingleLightColour);
}

// ---- The relief footprint -----------------------------------------------------------------------

TEST_F(ParallaxShadingComposition, TheFootprintIsTakenAtThePixelCentreWhereSurfaceInputsAreCentroid)
{
    // Desktop opaque shading interpolates its surface inputs at the covered samples' centroid, whose
    // derivatives are wrong along MSAA triangle edges; the relief's footprint re-interpolates uv0
    // and the position at the pixel centre. Cutouts and the compat arm interpolate at the pixel
    // centre already and take no second interpolation, and nothing reaches a material that does
    // not march.
    TestSupport::ScopedInterpolationFunctions available(true);
    const MaterialDocument relief = StandardDocument(true);
    const VertexAttributeFlags mesh = VertexAttributeFlags::StandardMesh;
    EXPECT_EQ(CountGlslStd450Instructions(Fragment(relief, kClusteredColour, mesh), kGlslStd450InterpolateAtOffset), 2u);
    EXPECT_EQ(CountGlslStd450Instructions(Fragment(relief, kSingleLightColour, mesh), kGlslStd450InterpolateAtOffset),
              2u);
    EXPECT_EQ(CountGlslStd450Instructions(Fragment(relief, kClusteredColour | MaterialKeyword::AlphaTest, mesh),
                                          kGlslStd450InterpolateAtOffset),
              0u);
    EXPECT_EQ(CountGlslStd450Instructions(Fragment(StandardDocument(false), kClusteredColour, mesh),
                                          kGlslStd450InterpolateAtOffset),
              0u);
    TestSupport::ScopedCompatShaderProfile compat;
    EXPECT_EQ(CountGlslStd450Instructions(Fragment(relief, kClusteredColour, mesh), kGlslStd450InterpolateAtOffset), 0u);
}

TEST_F(ParallaxShadingComposition, WithoutTheInterpolationFunctionsTheFootprintKeepsTheCentroidInputs)
{
    // interpolateAtOffset declares the SPIR-V InterpolationFunction capability, which Vulkan allows
    // only on a device that enabled sampleRateShading. Where the device did not, the relief's
    // footprint takes the centroid inputs' own derivatives: valid everywhere, with a step-count
    // seam along MSAA triangle edges.
    TestSupport::ScopedInterpolationFunctions unavailable(false);
    EXPECT_EQ(CountGlslStd450Instructions(Fragment(StandardDocument(true), kClusteredColour,
                                                   VertexAttributeFlags::StandardMesh),
                                          kGlslStd450InterpolateAtOffset),
              0u);
}

// ---- A validating device accepts the parallax colour pipeline -----------------------------------

// Restores the shader compile inputs RenderServices::Initialize sets from its device's profile
// (ApplyShaderCompileProfile), so the tests after this one compose as they would have.
struct RestoredShaderCompileProfile
{
    RestoredShaderCompileProfile()
        : Compat(IsCompatShaderProfile()), InterpolationFunctions(AreInterpolationFunctionsAvailable())
    {
    }
    ~RestoredShaderCompileProfile()
    {
        SetCompatShaderProfile(Compat);
        SetInterpolationFunctionsAvailable(InterpolationFunctions);
    }
    RestoredShaderCompileProfile(const RestoredShaderCompileProfile&) = delete;
    RestoredShaderCompileProfile& operator=(const RestoredShaderCompileProfile&) = delete;
    bool Compat;
    bool InterpolationFunctions;
};

uint64_t VuidCount(const IDevice& device, std::string_view vuid, std::string* firstMessage)
{
    for (const ValidationVuidStat& stat : device.GetValidationStats().Vuids)
    {
        if (stat.Vuid != vuid)
            continue;
        if (firstMessage)
            *firstMessage = stat.FirstMessage;
        return stat.Count;
    }
    return 0;
}

// The shader module of every desktop parallax colour pipeline is created when a pass first draws
// it. On a validating device that creation must raise no capability error: the
// VUID-VkShaderModuleCreateInfo-pCode-08740 an interpolation function raises on a device that did
// not enable sampleRateShading.
TEST(ParallaxPipelineOnAValidatingDevice, TheCentroidParallaxColourPipelineDeclaresOnlyEnabledCapabilities)
{
    const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
    ASSERT_TRUE(std::filesystem::exists(shaderDir)) << "Staged engine shader tree not found: " << shaderDir.string();
    // The layer's verdict is what this test reads; the default-on validation assert would abort the
    // process on the first error instead of letting the assertion name it. The device reads the
    // variable once, at the process's first unsuppressed error, so this holds whatever ran before:
    // an earlier error either found the assert off or ended the process.
    Rendering::Tests::ScopedEnvVar validationAssert("GE_VK_VALIDATION_ASSERT", "0");
    const RestoredShaderCompileProfile restoredProfile;

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    desc.enableDebugLayer = true;
    std::unique_ptr<IDevice> device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->GetValidationStats().Enabled)
    {
        device->Shutdown();
        GTEST_SKIP() << "Vulkan validation layer not active: the capability VUID cannot be observed";
    }

    {
        RenderServices services;
        ASSERT_TRUE(services.Initialize(device.get()));
        MaterialBuildContext context{};
        context.AdapterShaderDir = shaderDir;
        context.CacheRoot = std::filesystem::temp_directory_path() / ("ge_parallax_validity_" + GUID::Generate().ToString());
        context.IncludeDirs = {shaderDir};
        services.Materials().SetMaterialBuildContext(context);

        MaterialDocument doc{};
        doc.materialName = "ParallaxValidityProbe";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/standard_pbr.glsl";
        doc.textures["heightMap"] = "__embedded__:0";
        const Material* material = services.Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
        ASSERT_NE(material, nullptr);
        const GraphicsPipelineId id = material->GetGraphicsPipelineId();
        const GraphicsPipelineDesc* pipeline = device->LookupGraphicsPipeline(id);
        ASSERT_NE(pipeline, nullptr);
        ASSERT_TRUE(pipeline->PixelShader && !pipeline->PixelShader->empty());
        // The probe must be the case in question: a marching, centroid colour program that takes
        // the pixel-centre footprint wherever this device allows it.
        ASSERT_TRUE(Names(*pipeline->PixelShader, "GE_ParallaxMarch")) << "the registered program does not march";
        EXPECT_EQ(CountGlslStd450Instructions(*pipeline->PixelShader, kGlslStd450InterpolateAtOffset) != 0,
                  device->GetCapabilities().supportsSampleRateShading)
            << "the pixel-centre footprint does not follow the device's sampleRateShading";

        PipelineFormatKey format{};
        format.ColorCount = static_cast<uint8_t>(std::max<size_t>(pipeline->ColorBlend.attachments.size(), 1));
        for (uint8_t i = 0; i < format.ColorCount; ++i)
            format.ColorFormats[i] = TextureFormat::R16G16B16A16_FLOAT;
        format.DepthFormat = TextureFormat::D32_FLOAT;
        format.RasterizationSamples = 4;
        device->ResetValidationStats();
        const PipelineFormatKey formats[] = {format};
        device->PrewarmGraphicsPipeline(id, formats);
        EXPECT_TRUE(device->TryGetWarmGraphicsPipeline(id, format).IsValid()) << "the colour pipeline was not built";

        std::string message;
        EXPECT_EQ(VuidCount(*device, "VUID-VkShaderModuleCreateInfo-pCode-08740", &message), 0u) << message;
        services.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(context.CacheRoot, ec);
    }
    device->Shutdown();
}

// ---- The Parallax steps view --------------------------------------------------------------------


TEST_F(ParallaxShadingComposition, TheStepsViewReplacesTheShadingOfAMarchingVariant)
{
    const MaterialDocument doc = StandardDocument(true);
    const MaterialKeyword viewed = kClusteredColour | MaterialKeyword::ParallaxStepsView;
    EXPECT_TRUE(Names(Fragment(doc, viewed, VertexAttributeFlags::StandardMesh), "GE_ParallaxStepsViewShade"));
    EXPECT_FALSE(Names(Fragment(doc, kClusteredColour, VertexAttributeFlags::StandardMesh), "GE_ParallaxStepsViewShade"));
}

TEST_F(ParallaxShadingComposition, TheStepsViewKeywordChangesNothingThatDoesNotMarch)
{
    // A material without a height map, and a height-mapped material's depth-only shape: the
    // keyword reaches neither program, byte for byte.
    const MaterialDocument flat = StandardDocument(false);
    EXPECT_EQ(Fragment(flat, kClusteredColour | MaterialKeyword::ParallaxStepsView, VertexAttributeFlags::StandardMesh),
              Fragment(flat, kClusteredColour, VertexAttributeFlags::StandardMesh));

    const MaterialDocument relief = StandardDocument(true);
    const MaterialKeyword depth = MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment;
    EXPECT_EQ(Fragment(relief, depth | MaterialKeyword::ParallaxStepsView, VertexAttributeFlags::StandardMesh),
              Fragment(relief, depth, VertexAttributeFlags::StandardMesh));
}

TEST_F(ParallaxShadingComposition, TheStepsViewComposesOnTheCompatArm)
{
    TestSupport::ScopedCompatShaderProfile compat;
    const MaterialDocument doc = StandardDocument(true);
    EXPECT_TRUE(Names(Fragment(doc, kClusteredColour | MaterialKeyword::ParallaxStepsView,
                               VertexAttributeFlags::StandardMesh),
                      "GE_ParallaxStepsViewShade"));
}

} // namespace
