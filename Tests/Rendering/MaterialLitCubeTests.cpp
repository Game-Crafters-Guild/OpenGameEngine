#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "Assets/MaterialAsset.h"
#include "Engine/Rendering/MaterialSsboLayout.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

constexpr uint32_t kW = 128;
constexpr uint32_t kH = 128;

// Set 0 is the engine set; set 1 carries the bindless texture/sampler arrays every
// adapter declares. The test surface samples nothing, so set 1 is bound unwritten.
constexpr uint32_t kEngineSet = 0;
constexpr uint32_t kBindlessSet = 1;

void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

// Writes a material value at the lane the legacy name -> lane map places it.
// The test surface reads the row through the Mat.uBaseColor / Mat.uParams0
// aliases of material_param_lanes.glsl, and LegacyMaterialLaneTests pins those
// aliases to this map, so writer and reader share one placement.
void WriteLegacyLane(uint8_t* row, std::string_view name, std::initializer_list<float> values)
{
    const LegacyMaterialLane* lane = FindLegacyMaterialLane(name);
    ASSERT_NE(lane, nullptr) << name;
    ASSERT_EQ(lane->Components, values.size()) << name;
    std::memcpy(row + LegacyLaneByteOffset(*lane), std::data(values), LegacyLaneByteSize(*lane));
}

struct Vertex
{
    float pos[3];
    float nrm[3];
    float uv[2];
};

// 24-vertex cube in [-0.5, 0.5] (unique normals/UVs per face).
void BuildCube(std::vector<Vertex>& outVerts, std::vector<uint16_t>& outIdx)
{
    outVerts.clear();
    outIdx.clear();

    auto pushFace = [&](float nx, float ny, float nz,
                        const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d)
    {
        const uint16_t base = static_cast<uint16_t>(outVerts.size());
        outVerts.push_back(Vertex{{a.x, a.y, a.z}, {nx, ny, nz}, {0.0f, 0.0f}});
        outVerts.push_back(Vertex{{b.x, b.y, b.z}, {nx, ny, nz}, {1.0f, 0.0f}});
        outVerts.push_back(Vertex{{c.x, c.y, c.z}, {nx, ny, nz}, {1.0f, 1.0f}});
        outVerts.push_back(Vertex{{d.x, d.y, d.z}, {nx, ny, nz}, {0.0f, 1.0f}});
        for (int i : {0, 1, 2, 0, 2, 3})
            outIdx.push_back(static_cast<uint16_t>(base + i));
    };

    const Vector3 p000(-0.5f, -0.5f, -0.5f);
    const Vector3 p001(-0.5f, -0.5f,  0.5f);
    const Vector3 p010(-0.5f,  0.5f, -0.5f);
    const Vector3 p011(-0.5f,  0.5f,  0.5f);
    const Vector3 p100( 0.5f, -0.5f, -0.5f);
    const Vector3 p101( 0.5f, -0.5f,  0.5f);
    const Vector3 p110( 0.5f,  0.5f, -0.5f);
    const Vector3 p111( 0.5f,  0.5f,  0.5f);

    pushFace( 1,  0,  0, p101, p100, p110, p111); // +X
    pushFace(-1,  0,  0, p001, p101, p111, p011); // -X
    pushFace( 0,  1,  0, p011, p111, p110, p010); // +Y
    pushFace( 0, -1,  0, p001, p000, p100, p101); // -Y
    pushFace( 0,  0,  1, p001, p101, p111, p011); // +Z
    pushFace( 0,  0, -1, p100, p000, p010, p110); // -Z
}

BufferHandle CreateUploadBuffer(IDevice& dev, BufferUsage usage, size_t size, const char* debugName)
{
    BufferDesc d{};
    d.size = size;
    d.usage = static_cast<uint32_t>(usage);
    d.memoryUsage = BufferMemoryUsage::Upload;
    d.flags = BufferCreateFlags::PersistentlyMapped;
    d.debugName = debugName;
    return dev.CreateBuffer(d);
}

void Upload(IDevice& dev, BufferHandle buffer, const void* data, size_t size)
{
    void* p = dev.MapBuffer(buffer);
    ASSERT_NE(p, nullptr);
    std::memcpy(p, data, size);
    dev.UnmapBuffer(buffer);
}

// Mirrors adapter_vertex.glsl's non-instanced push-constant block.
struct PushConstants
{
    float uM[16];
    float uN0[4];
    float uN1[4];
    float uN2[4];
    float uExtra[4]; // x = materialIndex (uint bits), y = skinPaletteOffset (uint bits)
};
static_assert(sizeof(PushConstants) == 128, "push-constant block drifted from adapter_vertex.glsl");

} // namespace

// A schema-v2 .material document composes through the adapters, compiles to
// SPIR-V, reflects into a pipeline, and draws a lit cube through the raw device
// against the engine set-0 contract (camera + light UBOs, MaterialParams SSBO
// rows). With the Khronos validation layer discoverable (the default SDK
// install), the engine's validation assert traps the process on any validation
// ERROR the draw raises, so a binding-contract drift fails here instead of
// shading garbage; without a layer the test rests on the colour assertions
// alone.
TEST(MaterialShaders, MaterialV2_RendersLitCube)
{
    // Headless friendly
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    // Temp workspace with Assets/ so MaterialAsset can locate its workspace root.
    const std::filesystem::path tmpRoot = std::filesystem::temp_directory_path() / "ge_material_lit_cube";
    const std::filesystem::path assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);

    const auto surfacePath = assetsRoot / "Shaders" / "test_surface.glsl";
    const auto materialPath = assetsRoot / "Materials" / "test.material";

    // Reads its parameters from the MaterialParams row the way the shipped
    // surfaces do (Assets/Materials/Surfaces/standard_pbr.glsl), so a wrong row
    // layout shows up as a wrong colour.
    WriteTextFile(surfacePath,
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
                  "{\n"
                  "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "    o.baseColor = Mat.uBaseColor.rgb;\n"
                  "    o.metallic = Mat.uParams0.x;\n"
                  "    o.roughness = Mat.uParams0.y;\n"
                  "    o.normalWS = normalize(sIn.normalWS);\n"
                  "    return o;\n"
                  "}\n");

    WriteTextFile(materialPath,
                  "{\n"
                  "  \"schemaVersion\": 2,\n"
                  "  \"materialName\": \"TestMaterial\",\n"
                  "  \"surfaceShader\": \"../Shaders/test_surface.glsl\",\n"
                  "  \"properties\": {},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    MaterialAsset mat(GUID{}, materialPath);
    ASSERT_TRUE(mat.Load()) << (mat.GetErrors().empty() ? "" : mat.GetErrors().front());
    ASSERT_TRUE(mat.GetErrors().empty());

    Rendering::MaterialBuildContext ctx;
    ctx.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
    ctx.CacheRoot = tmpRoot / ".Cache" / "Shaders";
    ctx.IncludeDirs = {assetsRoot};
    auto built = Rendering::BuildMaterialToShaderPackage(
        mat.GetDocument(), materialPath, mat.GetName(), ctx, Rendering::ShaderSourceKind::SpirV);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? "" : built.errors.front());
    ASSERT_NE(built.package, nullptr);

    DeviceDesc dd{};
    dd.applicationName = "MaterialLitCube";
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = true;
    dd.enableDescriptorValidation = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        GTEST_SKIP() << "Device init failed";
    }

    // Offscreen targets
    TextureDesc colorD{};
    colorD.width = kW;
    colorD.height = kH;
    colorD.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    colorD.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    colorD.debugName = "MaterialLit.Color";
    TextureHandle color = dev->CreateTexture(colorD);
    ASSERT_TRUE(color.IsValid());

    TextureDesc depthD{};
    depthD.width = kW;
    depthD.height = kH;
    depthD.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthD.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    depthD.debugName = "MaterialLit.Depth";
    TextureHandle depth = dev->CreateTexture(depthD);
    ASSERT_TRUE(depth.IsValid());

    // Geometry
    std::vector<Vertex> verts;
    std::vector<uint16_t> idx;
    BuildCube(verts, idx);

    BufferHandle vb = CreateUploadBuffer(*dev, BufferUsage::Vertex, verts.size() * sizeof(Vertex), "MaterialLit.CubeVB");
    ASSERT_TRUE(vb.IsValid());
    Upload(*dev, vb, verts.data(), verts.size() * sizeof(Vertex));

    BufferHandle ib = CreateUploadBuffer(*dev, BufferUsage::Index, idx.size() * sizeof(uint16_t), "MaterialLit.CubeIB");
    ASSERT_TRUE(ib.IsValid());
    Upload(*dev, ib, idx.data(), idx.size() * sizeof(uint16_t));

    // Engine set-0 blocks, through the production C++ mirrors: their static_asserts
    // pin the byte layout the adapters index, so a GLSL-side change fails to compile
    // here rather than binding a short buffer.
    const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
    const Vector3 eye(0.0f, 0.0f, 2.0f);
    const Matrix4x4 V = Mathematics::MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f)).ToVulkan();
    constexpr float kNear = 0.1f;
    constexpr float kFar = 10.0f;
    const Matrix4x4 P = Mathematics::MakePerspectiveLH_ZO_ReverseZ(Math::ToRadians(60.0f), aspect, kNear, kFar).ToVulkan();
    const Matrix4x4 VP = P * V;

    CameraData cam{};
    std::memcpy(cam.view, V.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, P.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, VP.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = eye.x;
    cam.cameraPos[1] = eye.y;
    cam.cameraPos[2] = eye.z;
    // Render origin inactive (sector 0): the rebased matrices equal the full-world ones.
    std::memcpy(cam.viewRel, V.Data(), sizeof(cam.viewRel));
    std::memcpy(cam.viewProjRel, VP.Data(), sizeof(cam.viewProjRel));
    BufferHandle camBuf = CreateUploadBuffer(*dev, BufferUsage::Uniform, sizeof(CameraData), "MaterialLit.CameraUBO");
    ASSERT_TRUE(camBuf.IsValid());
    Upload(*dev, camBuf, &cam, sizeof(cam));

    // One white directional from above-front plus white ambient, so the +Z face the
    // camera looks at is lit but nowhere near saturation.
    GameEngine::Engine::Renderer::ForwardLightUBO light{};
    const Matrix4x4 identity = Matrix4x4::Identity().ToVulkan();
    std::memcpy(light.uLightVP, identity.Data(), sizeof(light.uLightVP));
    const Vector3 lightDir = Vector3(0.0f, -1.0f, -0.5f).Normalize(); // light -> surface
    light.uLightDirWorld[0] = lightDir.x;
    light.uLightDirWorld[1] = lightDir.y;
    light.uLightDirWorld[2] = lightDir.z;
    light.uLightDirWorld[3] = 1.0f; // intensity
    light.uLightColorWorld[0] = light.uLightColorWorld[1] = light.uLightColorWorld[2] = 1.0f;
    light.uAmbient[0] = light.uAmbient[1] = light.uAmbient[2] = 1.0f;
    light.uAmbient[3] = 0.25f; // intensity
    BufferHandle lightBuf = CreateUploadBuffer(*dev, BufferUsage::Uniform, sizeof(light), "MaterialLit.LightUBO");
    ASSERT_TRUE(lightBuf.IsValid());
    Upload(*dev, lightBuf, &light, sizeof(light));

    ViewParamsUBO viewParams{};
    std::memcpy(viewParams.ge_view, V.Data(), sizeof(viewParams.ge_view));
    std::memcpy(viewParams.ge_proj, P.Data(), sizeof(viewParams.ge_proj));
    std::memcpy(viewParams.ge_viewProj, VP.Data(), sizeof(viewParams.ge_viewProj));
    std::memcpy(viewParams.ge_prevViewProj, VP.Data(), sizeof(viewParams.ge_prevViewProj));
    viewParams.ge_nearFar[0] = kNear;
    viewParams.ge_nearFar[1] = kFar;
    viewParams.ge_nearFar[2] = 1.0f / std::log(kFar / kNear);
    viewParams.ge_cameraPosWS[0] = eye.x;
    viewParams.ge_cameraPosWS[1] = eye.y;
    viewParams.ge_cameraPosWS[2] = eye.z;
    viewParams.ge_screenSize[0] = static_cast<float>(kW);
    viewParams.ge_screenSize[1] = static_cast<float>(kH);
    viewParams.ge_screenSize[2] = 1.0f / static_cast<float>(kW);
    viewParams.ge_screenSize[3] = 1.0f / static_cast<float>(kH);
    BufferHandle viewBuf = CreateUploadBuffer(*dev, BufferUsage::Uniform, sizeof(viewParams), "MaterialLit.ViewParams");
    ASSERT_TRUE(viewBuf.IsValid());
    Upload(*dev, viewBuf, &viewParams, sizeof(viewParams));

    // Two MaterialParams rows, laid out at the C++ stride. Row 0 is a cool decoy;
    // the drawn material index is 1, so the warm colour is read only if the shader's
    // std430 stride for MaterialData matches kMaterialEntryStride — a row read at
    // offset 0 would pass whatever the stride. Texture indices, UV transforms and
    // sampler indices stay zero: the surface samples nothing.
    using GameEngine::Engine::Renderer::kMaterialEntryStride;
    constexpr uint32_t kDrawnMaterialIndex = 1u;
    std::vector<uint8_t> materialRows(kMaterialEntryStride * (kDrawnMaterialIndex + 1u), 0u);
    uint8_t* decoyRow = materialRows.data();
    WriteLegacyLane(decoyRow, "baseColor", {0.2f, 0.6f, 0.8f, 1.0f});
    WriteLegacyLane(decoyRow, "metallic", {0.0f});
    WriteLegacyLane(decoyRow, "roughness", {0.6f});
    uint8_t* drawnRow = materialRows.data() + kDrawnMaterialIndex * kMaterialEntryStride;
    WriteLegacyLane(drawnRow, "baseColor", {0.8f, 0.6f, 0.2f, 1.0f});
    WriteLegacyLane(drawnRow, "metallic", {0.0f});
    WriteLegacyLane(drawnRow, "roughness", {0.6f});
    BufferHandle matBuf = CreateUploadBuffer(*dev, BufferUsage::Storage, materialRows.size(), "MaterialLit.MaterialParams");
    ASSERT_TRUE(matBuf.IsValid());
    Upload(*dev, matBuf, materialRows.data(), materialRows.size());

    // Pipeline from the compiled package
    const auto& pkg = *built.package;
    const auto itVs = pkg.stageBytes.find("vs");
    const auto itFs = pkg.stageBytes.find("fs");
    ASSERT_TRUE(itVs != pkg.stageBytes.end());
    ASSERT_TRUE(itFs != pkg.stageBytes.end());
    ASSERT_FALSE(itVs->second.empty());
    ASSERT_FALSE(itFs->second.empty());

    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = itVs->second;
    pd.pixelShader = itFs->second;
    pd.debugName = "MaterialLit.LitCube";
    pd.topology = PrimitiveTopology::TriangleList;
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.EnableDepthTest(true, CompareOp::Greater); // reverse-Z: nearer is greater
    pd.SetCullingMode(CullModeFlagBits::None, FrontFace::CounterClockwise);
    pd.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
    pd.depthAttachmentFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    pd.vertexBindings.push_back(VertexInputBinding{0u, static_cast<uint32_t>(sizeof(Vertex)), 0u});
    pd.vertexAttributes.push_back(VertexInputAttribute{0u, 0u, Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(Vertex, pos))});
    pd.vertexAttributes.push_back(VertexInputAttribute{1u, 0u, Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(Vertex, nrm))});
    pd.vertexAttributes.push_back(VertexInputAttribute{2u, 0u, Format::R32G32_FLOAT, static_cast<uint32_t>(offsetof(Vertex, uv))});

    MaterialBuilder::FormatsHint fh{};
    fh.ColorFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
    fh.DepthFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    std::string err;
    ASSERT_TRUE(MaterialBuilder::BuildPipelineDescFromMeta(pkg.meta, pd, fh, MaterialBuilder::MergeMode::Auto,
                                                           MaterialBuilder::PushConstantPolicy{}, &err))
        << err;

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    ASSERT_GE(pd.descriptorSetLayouts.size(), 2u) << "adapters declare the engine set and the bindless set";
    DescriptorSetHandle engineSet =
        dev->CreateDescriptorSet(DescriptorSetDesc{pd.descriptorSetLayouts[kEngineSet], "MaterialLit.EngineSet", true});
    DescriptorSetHandle bindlessSet =
        dev->CreateDescriptorSet(DescriptorSetDesc{pd.descriptorSetLayouts[kBindlessSet], "MaterialLit.BindlessSet", true});
    ASSERT_TRUE(engineSet.IsValid());
    ASSERT_TRUE(bindlessSet.IsValid());

    // Every buffer the engine set reflects gets bound, matched by the name the
    // shader reflects (the names RenderServicesWorldPass registers): the blocks this
    // draw needs with real data, anything else with a zero-filled block of the
    // reflected size — the fallback contract RenderServices keeps for blocks a pass
    // does not supply. A uniform block's reflected size must equal its C++ mirror,
    // and a set-0 image binding has no fallback here: both fail loudly.
    struct NamedBlock
    {
        const char* Name;
        BufferHandle Buffer;
        size_t Bytes;
    };
    const NamedBlock namedBlocks[] = {
        {"Cam", camBuf, sizeof(CameraData)},
        {"LightUBO", lightBuf, sizeof(light)},
        {"Light", lightBuf, sizeof(light)},
        {"ViewParams", viewBuf, sizeof(viewParams)},
        {"MaterialParams", matBuf, materialRows.size()},
    };
    std::vector<BufferHandle> fallbackBuffers;
    size_t namedBound = 0;
    for (const auto& set : pkg.meta.Sets)
    {
        if (set.Set != kEngineSet)
            continue;
        for (const auto& b : set.Bindings)
        {
            const bool isUniform = b.Type == ShaderMetaBindingType::kUniformBuffer;
            const bool isStorage = b.Type == ShaderMetaBindingType::kStorageBuffer;
            ASSERT_TRUE(isUniform || isStorage)
                << "engine-set binding " << b.Binding << " (" << b.Name << ") is not a buffer";

            const NamedBlock* named = nullptr;
            for (const NamedBlock& candidate : namedBlocks)
                if (b.Name == candidate.Name)
                    named = &candidate;

            BufferHandle buffer{};
            size_t bytes = 0;
            if (named)
            {
                buffer = named->Buffer;
                bytes = named->Bytes;
                ++namedBound;
                if (isUniform && b.Block)
                    EXPECT_EQ(b.Block->Size, bytes) << b.Name << " block size drifted from its C++ mirror";
                if (b.Name == "MaterialParams")
                {
                    // The runtime array's reflected std430 stride is the row stride the
                    // C++ writer must use.
                    ASSERT_TRUE(b.Block) << "MaterialParams reflected without a block layout";
                    const auto rows = std::find_if(b.Block->Members.begin(), b.Block->Members.end(),
                                                   [](const Member& m) { return m.Name == "ge_Materials"; });
                    ASSERT_TRUE(rows != b.Block->Members.end()) << "MaterialParams has no ge_Materials member";
                    ASSERT_TRUE(rows->ArrayStride) << "ge_Materials reflected without an array stride";
                    EXPECT_EQ(*rows->ArrayStride, kMaterialEntryStride) << "MaterialData std430 stride drifted from kMaterialEntryStride";
                }
            }
            else
            {
                bytes = b.Block ? std::max<size_t>(b.Block->Size, sizeof(GpuVec4)) : sizeof(GpuVec4);
                const std::vector<uint8_t> zeros(bytes, 0u);
                buffer = CreateUploadBuffer(*dev, isStorage ? BufferUsage::Storage : BufferUsage::Uniform, bytes,
                                            "MaterialLit.Fallback");
                ASSERT_TRUE(buffer.IsValid()) << b.Name;
                Upload(*dev, buffer, zeros.data(), bytes);
                fallbackBuffers.push_back(buffer);
            }

            if (isStorage)
                dev->UpdateStorageBufferBinding(engineSet, b.Binding, buffer, 0, bytes);
            else
                dev->UpdateBufferBinding(engineSet, b.Binding, buffer, 0, bytes);
        }
    }
    ASSERT_EQ(namedBound, 4u) << "the engine set must reflect Cam, LightUBO, ViewParams and MaterialParams";

    // Record draw
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(static_cast<bool>(cl));
    cl->Begin();

    cl->Barrier(ResourceBarrier::CreateTextureBarrier(color, ResourceState::Undefined, ResourceState::RenderTarget));
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(depth, ResourceState::Undefined, ResourceState::DepthWrite));

    RenderPassDesc rp{};
    rp.colorTargets[0] = color;
    rp.colorTargetCount = 1;
    rp.depthTarget = depth;
    rp.clearColor[0] = true;
    // Non-black clear so the readback itself is verified.
    rp.clearColorValue[0][0] = 0.05f;
    rp.clearColorValue[0][1] = 0.07f;
    rp.clearColorValue[0][2] = 0.11f;
    rp.clearColorValue[0][3] = 1.0f;
    rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    rp.clearDepth = true;
    rp.clearDepthValue = 0.0f; // reverse-Z far
    rp.depthStoreOp = RenderPassDesc::StoreOp::Store;

    cl->BeginRenderPass(rp);
    cl->SetViewport(0, 0, kW, kH);
    cl->SetScissor(0, 0, kW, kH);
    cl->SetPipeline(pipe);
    cl->BindDescriptorSet(kEngineSet, engineSet, pipe);
    cl->BindDescriptorSet(kBindlessSet, bindlessSet, pipe);
    cl->SetVertexBuffer(vb, 0);
    cl->SetIndexBuffer(ib, IndexType::Uint16);

    PushConstants pc{};
    std::memcpy(pc.uM, identity.Data(), sizeof(pc.uM));
    pc.uN0[0] = 1.0f;
    pc.uN1[1] = 1.0f;
    pc.uN2[2] = 1.0f;
    std::memcpy(&pc.uExtra[0], &kDrawnMaterialIndex, sizeof(kDrawnMaterialIndex));
    cl->SetPushConstants(pc);

    cl->DrawIndexed(static_cast<uint32_t>(idx.size()), 1);
    cl->EndRenderPass();

    cl->Barrier(ResourceBarrier::CreateTextureBarrier(color, ResourceState::RenderTarget, ResourceState::CopySource));

    BufferHandle rb = dev->CreateReadbackBuffer(kW * kH * 4u);
    ASSERT_TRUE(rb.IsValid());
    cl->CopyTextureToBuffer(color, rb, kW, kH);

    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    const uint8_t* px = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(px, nullptr);

    auto toByte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    const uint8_t clearR = toByte(rp.clearColorValue[0][0]);
    const uint8_t clearG = toByte(rp.clearColorValue[0][1]);
    const uint8_t clearB = toByte(rp.clearColorValue[0][2]);

    // Corner pixel: outside the cube, holds the clear colour (readback works).
    EXPECT_EQ(px[0], clearR);
    EXPECT_EQ(px[1], clearG);
    EXPECT_EQ(px[2], clearB);

    // Centre pixel: the +Z face, lit with row 1's warm base colour. The channel
    // ordering survives any lighting scale, so it pins that the shader read row 1 at
    // kMaterialEntryStride: an unbound buffer, a short row, or a stride drift lands
    // on the cool decoy (B > G > R) or on zeros instead.
    const uint8_t* centre = px + ((kH / 2) * kW + (kW / 2)) * 4;
    EXPECT_GT(centre[0], centre[1]);
    EXPECT_GT(centre[1], centre[2]);
    EXPECT_GT(centre[0], clearR);
    dev->UnmapBuffer(rb);

    dev->DestroyBuffer(rb);
    dev->DestroyDescriptorSet(bindlessSet);
    dev->DestroyDescriptorSet(engineSet);
    for (BufferHandle fallback : fallbackBuffers)
        dev->DestroyBuffer(fallback);
    dev->DestroyBuffer(viewBuf);
    dev->DestroyTexture(depth);
    dev->DestroyTexture(color);
    dev->DestroyBuffer(matBuf);
    dev->DestroyBuffer(lightBuf);
    dev->DestroyBuffer(camBuf);
    dev->DestroyBuffer(ib);
    dev->DestroyBuffer(vb);
    dev->DestroyPipeline(pipe);
}
