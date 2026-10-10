// An alpha-masked surface whose opacity comes from vertex alpha must be cut the
// same way by the depth passes and by the colour pass.
//
// The colour pass discards a Mask fragment whose opacity (albedo alpha x base
// colour alpha x vertex alpha) is below the cutoff. When a depth pass decides
// the same coverage without the vertex alpha, it keeps those fragments: the
// prepass writes the surface's depth over the area the colour pass then
// discards, the surface behind it fails the depth test there, and the user sees
// a hole in the clear colour; the shadow passes cast the cut-out area's shadow.
//
// The rendered tests draw that case through the engine's own world path: a
// real Mask material and mesh, the GPU scatter, the depth prepass the depth
// recorder records, and a depth readback. One draws a material that reads the
// vertex alpha, the other one that ignores vertex colour, whose depth draw must
// leave the cut-out area covered as its colour pass does. The rule test pins
// which depth draws read the colour stream, so the cost stays on the draws whose
// coverage needs it.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"

#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{

constexpr uint32_t kTargetSize = 64u;
constexpr uint64_t kWorldId = 11u;
// Reverse-Z depth under the identity camera: NDC z is the world z, and the
// larger value is the nearer surface. The masked card sits in front of the floor.
constexpr float kCardDepth = 0.75f;
constexpr float kFloorDepth = 0.25f;
constexpr float kDepthTolerance = 1e-3f;
// Pixels well inside each region: the card's vertex-alpha-0 half, its
// vertex-alpha-1 half, and the floor outside the card.
constexpr uint32_t kCutOutX = kTargetSize / 4u;
constexpr uint32_t kKeptX = kTargetSize * 3u / 4u;
constexpr uint32_t kCentreY = kTargetSize / 2u;
constexpr uint32_t kFloorX = 1u;
// The prepass variant of a material compiles on the first frame that draws it
// and publishes at the next frame begin; a few frames cover both materials.
constexpr int kMaxFrames = 8;

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 65536) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc desc{};
    desc.width = kTargetSize;
    desc.height = kTargetSize;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = usage;
    return desc;
}

Vertex MakeVertex(float x, float y, float z)
{
    Vertex v{};
    v.Position[0] = x;
    v.Position[1] = y;
    v.Position[2] = z;
    v.Normal[2] = -1.0f; // faces the camera, which looks down +Z
    return v;
}

// One axis-aligned rectangle at depth z, appended as two triangles.
void AppendRectangle(Mesh& mesh, float x0, float y0, float x1, float y1, float z)
{
    const uint32_t base = static_cast<uint32_t>(mesh.Vertices.size());
    mesh.Vertices.push_back(MakeVertex(x0, y0, z));
    mesh.Vertices.push_back(MakeVertex(x1, y0, z));
    mesh.Vertices.push_back(MakeVertex(x1, y1, z));
    mesh.Vertices.push_back(MakeVertex(x0, y1, z));
    for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
        mesh.Indices.push_back(base + index);
}

void SetBounds(Mesh& mesh)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        mesh.MinBounds[axis] = mesh.Vertices[0].Position[axis];
        mesh.MaxBounds[axis] = mesh.Vertices[0].Position[axis];
        for (const Vertex& v : mesh.Vertices)
        {
            mesh.MinBounds[axis] = std::min(mesh.MinBounds[axis], v.Position[axis]);
            mesh.MaxBounds[axis] = std::max(mesh.MaxBounds[axis], v.Position[axis]);
        }
    }
}

// The masked card: its left half carries vertex alpha 0 and its right half
// vertex alpha 1, each half its own rectangle so no interpolation crosses the
// cut. White RGB, so only the alpha differs.
Mesh MakeCardMesh()
{
    Mesh mesh{};
    mesh.Name = "VertexAlphaCard";
    AppendRectangle(mesh, -0.8f, -0.8f, 0.0f, 0.8f, kCardDepth);
    AppendRectangle(mesh, 0.0f, -0.8f, 0.8f, 0.8f, kCardDepth);
    for (size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        const float alpha = i < 4 ? 0.0f : 1.0f;
        mesh.Color0.insert(mesh.Color0.end(), {1.0f, 1.0f, 1.0f, alpha});
    }
    SetBounds(mesh);
    return mesh;
}

// The opaque floor behind the card, covering the whole target.
Mesh MakeFloorMesh()
{
    Mesh mesh{};
    mesh.Name = "Floor";
    AppendRectangle(mesh, -1.0f, -1.0f, 1.0f, 1.0f, kFloorDepth);
    SetBounds(mesh);
    return mesh;
}

MaterialDocument StandardDocument(const char* name, MaterialAlphaMode alphaMode)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = alphaMode;
    doc.doubleSided = true;
    if (alphaMode == MaterialAlphaMode::Mask)
        doc.properties["alphaCutoff"] = 0.5f;
    return doc;
}

uint32_t AddInstance(RenderServices& rs, const MeshGPUEntry& entry, const Material& material,
                     const Mesh& mesh)
{
    GPUInstance instance{};
    instance.transform = Matrix4x4::Identity();
    instance.prevTransform = Matrix4x4::Identity();
    instance.normalMatrixCol0 = Vector3(1.0f, 0.0f, 0.0f);
    instance.normalMatrixCol1 = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    instance.normalMatrixCol2 = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    instance.renderLayerMask = 1u;
    instance.meshIndex = entry.gpuMeshIndex;
    instance.materialIndex = material.GetGpuSceneMaterialIndex();
    instance.flags = DepthClassInstanceFlagBits(
                         rs.Materials().GetMaterialDepthClass(material.GetGpuSceneMaterialIndex())) |
                     (PackWorldKey16(kWorldId) << 16);
    instance.lodBias = 0.0f;
    instance.boundingCenter = Vector3((mesh.MinBounds[0] + mesh.MaxBounds[0]) * 0.5f,
                                      (mesh.MinBounds[1] + mesh.MaxBounds[1]) * 0.5f,
                                      (mesh.MinBounds[2] + mesh.MaxBounds[2]) * 0.5f);
    instance.boundingRadius = 1.5f;
    return rs.GetGPUScene()->AddInstance(instance);
}

float DepthAt(const ViewReadbackResult& result, uint32_t x, uint32_t y)
{
    float depth = -1.0f;
    const size_t offset = (static_cast<size_t>(y) * result.width + x) * sizeof(float);
    if (offset + sizeof(float) <= result.pixels.size())
        std::memcpy(&depth, result.pixels.data() + offset, sizeof(float));
    return depth;
}

// Draws the card, with the material `cardDocument` describes, over the opaque
// floor through the world path's depth prepass, and reads back the depth where
// the card's vertex alpha is 0. Frames repeat until the floor and the card's
// vertex-alpha-1 half both reach the depth target, so a variant still compiling
// cannot pass for a cut-out.
void DrawTheCardOverTheFloor(IDevice* device, const MaterialDocument& cardDocument, float& cutOutDepth)
{
    const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
    ASSERT_TRUE(std::filesystem::exists(shaderDir)) << "staged shader tree not found: " << shaderDir.string();
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device));
    ASSERT_NE(rs.GetGPUScene(), nullptr);

    const std::filesystem::path cacheRoot =
        std::filesystem::temp_directory_path() / ("ge_mask_vertex_alpha_" + GUID::Generate().ToString());
    MaterialBuildContext context{};
    context.AdapterShaderDir = shaderDir;
    context.CacheRoot = cacheRoot / "Shaders";
    context.IncludeDirs = {shaderDir};
    rs.Materials().SetMaterialBuildContext(context);

    Material* card = rs.Materials().RegisterMaterialFromDocument(GUID::Generate(), cardDocument);
    Material* floor = rs.Materials().RegisterMaterialFromDocument(
        GUID::Generate(), StandardDocument("OpaqueFloor", MaterialAlphaMode::Opaque));
    ASSERT_NE(card, nullptr);
    ASSERT_NE(floor, nullptr);
    ASSERT_EQ(card->GetAlphaMode(), MaterialAlphaMode::Mask) << "the card material must stay masked";
    ASSERT_EQ(card->IgnoresVertexColor(), cardDocument.ignoreVertexColor);

    const Mesh cardMesh = MakeCardMesh();
    const Mesh floorMesh = MakeFloorMesh();
    const MeshGPUHandle cardHandle = rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, cardMesh);
    const MeshGPUHandle floorHandle = rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, floorMesh);
    const MeshGPUEntry* cardEntry = rs.GetMeshGPURegistry().Find(cardHandle);
    const MeshGPUEntry* floorEntry = rs.GetMeshGPURegistry().Find(floorHandle);
    ASSERT_NE(cardEntry, nullptr);
    ASSERT_NE(floorEntry, nullptr);
    ASSERT_TRUE(HasFlag(cardEntry->vertexFlags, VertexAttributeFlags::HasColor))
        << "the card must upload its vertex colour stream";

    const CameraId cameraId = rs.Views().AllocateCamera("MaskVertexAlpha.Camera");
    CameraData camera{};
    for (int i = 0; i < 16; i += 5)
    {
        camera.view[i] = 1.0f;
        camera.proj[i] = 1.0f;
        camera.viewProj[i] = 1.0f;
    }
    rs.Views().SetCameraData(cameraId, camera);
    const ViewId viewId = rs.Views().AllocateView("MaskVertexAlpha.View", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, 1u);
    rs.Views().SetViewWorldId(viewId, kWorldId);
    ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    const uint32_t cardInstance = AddInstance(rs, *cardEntry, *card, cardMesh);
    const uint32_t floorInstance = AddInstance(rs, *floorEntry, *floor, floorMesh);
    std::array<WorldSubmissionRecord, 2> submissions{};
    submissions[0] = {viewId, cardHandle, card, cardInstance, 1u, 0u};
    submissions[1] = {viewId, floorHandle, floor, floorInstance, 1u, 0u};

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    std::optional<ViewReadbackResult> settled;
    for (int frameIndex = 0; frameIndex < kMaxFrames && !settled; ++frameIndex)
    {
        frame.BeginFrame(static_cast<uint64_t>(frameIndex));
        rs.BeginWorldDrawFrame();
        rs.GetPerFrameWritePool().BeginFrame(static_cast<uint32_t>(frameIndex));
        rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(submissions));
        rs.BuildWorldBatchKeys();
        rs.Materials().FinalizeFrameBuffers();
        if (rs.GetGPUScene()->IsDirty())
            rs.GetGPUScene()->FlushGPUBuffers();

        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
            "MaskVertexAlpha.Depth",
            TargetDesc(TextureFormat::D32_FLOAT,
                       static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));

        rs.ScheduleGpuSkinningAndRetarget(frame);
        rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
        rs.ScheduleWorldBucketerDispatches(frame);
        const RenderGraph::RGPass prepass = rs.AddWorldDepthPrepassForView(frame, viewId, depth, 0.0f);
        ASSERT_TRUE(prepass.IsValid());
        // The prepass narrows the view's world-pass keywords to the instanced
        // fetch at execution. No world pass is declared, so the depth read
        // back is the prepass's alone and no other draw binds the colour
        // stream it must bind itself; the keywords a world pass records are
        // recorded here the same way.
        rs.Views().PerView(viewId).WorldPassKeywords = MaterialKeyword::Instanced;
        frame.MarkOutput(depth);

        const auto ticket = RequestTextureReadbackRG(device, frame, depth, "MaskVertexAlpha.DepthReadback");
        ASSERT_TRUE(ticket);
        frame.Execute();
        OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        device->WaitForIdle();

        ViewReadbackResult result{};
        ASSERT_TRUE(ticket->TryGet(result)) << "the depth readback did not resolve";
        ASSERT_EQ(result.width, kTargetSize);
        ASSERT_EQ(result.pixels.size(), static_cast<size_t>(kTargetSize) * kTargetSize * sizeof(float));
        const bool floorDrawn = std::abs(DepthAt(result, kFloorX, kCentreY) - kFloorDepth) < kDepthTolerance;
        const bool cardDrawn = std::abs(DepthAt(result, kKeptX, kCentreY) - kCardDepth) < kDepthTolerance;
        if (floorDrawn && cardDrawn)
            settled = std::move(result);
    }

    ASSERT_TRUE(settled.has_value())
        << "the floor and the kept half of the card never both reached the depth target in " << kMaxFrames
        << " frames: the scene did not draw, so the cut-out cannot be judged";
    cutOutDepth = DepthAt(*settled, kCutOutX, kCentreY);

    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(cacheRoot, ec);
}

} // namespace

// The card's left half is cut away by its vertex alpha, so the prepass depth
// there must be the floor's: the colour pass discards those fragments, and a
// prepass that kept them would hide the floor behind a surface that never draws.
TEST(MaskVertexAlphaDepth, ThePrepassLeavesTheVertexAlphaCutOutToTheSurfaceBehind)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const MaterialDocument cardDocument = StandardDocument("VertexAlphaMask", MaterialAlphaMode::Mask);
    float cutOutDepth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), cardDocument, cutOutDepth));
    EXPECT_NEAR(cutOutDepth, kFloorDepth, kDepthTolerance)
        << "the depth where vertex alpha cuts the card away must be the floor's: the colour pass discards "
           "those fragments, so a prepass that writes the card's depth there leaves a hole";
    device->Shutdown();
}

// A Mask material that ignores vertex colour is not cut by the vertex alpha in
// its colour pass, so its prepass must keep the card's depth there too: a
// prepass that read the stream anyway would discard what the colour pass draws.
// Its base colour alpha of 0.9 passes the 0.5 cutoff everywhere and keeps it
// masked: DemoteMaskWithoutAlphaSource turns a Mask that ignores vertex colour
// into Opaque only when its base alpha is at least 0.999.
TEST(MaskVertexAlphaDepth, AMaskThatIgnoresVertexColourKeepsItsDepthWhereTheVertexAlphaIsZero)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    MaterialDocument cardDocument = StandardDocument("VertexColourIgnoringMask", MaterialAlphaMode::Mask);
    cardDocument.ignoreVertexColor = true;
    cardDocument.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.9f};
    float cutOutDepth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), cardDocument, cutOutDepth));
    EXPECT_NEAR(cutOutDepth, kCardDepth, kDepthTolerance)
        << "a material that ignores vertex colour draws its whole card in the colour pass, so the prepass must "
           "write the card's depth where the vertex alpha is 0";
    device->Shutdown();
}

// Which depth draws read the vertex colour: a Mask material's coverage fragment
// in every depth pass that evaluates its opacity, and nothing else, so an opaque
// material's depth and shadow draws keep the position-only layout. A material
// that ignores vertex colour opts out through its colour pass's layout, which
// has no Color stream (AMaskThatIgnoresVertexColourKeepsItsDepthWhereTheVertexAlphaIsZero).
TEST(MaskVertexAlphaDepth, OnlyAMaskedCoverageFragmentReadsTheVertexColour)
{
    constexpr DepthPassType kCoveragePasses[] = {
        DepthPassType::Prepass,     DepthPassType::ShadowCascade, DepthPassType::AreaShadow,
        DepthPassType::SpotShadow,  DepthPassType::PointShadow,   DepthPassType::DeformationMotion};
    for (const DepthPassType pass : kCoveragePasses)
    {
        EXPECT_TRUE(DepthPassReadsVertexColor(pass, MaterialAlphaMode::Mask))
            << "pass " << static_cast<int>(pass) << ": the coverage fragment must see the vertex alpha";
        EXPECT_FALSE(DepthPassReadsVertexColor(pass, MaterialAlphaMode::Opaque))
            << "pass " << static_cast<int>(pass) << ": an opaque draw keeps the position-only layout";
    }
    EXPECT_FALSE(DepthPassReadsVertexColor(DepthPassType::TransmittanceCascade, MaterialAlphaMode::Mask))
        << "the glass tint writes the transmission colour without evaluating opacity";
}
