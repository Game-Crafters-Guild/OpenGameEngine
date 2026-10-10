#include <gtest/gtest.h>
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Core/Device.h"
#include "TestUtils.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

// Exercise the real composer and shaderc: instancing, camera basis varyings,
// atlas sampling, scene lights, shadows, and six-way ambient all share adapters.
TEST(ParticleShaderCompose, SurfaceCompilesWithoutParticleInstanceBuffer)
{
    const bool original = IsCompatShaderProfile();
    for (bool compat : {false, true})
    {
        SetCompatShaderProfile(compat);
        MaterialDocument doc;
        doc.materialName = "Particle asset preview";
        doc.surfaceShader = "Surfaces/particle_surface.glsl";
        doc.lightingModel = "Unlit";
        doc.alphaMode = MaterialAlphaMode::Blend;
        MaterialBuildContext ctx;
        ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
        ctx.PackageShaderDirs = {ctx.AdapterShaderDir};
        ctx.CacheRoot = std::filesystem::temp_directory_path() / "ge_particle_asset_preview";
        const auto result = BuildMaterialToShaderPackage(doc, ctx.AdapterShaderDir / "Particle.material",
            "ParticlePreview", ctx, ShaderSourceKind::SpirV, MaterialKeyword::Instanced,
            VertexAttributeFlags::StandardMesh);
        for (const auto& error : result.errors) ADD_FAILURE() << error;
        EXPECT_TRUE(result.success);
        std::filesystem::remove_all(ctx.CacheRoot);
    }
    SetCompatShaderProfile(original);
}

namespace
{
// One particle program variant: profile, lighting (0 unlit, 1 lit, 2 six-way), texture sheet, clustered
// lighting with shadows and image-based light, emission texture, and six-way packing.
struct ParticleVariant
{
    bool Compat = false;
    int Lighting = 0;
    bool Textured = false;
    bool Clustered = false;
    bool Emission = false;
    int Packing = 0;
};

std::vector<ParticleVariant> EveryParticleVariant()
{
    std::vector<ParticleVariant> variants;
    for (bool compat : {false, true})
    for (int lighting = 0; lighting < 3; ++lighting)
    for (bool textured : {false, true})
    for (bool clustered : {false, true})
    for (bool emission : {false, true})
    for (int packing = 0; packing < (lighting == 2 ? 3 : 1); ++packing)
        variants.push_back({compat, lighting, textured, clustered, emission, packing});
    return variants;
}

std::string Describe(const ParticleVariant& variant)
{
    return std::string(variant.Compat ? "compatibility profile" : "native profile") +
           " lighting=" + std::to_string(variant.Lighting) + " textured=" + std::to_string(variant.Textured) +
           " clustered=" + std::to_string(variant.Clustered) + " emission=" + std::to_string(variant.Emission) +
           " packing=" + std::to_string(variant.Packing);
}

MaterialBuildResult BuildParticleVariant(const ParticleVariant& variant)
{
    SetCompatShaderProfile(variant.Compat);
    MaterialDocument doc;
    doc.materialName = "Particle test";
    doc.vertexModifier = "Particles/particle_vertex.glsl";
    doc.surfaceShader = "Surfaces/particle_surface.glsl";
    doc.lightingModel = variant.Lighting ? "StandardPBR" : "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.doubleSided = true;
    doc.keywords = {"PARTICLE_BUFFER"};
    if (variant.Lighting) doc.keywords.push_back("PARTICLE_LIT");
    if (variant.Lighting == 2) doc.keywords.push_back("SIX_WAY");
    if (variant.Packing == 1) doc.keywords.push_back("SIX_WAY_RGBA");
    if (variant.Packing == 2) doc.keywords.push_back("SIX_WAY_TOP_LEFT_RIGHT_BOTTOM_BACK_FRONT");
    if (variant.Emission) doc.keywords.push_back("PARTICLE_EMISSION_TEXTURE");
    if (variant.Textured) doc.keywords.push_back("PARTICLE_TEXTURE");
    MaterialBuildContext ctx;
    ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
    ctx.PackageShaderDirs = {ctx.AdapterShaderDir};
    ctx.CacheRoot = std::filesystem::temp_directory_path() / ("ge_particle_shader_" + std::to_string(variant.Lighting) +
                                                              std::to_string(variant.Textured) + std::to_string(variant.Clustered));
    auto result = BuildMaterialToShaderPackage(doc, ctx.AdapterShaderDir / "Particle.material",
        "ParticleTest", ctx, ShaderSourceKind::SpirV, variant.Clustered ? MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL
                                                    : MaterialKeyword::Instanced,
        variant.Textured ? VertexAttributeFlags::StandardMeshWithTangent | VertexAttributeFlags::HasColor | VertexAttributeFlags::HasUV1 |
                           VertexAttributeFlags::HasUV2 | VertexAttributeFlags::HasUV3 | VertexAttributeFlags::HasUV4 | VertexAttributeFlags::HasUV5 |
                           VertexAttributeFlags::HasUV6 | VertexAttributeFlags::HasUV7 : VertexAttributeFlags::StandardMesh);
    std::filesystem::remove_all(ctx.CacheRoot);
    return result;
}

struct RestoreProfile
{
    bool original = IsCompatShaderProfile();
    ~RestoreProfile() { SetCompatShaderProfile(original); }
};

// The SPIR-V opcodes the discard guard reads.
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpFunction = 54;
constexpr uint32_t kOpFunctionEnd = 56;
constexpr uint32_t kOpFunctionCall = 57;
constexpr uint32_t kOpKill = 252;
constexpr uint32_t kOpTerminateInvocation = 4416;
constexpr uint32_t kExecutionModelFragment = 4;
// Everything that reads the quad's screen-space derivatives: the samples whose level of detail comes
// from them (ImplicitLod, DrefImplicitLod, ProjImplicitLod, ProjDrefImplicitLod and their sparse
// forms), the level query (ImageQueryLod), and the derivatives themselves (DPdx, DPdy, Fwidth and
// their Fine and Coarse forms).
constexpr uint32_t kDerivativeReads[] = {87, 89, 91, 93, 305, 307, 309, 311, 105,
                                         207, 208, 209, 210, 211, 212, 213, 214, 215};

using FunctionBodies = std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, std::vector<uint32_t>>>>;

// The opcodes of `function` in program order, every call expanded in place.
void Flatten(const FunctionBodies& functions, uint32_t function, int depth, std::vector<uint32_t>& opcodes)
{
    const auto found = functions.find(function);
    if (found == functions.end() || depth > 64)
        return;
    for (const auto& [opcode, operands] : found->second)
    {
        if (opcode == kOpFunctionCall && operands.size() >= 3)
            Flatten(functions, operands[2], depth + 1, opcodes);
        else
            opcodes.push_back(opcode);
    }
}

struct DiscardOrder
{
    bool Discards = false;
    int DerivativeReadsAfterFirstDiscard = 0;
};

// Reads a fragment stage's SPIR-V in program order from its entry point.
DiscardOrder ReadDiscardOrder(const std::vector<uint8_t>& bytes)
{
    std::vector<uint32_t> words(bytes.size() / 4u);
    std::memcpy(words.data(), bytes.data(), words.size() * 4u);
    FunctionBodies functions;
    uint32_t entry = 0;
    uint32_t current = 0;
    for (size_t at = 5; at < words.size();)
    {
        const uint32_t count = words[at] >> 16;
        const uint32_t opcode = words[at] & 0xFFFFu;
        if (count == 0 || at + count > words.size())
            break;
        std::vector<uint32_t> operands(words.begin() + static_cast<std::ptrdiff_t>(at + 1),
                                       words.begin() + static_cast<std::ptrdiff_t>(at + count));
        if (opcode == kOpEntryPoint && operands.size() >= 2 && operands[0] == kExecutionModelFragment)
            entry = operands[1];
        if (opcode == kOpFunction && operands.size() >= 2)
            current = operands[1];
        else if (opcode == kOpFunctionEnd)
            current = 0;
        else if (current != 0)
            functions[current].emplace_back(opcode, std::move(operands));
        at += count;
    }
    std::vector<uint32_t> opcodes;
    Flatten(functions, entry, 0, opcodes);
    DiscardOrder order;
    for (const uint32_t opcode : opcodes)
    {
        if (opcode == kOpKill || opcode == kOpTerminateInvocation)
            order.Discards = true;
        else if (order.Discards && std::find(std::begin(kDerivativeReads), std::end(kDerivativeReads), opcode) !=
                                       std::end(kDerivativeReads))
            ++order.DerivativeReadsAfterFirstDiscard;
    }
    return order;
}
} // namespace

TEST(ParticleShaderCompose, AllLightingAndTextureVariantsCompile)
{
    RestoreProfile restoreProfile;
    for (const ParticleVariant& variant : EveryParticleVariant())
    {
        SCOPED_TRACE(Describe(variant));
        const auto result = BuildParticleVariant(variant);
        for (const auto& error : result.errors) ADD_FAILURE() << error;
        ASSERT_TRUE(result.success);
        ASSERT_NE(result.package, nullptr);
        EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
        EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
        if (variant.Compat)
        {
            bool particleBinding = false;
            for (const auto& set : result.package->meta.Sets)
                if (set.Set == 0)
                    for (const auto& binding : set.Bindings)
                        if (binding.Binding == 29 && binding.Name == "ParticleInstances")
                            particleBinding = true;
            EXPECT_TRUE(particleBinding);
        }
    }
}

// A discarded pixel stops contributing to its 2x2 quad, so a derivative taken after a discard, or a
// sample that takes its level from one, is undefined in the pixels that keep shading; a trail's zero
// end discards helper pixels while their neighbours still sample. Every unlit particle takes its
// gradients before the first discard, and lit particles, whose adapter lighting reads derivatives,
// do not discard.
TEST(ParticleShaderCompose, NoDerivativeReadFollowsADiscard)
{
    RestoreProfile restoreProfile;
    const std::vector<ParticleVariant> variants = EveryParticleVariant();
    size_t unlit = 0;
    size_t unlitDiscarding = 0;
    size_t litDiscarding = 0;
    for (const ParticleVariant& variant : variants)
    {
        SCOPED_TRACE(Describe(variant));
        const auto result = BuildParticleVariant(variant);
        ASSERT_TRUE(result.success);
        ASSERT_NE(result.package, nullptr);
        const DiscardOrder order = ReadDiscardOrder(result.package->stageBytes.at("fs"));
        EXPECT_EQ(order.DerivativeReadsAfterFirstDiscard, 0)
            << "a derivative, or a sample or level query that reads one, follows a discard";
        unlit += variant.Lighting == 0 ? 1u : 0u;
        unlitDiscarding += variant.Lighting == 0 && order.Discards ? 1u : 0u;
        litDiscarding += variant.Lighting != 0 && order.Discards ? 1u : 0u;
    }
    EXPECT_EQ(unlitDiscarding, unlit) << "positive control: every unlit particle fragment variant discards";
    EXPECT_EQ(litDiscarding, 0u) << "a lit particle fragment variant discards";
}

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include <array>

TEST(ParticleShaderCompose, SixWayResponseReadsBackEverySignedAxis)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc desc;
    desc.applicationName = "Particle six-way test";
#ifdef __APPLE__
    desc.preferredAPI = GraphicsAPI::Metal;
#else
    desc.preferredAPI = GraphicsAPI::Vulkan;
#endif
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc)) GTEST_SKIP() << "No headless GPU backend";
    ShaderProgramCompileRequest request;
    request.debugName = "SixWayResponseTest";
    request.baseDirectory = Tests::GetAdapterShaderDir();
    request.includeDirs = {request.baseDirectory};
    request.cacheRoot = std::filesystem::temp_directory_path() / "ge_particle_six_way_readback";
    ShaderStageCompileSpec stage;
    stage.stage = "cs"; stage.sourcePath = "ParticleSixWayTest.comp";
    stage.inlineSource = R"glsl(#version 450
#include "Includes/particle_six_way.glsl"
#include "Includes/particle_billboard.glsl"
#include "Includes/particle_mesh_animation.glsl"
layout(local_size_x = 1) in;
layout(set=0,binding=0,std430) buffer Output { float responses[]; } result;
void main()
{
    vec3 positive = vec3(0.1,0.2,0.3), negative = vec3(0.4,0.5,0.6);
    for (int i=0; i<6; ++i)
    {
        vec3 direction = vec3(0.0); direction[i%3] = i<3 ? 1.0 : -1.0;
        result.responses[i] = GE_SixWayResponse(positive,negative,direction);
    }
    result.responses[6] = GE_SixWayResponse(positive,negative,vec3(1.0));
    result.responses[7] = GE_SixWayResponse(vec3(1.0),vec3(1.0),vec3(-3,2,1));
    // A second view rotates the sprite basis by 90 degrees around Z.
    mat3 basis = mat3(vec3(0,1,0),vec3(-1,0,0),vec3(0,0,1));
    result.responses[8] = GE_SixWayResponse(positive,negative,transpose(basis)*vec3(1,0,0));
    result.responses[9] = GE_SixWayResponse(positive,negative,vec3(0));
    mat3 cameraA = mat3(vec3(1,0,0),vec3(0,1,0),vec3(0,0,-1));
    mat3 cameraB = mat3(vec3(0,0,-1),vec3(0,1,0),vec3(-1,0,0));
    mat3 a = GE_ParticleBillboardBasis(mat3(1),cameraA,cameraA[2],vec3(0,1,0),1u,0u,0.0);
    mat3 b = GE_ParticleBillboardBasis(mat3(1),cameraB,cameraB[2],vec3(0,1,0),1u,0u,0.0);
    result.responses[10] = dot(a[0],vec3(1,0,0));
    result.responses[11] = dot(a[2],vec3(0,0,-1));
    result.responses[12] = dot(b[0],vec3(0,0,-1));
    result.responses[13] = dot(b[2],vec3(-1,0,0));
    mat3 velocity = GE_ParticleBillboardBasis(mat3(1),cameraA,cameraA[2],vec3(0,1,1),2u,0u,0.0);
    mat3 projected = GE_ParticleBillboardBasis(mat3(1),cameraA,cameraA[2],vec3(0,1,1),3u,0u,0.0);
    result.responses[14] = dot(velocity[1],normalize(vec3(0,1,1)));
    result.responses[15] = dot(projected[1],vec3(0,1,0));
    result.responses[16] = dot(projected[0],vec3(1,0,0)); // no mirrored atlas
    mat3 rotated = GE_ParticleBillboardBasis(mat3(1),cameraA,cameraA[2],vec3(0),1u,0u,1.57079632679);
    result.responses[17] = dot(rotated[0],vec3(0,1,0));
    // Orbit from front to back and above: neither the sprite axes nor the
    // signed six-way response may change in world-oriented mode.
    mat3 worldA = GE_ParticleBillboardBasis(mat3(1),cameraA,cameraA[2],vec3(0),0u,0u,0.0);
    mat3 worldB = GE_ParticleBillboardBasis(mat3(1),mat3(1),vec3(0,0,1),vec3(0),0u,0u,0.0);
    mat3 above = mat3(vec3(1,0,0),vec3(0,0,1),vec3(0,1,0));
    mat3 worldC = GE_ParticleBillboardBasis(mat3(1),above,above[2],vec3(0),0u,0u,0.0);
    result.responses[18] = dot(worldA[2],vec3(0,0,1));
    result.responses[19] = length(worldA[2]-worldB[2]);
    result.responses[20] = length(worldA[0]-worldC[0])+length(worldA[1]-worldC[1]);
    result.responses[21] = GE_SixWayResponse(positive,negative,transpose(worldA)*vec3(0,0,1));
    result.responses[22] = GE_SixWayResponse(positive,negative,transpose(worldB)*vec3(0,0,1));
    vec3 decodedPositive, decodedNegative;
    float opacity, emissive;
    GE_DecodeSixWayRGBA(vec4(0.1,0.2,0.3,0.4),vec4(0.5,0.6,0.7,0.8),
                        decodedPositive,decodedNegative,opacity,emissive);
    for (int i=0;i<3;++i) { result.responses[23+i]=decodedPositive[i]; result.responses[26+i]=decodedNegative[i]; }
    result.responses[29]=opacity; result.responses[30]=emissive;
    vec3 glow=GE_ParticleEmission(vec3(0.2,0.4,0.6),vec4(1,0.5,0.25,1),5.0);
    for (int i=0;i<3;++i) result.responses[31+i]=glow[i];
    result.responses[34]=length(GE_ParticleEmission(vec3(1),vec4(1),0.0));
    GE_DecodeSixWayTopLeftRightBottomBackFront(vec3(0.1,0.2,0.3),vec3(0.5,0.6,0.7),decodedPositive,decodedNegative);
    for (int i=0;i<3;++i) { result.responses[35+i]=decodedPositive[i]; result.responses[38+i]=decodedNegative[i]; }
    vec3 toEye = vec3(1,0,-2);
    mat3 facing = GE_ParticleBillboardBasis(mat3(1),cameraA,toEye,vec3(0),5u,0u,0.0);
    result.responses[41] = dot(facing[2],normalize(toEye));
    result.responses[42] = abs(dot(facing[0],facing[2]))+abs(dot(facing[1],facing[2]));
    mat3 groundA = GE_ParticleBillboardBasis(mat3(1),cameraA,toEye,vec3(0),6u,0u,0.0);
    mat3 groundB = GE_ParticleBillboardBasis(mat3(1),cameraB,-toEye,vec3(0),6u,0u,0.0);
    result.responses[43] = dot(groundA[2],vec3(0,1,0));
    result.responses[44] = length(groundA[0]-groundB[0])+length(groundA[1]-groundB[1]);
    mat3 forward = GE_ParticleBillboardBasis(mat3(1),cameraA,toEye,vec3(1,2,3),7u,1u,0.0);
    result.responses[45] = dot(forward[2],normalize(vec3(1,2,3)));
    mat3 stationary = GE_ParticleBillboardBasis(mat3(1),cameraA,toEye,vec3(0),7u,1u,0.0);
    result.responses[46] = dot(stationary[2],vec3(0,0,1));
    mat3 pole = GE_ParticleBillboardBasis(mat3(1),cameraA,vec3(0,1,0),vec3(0),5u,0u,0.0);
    result.responses[47] = dot(pole[2],vec3(0,1,0));
    result.responses[48] = abs(dot(pole[0],pole[2]))+abs(dot(pole[1],pole[2]));
    // Full flap amplitude survives particle scaling, with a separate phase per index.
    result.responses[49] = GE_ParticleWingDisplacement(vec3(0,.25,0),0,0,1.57079632679,vec2(-.15,.15),.5).y;
    result.responses[50] = GE_ParticleWingDisplacement(vec3(0,.25,0),0,1,1.57079632679,vec2(-.15,.15),.5).y;
    result.responses[51] = GE_ParticleWingDisplacement(vec3(0,2,0),0,1,1.57079632679,vec2(-.15,.15),.5).y;
    result.responses[52] = GE_ParticleWingDisplacement(vec3(0,1,0),0,3,1.57079632679,vec2(-.15,.15),.5).y;
    result.responses[53] = GE_ParticleWingDisplacement(vec3(0,1,0),asin(.1),0,1,vec2(-.15,.15),.5).y;
}
)glsl";
    request.stages.push_back(stage);
    ShaderProgramCompileResult compiled;
    std::string error;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, device->PreferredShaderSource(), compiled, &error)) << error;
    device->BeginFrame();
    BufferDesc outputDesc;
    outputDesc.size = 54 * sizeof(float);
    outputDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    outputDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    const auto output = device->CreateBuffer(outputDesc);
    ASSERT_TRUE(output.IsValid());
    BufferDesc readbackDesc;
    readbackDesc.size = outputDesc.size;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    const auto readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());
    DescriptorSetLayoutDesc layout;
    DescriptorBinding binding;
    binding.binding=0; binding.type=DescriptorType::StorageBuffer; binding.count=1; binding.shaderStages=0x20;
    layout.bindings.push_back(binding);
    PipelineDesc pipelineDesc;
    pipelineDesc.type=PipelineType::Compute;
    pipelineDesc.computeShader=compiled.stageBytes.at("cs");
    pipelineDesc.descriptorSetLayouts.push_back(layout);
    const auto pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_NE(pipeline, INVALID_PIPELINE_HANDLE);
    DescriptorSetDesc setDesc; setDesc.layout=layout; setDesc.transient=false;
    const auto set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());
    device->UpdateStorageBufferBinding(set, 0, output, 0, outputDesc.size);
    auto command = device->CreateCommandList(IDevice::QueueType::Graphics);
    command->Begin(); command->SetPipeline(pipeline); command->BindDescriptorSet(0, set, pipeline);
    command->Dispatch(1,1,1);
    command->Barrier(ResourceBarrier::CreateBufferBarrier(output, ResourceState::UnorderedAccess, ResourceState::CopySource));
    command->CopyBuffer(output, readback, outputDesc.size, 0, 0);
    command->End();
    std::vector<CommandList*> commands{command.get()};
    device->ExecuteCommandLists(commands); device->FinalizeFrame(); device->WaitForIdle();
    const auto* values=static_cast<const float*>(device->MapBuffer(readback));
    ASSERT_NE(values, nullptr);
    const std::array<float,54> expected{0.1f,0.2f,0.3f,0.4f,0.5f,0.6f,0.2f,1.0f,0.5f,0.0f,1,1,1,1,1,1,1,1,1,0,0,0.3f,0.3f,
        0.1f,0.2f,0.7f,0.5f,0.6f,0.3f,0.4f,0.8f,1,1,0.75f,0,0.3f,0.1f,0.7f,0.2f,0.5f,0.6f,1,0,1,0,1,1,1,0,0,.075f,.075f,-.075f,.05f};
    for(size_t i=0;i<expected.size();++i) EXPECT_NEAR(values[i],expected[i],1e-5f) << "sample " << i;
    device->UnmapBuffer(readback);
    device->DestroyDescriptorSet(set); device->DestroyPipeline(pipeline);
    device->DestroyBuffer(output); device->DestroyBuffer(readback);
    std::filesystem::remove_all(request.cacheRoot);
}
