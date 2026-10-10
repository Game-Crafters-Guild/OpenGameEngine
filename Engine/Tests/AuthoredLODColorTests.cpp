#include "Assets/AuthoredLodImport.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Assets/HlodMeshMerge.h"
#include "Assets/MeshLODCache.h"
#include "Assets/MeshLODGeometry.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/GPUScene.h"
#include "TestDeviceHelper.h"
#include "ModelAssetFbxTestAccess.h"
#include "StagedTestPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace {

// Vertex and RGBA values identify both their level and their vertex. Unequal
// counts make an accidentally shared/repeated LOD0 stream observably wrong.
Mesh Level(const String& name, uint32 count, uint32 level, bool coloured = true) {
    Mesh mesh{};
    mesh.Name = name;
    mesh.MaterialIndex = coloured ? 7u : 9u;
    mesh.SourceNodeIndex = static_cast<int32>(level);
    mesh.Vertices.resize(count);
    for (uint32 i = 0; i < count; ++i) {
        auto& v = mesh.Vertices[i];
        v.Position[0] = static_cast<float>(level * 10u + i);
        v.Position[1] = static_cast<float>(i & 1u);
        v.Normal[2] = 1.0f;
        v.Tangent[0] = v.Tangent[3] = 1.0f;
        v.TexCoords[0] = 0.1f * static_cast<float>(i);
        if (coloured)
            mesh.Color0.insert(mesh.Color0.end(), {0.1f * (level + 1u),
                0.01f * (i + 1u), 0.2f, 0.3f + 0.1f * level});
    }
    mesh.Indices = {count - 1u, 1u, 0u};
    mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = -1.0f;
    mesh.MaxBounds[0] = 40.0f; mesh.MaxBounds[1] = mesh.MaxBounds[2] = 2.0f;
    return mesh;
}

// The same independent inputs enter each real assembly boundary.
Vector<Mesh> Assemble(uint32 route, Vector<Mesh> meshes) {
    if (route == 0u) {
        ConsumeLodSuffixFamilies(meshes);
    } else if (route == 1u) {
        Vector<Mesh> targets;
        for (const auto& m : meshes) if (m.SourceNodeIndex == 0) targets.push_back(m);
        for (uint32 level = 1; level <= 2; ++level) {
            Vector<Mesh> slot;
            for (const auto& m : meshes)
                if (m.SourceNodeIndex == static_cast<int32>(level)) slot.push_back(m);
            if (!slot.empty()) AppendSlotLevelByName(targets, slot, level, {}, "RGBA test");
        }
        meshes = std::move(targets);
    } else {
        Vector<int32> lower{1};
        if (std::any_of(meshes.begin(), meshes.end(), [](const Mesh& m) { return m.SourceNodeIndex == 2; }))
            lower.push_back(2);
        if (route == 2u) {
            MsftLodGroup group; group.Lod0Node = 0; group.LowerNodes = lower;
            AssembleMsftLodChains(meshes, {group}, "RGBA test");
        } else {
            FbxLodGroup group; group.Lod0Node = 0; group.LowerNodes = lower;
            group.SwitchDistances.assign(lower.size(), 0.0f); // default-table source metadata
            AssembleFbxLodGroupChains(meshes, {group}, "RGBA test");
        }
    }
    return meshes;
}

Vector<Mesh> Family() {
    return {Level("Leaves", 7, 0), Level("Bark", 6, 0, false),
            Level("Leaves_LOD1", 5, 1), Level("Bark_LOD1", 4, 1, false),
            Level("Leaves_LOD2", 3, 2), Level("Bark_LOD2", 3, 2, false)};
}

Mesh ColouredChain() { return Assemble(0, Family())[0]; }

class AuthoredColourCache : public ::testing::Test {
protected:
    void SetUp() override {
        folder = std::filesystem::temp_directory_path() / ("ge-lod-rgba-" + GUID::Generate().ToString());
        std::filesystem::create_directories(folder);
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(folder, ec); }
    std::filesystem::path folder;
};

class AuthoredColourGPU : public ::testing::Test {
protected:
    void SetUp() override {
        device = CreateVulkanDeviceFast();
        if (!device) GTEST_SKIP() << "No Vulkan device available";
        scene = std::make_unique<GPUScene>(device.get());
        ASSERT_TRUE(scene->Initialize(1024u, 256u));
        registry.Initialize(device.get());
        registry.SetGPUScene(scene.get());
    }
    void TearDown() override {
        registry.Shutdown();
        if (scene) scene->Shutdown();
        scene.reset();
        if (device) device->Shutdown();
    }
    template<class T> std::vector<T> Read(BufferHandle source, size_t offset, size_t count) {
        const size_t bytes = count * sizeof(T);
        auto readback = device->CreateReadbackBuffer(bytes, "Authored RGBA readback");
        auto cmd = device->CreateCommandList(IDevice::QueueType::Graphics);
        cmd->Begin(); cmd->CopyBuffer(source, readback, bytes, offset); cmd->End();
        std::vector<CommandList*> commands{cmd.get()};
        device->ExecuteCommandLists(commands); device->WaitForIdle();
        std::vector<T> out(count);
        const void* mapped = device->MapBuffer(readback);
        if (mapped) std::memcpy(out.data(), mapped, bytes);
        EXPECT_NE(mapped, nullptr);
        device->UnmapBuffer(readback); device->DestroyBuffer(readback);
        return out;
    }
    std::unique_ptr<IDevice> device;
    std::unique_ptr<GPUScene> scene;
    MeshGPURegistry registry;
};

} // namespace

TEST(AuthoredLODColours, AllFourImportRoutesPreservePerMaterialUnequalRgbaBlocks) {
    const auto original = Family();
    for (uint32 route = 0; route < 4; ++route) {
        SCOPED_TRACE(route);
        const auto meshes = Assemble(route, original);
        ASSERT_EQ(meshes.size(), 2u);
        const auto& leaves = meshes[0];
        const auto& bark = meshes[1];
        ASSERT_EQ(leaves.LODCount(), 3u);
        ASSERT_EQ(bark.LODCount(), 3u);
        ASSERT_EQ(leaves.ExtraLODColor0.size(), 2u);
        EXPECT_TRUE(leaves.HasValidLODColor0());
        EXPECT_EQ(leaves.Color0, original[0].Color0);
        EXPECT_EQ(leaves.ExtraLODColor0[0], original[2].Color0);
        EXPECT_EQ(leaves.ExtraLODColor0[1], original[4].Color0);
        EXPECT_EQ(leaves.ExtraLODVertices[0].size(), 5u);
        EXPECT_EQ(leaves.ExtraLODVertices[1].size(), 3u);
        EXPECT_EQ(leaves.ExtraLODs[1], original[4].Indices);
        EXPECT_TRUE(bark.Color0.empty());
        EXPECT_TRUE(bark.ExtraLODColor0.empty());
    }
}

TEST(AuthoredLODColours, InvalidOrMorphedMaterialChainDoesNotBlockColourlessSibling) {
    for (uint32 route = 0; route < 4; ++route) for (uint32 bad = 0; bad < 8; ++bad) {
        SCOPED_TRACE(route);
        SCOPED_TRACE(bad);
        auto family = Family(); family.resize(4); // one lower level, two materials
        Mesh& m = family[(bad & 1u) ? 2 : 0];
        switch (bad / 2u) {
        case 0: m.Color0.pop_back(); break;
        case 1: m.Color0[0] = std::numeric_limits<float>::infinity(); break;
        case 2: m.Color0.clear(); break;
        case 3: m.MorphTargets.resize(1); break;
        }
        auto out = Assemble(route, std::move(family));
        const auto leaves = std::find_if(out.begin(), out.end(), [](const Mesh& m) { return m.Name == "Leaves"; });
        const auto bark = std::find_if(out.begin(), out.end(), [](const Mesh& m) { return m.Name == "Bark"; });
        ASSERT_NE(leaves, out.end()); ASSERT_NE(bark, out.end());
        EXPECT_EQ(leaves->LODCount(), 1u);
        EXPECT_FALSE(leaves->HasAuthoredLODs());
        EXPECT_EQ(bark->LODCount(), 2u);
    }
}

TEST_F(AuthoredColourCache, AuthoredPlaceholderPreservesFreshSourceRgbaAndChecksSourceIdentity) {
    auto source = Family();
    Vector<Mesh> original{Assemble(0, source)[0]};
    const LodCacheKey key{ComputeLodSourceHash(reinterpret_cast<const uint8*>(source[2].Color0.data()),
                                             source[2].Color0.size() * sizeof(float)),
                          ComputeLodConfigHash({}, false, 0)};
    const auto path = folder / "authored.gelod";
    ASSERT_TRUE(WriteLodCache(path, key, ComputeGeneratedLodHash(original), original));
    auto unchanged = original;
    ASSERT_EQ(ReadLodCacheInto(path, &key, unchanged), LodCacheStatus::Hit);
    EXPECT_EQ(unchanged[0].ExtraLODColor0, original[0].ExtraLODColor0);

    source[2].Color0[3] = 0.125f; // geometry/indices unchanged; newly parsed source wins
    auto parsed = Assemble(0, source);
    parsed.resize(1);
    auto changedKey = key;
    changedKey.SourceHash = ComputeLodSourceHash(reinterpret_cast<const uint8*>(source[2].Color0.data()),
                                               source[2].Color0.size() * sizeof(float));
    EXPECT_NE(changedKey.SourceHash, key.SourceHash);
    EXPECT_EQ(ReadLodCacheInto(path, &changedKey, parsed), LodCacheStatus::KeyMismatch);
    EXPECT_FLOAT_EQ(parsed[0].ExtraLODColor0[0][3], 0.125f);
    // Player mode has no source key: authored cache data still cannot replace
    // the freshly parsed source stream with serialized or formerly white data.
    ASSERT_EQ(ReadLodCacheInto(path, nullptr, parsed), LodCacheStatus::Hit);
    EXPECT_FLOAT_EQ(parsed[0].ExtraLODColor0[0][3], 0.125f);
    EXPECT_EQ(ComputeGeneratedLodHash(parsed), ComputeGeneratedLodHash(original));
}

TEST_F(AuthoredColourCache, CachedProvenanceCannotReplaceAuthoredOrGeneratedSource) {
    Vector<Mesh> authored{ColouredChain()};
    Mesh generated = Level("Leaves", 7, 0);
    generated.ExtraLODs = {{0, 1, 2}};
    Vector<Mesh> plain{generated};
    const LodCacheKey key{1, 2};
    for (bool cachedAuthored : {false, true}) {
        const auto path = folder / (cachedAuthored ? "a.gelod" : "g.gelod");
        const auto& from = cachedAuthored ? authored : plain;
        auto to = cachedAuthored ? plain : authored;
        const auto before = to[0].ExtraLODColor0;
        ASSERT_TRUE(WriteLodCache(path, key, ComputeGeneratedLodHash(from), from));
        EXPECT_EQ(ReadLodCacheInto(path, nullptr, to), LodCacheStatus::StructureMismatch);
        EXPECT_EQ(to[0].ExtraLODColor0, before);
    }
}

TEST_F(AuthoredColourCache, GeneratedReplacementClearsPerLevelColours) {
    Mesh generated = Level("generated", 7, 0, false);
    generated.ExtraLODs = {{0, 1, 2}};
    Vector<Mesh> saved{generated};
    const LodCacheKey key{1, 2};
    const auto path = folder / "g.gelod";
    ASSERT_TRUE(WriteLodCache(path, key, ComputeGeneratedLodHash(saved), saved));
    generated.ExtraLODColor0 = {{0.25f}}; // stale scratch data is retired on replacement
    Vector<Mesh> dest{generated};
    ASSERT_EQ(ReadLodCacheInto(path, &key, dest), LodCacheStatus::Hit);
    EXPECT_TRUE(dest[0].ExtraLODColor0.empty());
    GenerateMeshLODsInto(generated);
    EXPECT_TRUE(generated.ExtraLODColor0.empty());
    generated.Skinned = true;
    generated.Joints0.assign(generated.Vertices.size() * 4u, 0u);
    generated.Weights0.assign(generated.Vertices.size() * 4u, 0.25f);
    ASSERT_TRUE(generated.IsSkinned());
    generated.ExtraLODColor0 = {{0.25f}};
    ModelAsset model(GUID::Generate(), "synthetic://skip.glb");
    model.SetMeshesForTest({generated});
    model.GenerateLODs({}, false);
    EXPECT_TRUE(model.GetMesh(0).ExtraLODColor0.empty());
}

TEST(AuthoredLODColours, HlodBakesChosenOwnLevelRgbAndAlphaAndSharedLevelBase) {
    auto mesh = ColouredChain();
    mesh.ExtraLODs.insert(mesh.ExtraLODs.begin(), {1, 0, 2});
    mesh.ExtraLODVertices.insert(mesh.ExtraLODVertices.begin(), Vector<Vertex>{});
    mesh.ExtraLODColor0.insert(mesh.ExtraLODColor0.begin(), Vector<float>{});
    for (uint32 level = 0; level < 4; ++level) {
        SCOPED_TRACE(level);
        Hlod::MergeMember member{};
        member.Source = &mesh; member.ChosenLod = level;
        member.MaterialGuid = GUID::Derive(GUID::Null(), "leaves");
        member.Transform[0] = member.Transform[5] = member.Transform[10] = member.Transform[15] = 1.0f;
        auto result = Hlod::MergeClusterMembers(std::span(&member, 1));
        ASSERT_EQ(result.Submeshes.size(), 1u);
        EXPECT_EQ(result.Submeshes[0].Color0, level < 2 ? mesh.Color0 : mesh.ExtraLODColor0[level - 1]);
        EXPECT_EQ(result.Submeshes[0].Indices, mesh.LODIndices(level));
    }
    mesh.ExtraLODColor0[2].pop_back();
    Hlod::MergeMember bad{}; bad.Source = &mesh; bad.ChosenLod = 3;
    EXPECT_TRUE(Hlod::MergeClusterMembers(std::span(&bad, 1)).Submeshes.empty());
}

TEST(AuthoredLODColours, InferredMaskDemotionIncludesEveryAuthoredAlphaStream) {
    auto mesh = ColouredChain(); mesh.MaterialIndex = 0;
    // One opaque BGRA texel encoded as an uncompressed 32-bit TGA. Texture
    // coverage alone is opaque; only the selected vertex alpha can discard.
    EmbeddedImage image;
    image.MimeType = "image/x-tga";
    image.Data = {0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,32,8,255,255,255,255};
    ImportedMaterialData material{};
    material.AlphaMode = AlphaMode::Mask; material.AlphaModeInferred = true;
    material.DiffuseTexture = "__embedded:0";
    std::fill_n(material.DiffuseColor, 4, 1.0f);
    for (size_t i = 3; i < mesh.Color0.size(); i += 4) mesh.Color0[i] = 1.0f;
    for (auto& colours : mesh.ExtraLODColor0)
        for (size_t i = 3; i < colours.size(); i += 4) colours[i] = 1.0f;
    ModelAsset asset(GUID::Generate(), "synthetic://alpha.fbx");
    ModelAssetFbxTestAccess::DemoteMasks(asset, {mesh}, {material}, {image});
    ASSERT_EQ(asset.GetMaterial(0).AlphaMode, AlphaMode::Opaque) << "opaque control must actually demote";
    mesh.ExtraLODColor0[1][3] = 0.1f;
    ModelAssetFbxTestAccess::DemoteMasks(asset, {mesh}, {material}, {image});
    EXPECT_EQ(asset.GetMaterial(0).AlphaMode, AlphaMode::Mask);
    mesh.ExtraLODColor0[1].pop_back();
    ModelAssetFbxTestAccess::DemoteMasks(asset, {mesh}, {material}, {image});
    EXPECT_EQ(asset.GetMaterial(0).AlphaMode, AlphaMode::Mask) << "malformed data is not evidence of opacity";
}

TEST(AuthoredLODColours, InferredMaskDemotionSkipsVertexAlphaTheMaterialIgnores) {
    // Synty-style FBX: a zero-alpha colour layer on every level over an opaque
    // texture. The material ignores vertex colour, so only the texture decides.
    auto mesh = ColouredChain(); mesh.MaterialIndex = 0;
    EmbeddedImage image;
    image.MimeType = "image/x-tga";
    image.Data = {0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,32,8,255,255,255,255};
    ImportedMaterialData material{};
    material.AlphaMode = AlphaMode::Mask; material.AlphaModeInferred = true;
    material.IgnoresVertexColor = true;
    material.DiffuseTexture = "__embedded:0";
    std::fill_n(material.DiffuseColor, 4, 1.0f);
    for (size_t i = 3; i < mesh.Color0.size(); i += 4) mesh.Color0[i] = 0.0f;
    for (auto& colours : mesh.ExtraLODColor0)
        for (size_t i = 3; i < colours.size(); i += 4) colours[i] = 0.0f;
    ModelAsset asset(GUID::Generate(), "synthetic://synty.fbx");
    ModelAssetFbxTestAccess::DemoteMasks(asset, {mesh}, {material}, {image});
    EXPECT_EQ(asset.GetMaterial(0).AlphaMode, AlphaMode::Opaque);
}

TEST_F(AuthoredColourGPU, ActualGpuBuffersAlignUnequalOwnAndSharedLevelsAtMeshTableOffsets) {
    // Occupy the same bucket first: checking only offset zero would miss a
    // pool/allocation alignment error affecting every later draw.
    registry.RegisterSubmesh({GUID::Generate(), 0}, Level("padding", 11, 0));
    auto mesh = ColouredChain();
    mesh.ExtraLODs.insert(mesh.ExtraLODs.begin(), {1, 0, 2});
    mesh.ExtraLODVertices.insert(mesh.ExtraLODVertices.begin(), Vector<Vertex>{});
    mesh.ExtraLODColor0.insert(mesh.ExtraLODColor0.begin(), Vector<float>{});
    auto handle = registry.RegisterSubmesh({GUID::Generate(), 0}, mesh);
    ASSERT_TRUE(handle.IsValid());
    const auto* entry = registry.Find(handle);
    ASSERT_NE(entry, nullptr); ASSERT_EQ(entry->lodCount, 4u);
    const auto& row = scene->GetMeshes()[entry->gpuMeshIndex];
    EXPECT_GT(row.vertexOffset, 0u);
    EXPECT_EQ(row.lodVertexOffset[0], 0u); EXPECT_EQ(row.lodVertexOffset[1], 0u);
    EXPECT_EQ(row.lodVertexOffset[2], 7u); EXPECT_EQ(row.lodVertexOffset[3], 12u);
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(registry.TryGetDrawableBindings(*entry, bindings));
    for (uint32 level = 0; level < 4; ++level) {
        SCOPED_TRACE(level);
        const auto& vertices = level < 2 ? mesh.Vertices : mesh.ExtraLODVertices[level - 1];
        const auto& colours = level < 2 ? mesh.Color0 : mesh.ExtraLODColor0[level - 1];
        const size_t start = row.vertexOffset + row.lodVertexOffset[level];
        const auto gpuColors = Read<float>(bindings.colorVB, start * 4 * sizeof(float), vertices.size() * 4);
        EXPECT_EQ(gpuColors, std::vector<float>(colours.begin(), colours.end()));
        // Production core stream is position3 + normal3 + UV2. Read the actual
        // indexed vertices and RGBA selected by the published GPU row.
        const auto core = Read<float>(bindings.coreVB, start * 8 * sizeof(float), vertices.size() * 8);
        const size_t count = row.lodIndexCount[level];
        const size_t indexStart = row.lodIndexOffset[level];
        const size_t prefix = indexStart & 1u;
        const auto packed = Read<uint16_t>(bindings.indexBuffer, (indexStart - prefix) * sizeof(uint16_t),
                                          (count + prefix + 1u) & ~size_t(1));
        ASSERT_EQ(entry->indexType, static_cast<uint32_t>(IndexType::Uint16));
        for (size_t i = 0; i < count; ++i) {
            const auto local = packed[i + prefix]; ASSERT_LT(local, vertices.size());
            EXPECT_EQ(local, mesh.LODIndices(level)[i]);
            EXPECT_FLOAT_EQ(core[local * 8], vertices[local].Position[0]);
            EXPECT_FLOAT_EQ(core[local * 8 + 6], vertices[local].TexCoords[0]);
            for (size_t c = 0; c < 4; ++c) EXPECT_FLOAT_EQ(gpuColors[local * 4 + c], colours[local * 4 + c]);
        }
    }
}

TEST_F(AuthoredColourGPU, ColourOnlyReloadChangesContentAndUploadedBytesWithoutChangingIdentity) {
    auto mesh = ColouredChain();
    const auto guid = GUID::Generate();
    ModelAsset asset(guid, "synthetic://colours.glb");
    asset.SetMeshesForTest({mesh});
    auto handles = registry.RegisterModelMeshes(guid, asset);
    ASSERT_EQ(handles.size(), 1u);
    const auto oldHash = registry.Find(handles[0])->contentHash;
    const auto oldRow = registry.Find(handles[0])->gpuMeshIndex;
    mesh.ExtraLODColor0[1][3] = 0.875f;
    asset.SetMeshesForTest({mesh});
    auto report = registry.ReloadModelMeshes(guid, asset);
    EXPECT_EQ(report.SubmeshesReuploaded, 1u); EXPECT_EQ(report.SubmeshesUnchanged, 0u);
    const auto* entry = registry.Find(handles[0]); ASSERT_NE(entry, nullptr);
    EXPECT_NE(entry->contentHash, oldHash); EXPECT_EQ(entry->gpuMeshIndex, oldRow);
    MeshGPUEntryBindings binding{}; ASSERT_TRUE(registry.TryGetDrawableBindings(*entry, binding));
    const auto rgba = Read<float>(binding.colorVB, (entry->vertexOffset + entry->lodVertexOffset[2]) * 16u, 4);
    EXPECT_FLOAT_EQ(rgba[3], 0.875f);
    report = registry.ReloadModelMeshes(guid, asset);
    EXPECT_EQ(report.SubmeshesUnchanged, 1u);
}

TEST_F(AuthoredColourGPU, ForcedIndexedLodsFetchPositionUvAndRgbaThroughRealVertexBindings) {
    auto load = [](const char* filename) {
        std::ifstream stream(TestPaths::StagedRoot() / "Shaders" / filename, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
    };
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = load("authored_lod_color_probe.vert.spv");
    pipelineDesc.pixelShader = load("authored_lod_color_probe.frag.spv");
    ASSERT_FALSE(pipelineDesc.vertexShader.empty()); ASSERT_FALSE(pipelineDesc.pixelShader.empty());
    pipelineDesc.topology = PrimitiveTopology::PointList;
    pipelineDesc.AddDynamicState(DynamicState::Viewport); pipelineDesc.AddDynamicState(DynamicState::Scissor);
    pipelineDesc.EnableDepthTest(false);
    pipelineDesc.SetCullingMode(CullModeFlagBits::None);
    pipelineDesc.pushConstantSize = sizeof(uint32_t);
    pipelineDesc.pushConstantStagesMask = 1u; // vertex
    pipelineDesc.colorAttachmentFormats.assign(2, uint32_t(TextureFormat::R32G32B32A32_FLOAT));
    pipelineDesc.vertexBindings = {{0, 8u * sizeof(float), 0}, {1, 4u * sizeof(float), 0}};
    pipelineDesc.vertexAttributes = {{0, 0, Format::R32G32B32_FLOAT, 0},
        {2, 0, Format::R32G32_FLOAT, 6u * sizeof(float)}, {4, 1, Format::R32G32B32A32_FLOAT, 0}};
    const auto pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());
    registry.RegisterSubmesh({GUID::Generate(), 0}, Level("padding", 11, 0));
    auto mesh = ColouredChain();
    mesh.ExtraLODs.insert(mesh.ExtraLODs.begin(), {1, 0, 2});
    mesh.ExtraLODVertices.insert(mesh.ExtraLODVertices.begin(), Vector<Vertex>{});
    mesh.ExtraLODColor0.insert(mesh.ExtraLODColor0.begin(), Vector<float>{});
    auto handle = registry.RegisterSubmesh({GUID::Generate(), 0}, mesh);
    const auto* entry = registry.Find(handle); ASSERT_NE(entry, nullptr);
    ASSERT_EQ(entry->lodCount, 4u);
    const auto& row = scene->GetMeshes()[entry->gpuMeshIndex];
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(registry.TryGetDrawableBindings(*entry, bindings));
    for (uint32_t lod = 0; lod < 4; ++lod) {
        SCOPED_TRACE(lod);
        TextureDesc targetDesc{};
        targetDesc.width = 16; targetDesc.height = 1;
        targetDesc.format = uint32_t(TextureFormat::R32G32B32A32_FLOAT);
        targetDesc.usage = uint32_t(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        const auto color = device->CreateTexture(targetDesc), position = device->CreateTexture(targetDesc);
        ASSERT_TRUE(color.IsValid()); ASSERT_TRUE(position.IsValid());
        const auto colorRead = device->CreateReadbackBuffer(16 * 4 * sizeof(float));
        const auto positionRead = device->CreateReadbackBuffer(16 * 4 * sizeof(float));
        auto cmd = device->CreateCommandList(IDevice::QueueType::Graphics);
        cmd->Begin();
        for (auto texture : {color, position})
            cmd->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc pass{};
        pass.colorTargetCount = 2; pass.colorTargets[0] = color; pass.colorTargets[1] = position;
        for (int i = 0; i < 2; ++i) {
            pass.clearColor[i] = true; pass.colorStoreOp[i] = RenderPassDesc::StoreOp::Store;
            std::fill_n(pass.clearColorValue[i], 4, -1.0f);
        }
        cmd->BeginRenderPass(pass);
        cmd->SetViewport(0, 0, 16, 1); cmd->SetScissor(0, 0, 16, 1);
        cmd->SetPipeline(pipeline);
        cmd->SetVertexBuffer(bindings.coreVB, 0); cmd->SetVertexBuffer(bindings.colorVB, 1);
        cmd->SetIndexBuffer(bindings.indexBuffer, IndexType::Uint16);
        const uint32_t base = row.vertexOffset + row.lodVertexOffset[lod];
        cmd->SetPushConstants(base);
        cmd->DrawIndexed(row.lodIndexCount[lod], 1, row.lodIndexOffset[lod], static_cast<int32_t>(base));
        cmd->EndRenderPass();
        for (auto texture : {color, position})
            cmd->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::RenderTarget, ResourceState::CopySource));
        cmd->CopyTextureToBuffer(color, colorRead, 16, 1);
        cmd->CopyTextureToBuffer(position, positionRead, 16, 1);
        cmd->End(); std::vector<CommandList*> commands{cmd.get()};
        device->ExecuteCommandLists(commands); device->WaitForIdle();
        const auto* gotColor = static_cast<const float*>(device->MapBuffer(colorRead));
        const auto* gotPosition = static_cast<const float*>(device->MapBuffer(positionRead));
        ASSERT_NE(gotColor, nullptr); ASSERT_NE(gotPosition, nullptr);
        const auto& vertices = lod < 2 ? mesh.Vertices : mesh.ExtraLODVertices[lod - 1];
        const auto& colours = lod < 2 ? mesh.Color0 : mesh.ExtraLODColor0[lod - 1];
        for (uint32_t i = 0; i < 16; ++i) {
            const auto& indices = mesh.LODIndices(lod);
            if (std::find(indices.begin(), indices.end(), i) == indices.end()) {
                EXPECT_FLOAT_EQ(gotColor[i * 4], -1.0f); continue;
            }
            for (uint32_t c = 0; c < 4; ++c) EXPECT_FLOAT_EQ(gotColor[i * 4 + c], colours[i * 4 + c]);
            for (uint32_t c = 0; c < 3; ++c) EXPECT_FLOAT_EQ(gotPosition[i * 4 + c], vertices[i].Position[c]);
            EXPECT_FLOAT_EQ(gotPosition[i * 4 + 3], vertices[i].TexCoords[0]);
        }
        device->UnmapBuffer(colorRead); device->UnmapBuffer(positionRead);
        device->DestroyBuffer(colorRead); device->DestroyBuffer(positionRead);
        device->DestroyTexture(color); device->DestroyTexture(position);
    }
    device->DestroyPipeline(pipeline);
}

TEST_F(AuthoredColourGPU, InvalidRawRgbaOrMorphChainFallsBackWithoutExtraThresholds) {
    for (uint32 bad = 0; bad < 7; ++bad) {
        SCOPED_TRACE(bad);
        auto mesh = ColouredChain(); mesh.ExtraLODCoverage = {0.8f, 0.4f};
        switch (bad) {
        case 0: mesh.Color0.pop_back(); break;
        case 1: mesh.Color0[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 2: mesh.ExtraLODColor0[0].pop_back(); break;
        case 3: mesh.ExtraLODColor0[1][3] = std::numeric_limits<float>::infinity(); break;
        case 4: mesh.ExtraLODColor0.clear(); break;
        case 5: mesh.Color0.clear(); break;
        case 6: mesh.MorphTargets.resize(1); break;
        }
        const auto handle = registry.RegisterSubmesh({GUID::Generate(), 0}, mesh);
        ASSERT_TRUE(handle.IsValid());
        const auto* entry = registry.Find(handle); ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->lodCount, 1u); EXPECT_FALSE(entry->lodAuthored);
        EXPECT_EQ(entry->lodCoverageCount, 0u);
        MeshGPUEntryBindings bindings{};
        ASSERT_TRUE(registry.TryGetDrawableBindings(*entry, bindings));
        const bool validBase = bad != 0u && bad != 1u && bad != 5u;
        EXPECT_EQ(HasFlag(entry->vertexFlags, VertexAttributeFlags::HasColor), validBase);
        EXPECT_EQ(bindings.colorVB.IsValid(), validBase);
        const auto& row = scene->GetMeshes()[entry->gpuMeshIndex];
        for (uint32 level = 1; level < 4; ++level) {
            EXPECT_EQ(row.lodIndexCount[level], 0u);
            EXPECT_EQ(row.lodVertexOffset[level], 0u);
        }
    }
}

TEST(AuthoredLODColours, OwnVertexStreamSupportIsTheSingleRuleUploadAndTheInspectorShare) {
    const auto coloured = ColouredChain();
    EXPECT_TRUE(coloured.OwnVertexLODStreamsSupported());
    // The shared resolver is what both the GPU upload and the editor's provenance
    // badge read, so a coloured chain must survive it with all levels admitted.
    EXPECT_EQ(ResolveMeshLODGeometry(coloured).LevelCount, coloured.LODCount());
    EXPECT_EQ(ResolveMeshLODGeometry(coloured).Issue, MeshLODGeometryIssue::None);

    Mesh colourless = Assemble(0, Family())[1];
    ASSERT_TRUE(colourless.Color0.empty());
    ASSERT_TRUE(colourless.HasOwnVertexLODs());
    EXPECT_TRUE(colourless.OwnVertexLODStreamsSupported());

    for (uint32 unsupported = 0; unsupported < 6u; ++unsupported) {
        SCOPED_TRACE(unsupported);
        Mesh mesh = coloured;
        switch (unsupported) {
        case 0: mesh.ExtraLODColor0[0].pop_back(); break;
        case 1: mesh.ExtraLODColor0[1][0] = std::numeric_limits<float>::infinity(); break;
        case 2: mesh.TexCoords1.assign(mesh.Vertices.size() * 2u, 0.5f); break;
        case 3:
            mesh.Skinned = true;
            mesh.Joints0.assign(mesh.Vertices.size() * 4u, 0u);
            mesh.Weights0.assign(mesh.Vertices.size() * 4u, 0.25f);
            break;
        case 4: mesh.MorphTargets.resize(1); break;
        case 5: mesh.ExtraTexCoords = {Vector<float>(mesh.Vertices.size() * 2u, 0.25f)}; break;
        }
        EXPECT_FALSE(mesh.OwnVertexLODStreamsSupported());
        EXPECT_EQ(ResolveMeshLODGeometry(mesh).LevelCount, 1u);
        EXPECT_EQ(ResolveMeshLODGeometry(mesh).Issue, MeshLODGeometryIssue::UnsupportedStreams);
    }
}

// An FBX LOD group stores its levels as node children, and lod_levels only
// promises to run parallel to that child order. Real exports do write them out
// of order — the Farmlands trees store LOD1, LOD2, LOD0 — so taking the stored
// order as the level order renders a tree that gets denser as it recedes.
TEST(AuthoredLODOrder, LevelOrderComesFromGroupDataNotChildOrder) {
    // Child order LOD1, LOD2, LOD0, as the Farmlands trees store it.
    const Vector<uint32> storedOrder{0u, 1u, 2u};

    Vector<FbxLodGroupChild> byDistance(3);
    byDistance[0].SwitchDistance = 25.0f;  byDistance[0].Name = "Tree_LOD1";
    byDistance[1].SwitchDistance = 60.0f;  byDistance[1].Name = "Tree_LOD2";
    byDistance[2].SwitchDistance = 0.0f;   byDistance[2].Name = "Tree_LOD0";
    // World units: `distance` is the minimum distance at which a level shows, so
    // it rises with coarseness. Here the names agree with it, so the distances
    // stand and may seed switch coverages.
    auto world = ResolveFbxLodGroupOrder(byDistance, false);
    EXPECT_EQ(world.Levels, (Vector<uint32>{2u, 0u, 1u}));
    EXPECT_NE(world.Levels, storedOrder);
    EXPECT_TRUE(world.FromSwitchDistances);

    // Screen percentage inverts the comparison, not the meaning. Asserted on
    // children with no level names, so only the distances can decide it.
    Vector<FbxLodGroupChild> unnamed = byDistance;
    for (auto& child : unnamed) child.Name = "Tree";
    EXPECT_EQ(ResolveFbxLodGroupOrder(unnamed, false).Levels, (Vector<uint32>{2u, 0u, 1u}));
    EXPECT_EQ(ResolveFbxLodGroupOrder(unnamed, true).Levels, (Vector<uint32>{1u, 0u, 2u}));
    EXPECT_TRUE(ResolveFbxLodGroupOrder(unnamed, true).FromSwitchDistances);

    // Read as percentages the same distances contradict the names, and the names
    // win — see LevelNamesOutrankSwitchDistancesThatContradictThem.
    const auto contradicted = ResolveFbxLodGroupOrder(byDistance, true);
    EXPECT_EQ(contradicted.Levels, (Vector<uint32>{2u, 0u, 1u}));
    EXPECT_FALSE(contradicted.FromSwitchDistances);
    EXPECT_EQ(contradicted.OverriddenDistanceOrder, (Vector<uint32>{1u, 0u, 2u}));

    // Names decide when the thresholds cannot: non-finite, or all equal. The
    // distances are then not the artist's, so the caller must not consume them.
    Vector<FbxLodGroupChild> byName = byDistance;
    byName[0].SwitchDistance = std::numeric_limits<float>::quiet_NaN();
    auto named = ResolveFbxLodGroupOrder(byName, false);
    EXPECT_EQ(named.Levels, (Vector<uint32>{2u, 0u, 1u}));
    EXPECT_FALSE(named.FromSwitchDistances);
    for (auto& child : byName) child.SwitchDistance = 0.0f;
    EXPECT_EQ(ResolveFbxLodGroupOrder(byName, false).Levels, (Vector<uint32>{2u, 0u, 1u}));
    EXPECT_FALSE(ResolveFbxLodGroupOrder(byName, true).FromSwitchDistances);

    // Neither usable: the caller is told rather than handed a guess.
    Vector<FbxLodGroupChild> neither = byName;
    for (auto& child : neither) child.Name = "Tree";
    EXPECT_TRUE(ResolveFbxLodGroupOrder(neither, false).Levels.empty());
    Vector<FbxLodGroupChild> duplicateLevels = byName;
    duplicateLevels[2].Name = "Tree_LOD1";
    EXPECT_TRUE(ResolveFbxLodGroupOrder(duplicateLevels, false).Levels.empty());

    // A group already stored finest-first is left exactly as it is.
    Vector<FbxLodGroupChild> inOrder(3);
    for (uint32 i = 0; i < 3u; ++i) {
        inOrder[i].SwitchDistance = static_cast<float>(i) * 10.0f;
        inOrder[i].Name = "Tree_LOD" + std::to_string(i);
    }
    EXPECT_EQ(ResolveFbxLodGroupOrder(inOrder, false).Levels, storedOrder);
}

// ufbx synthesises lod_levels[0] (0 in world units, 100 as a screen percentage)
// and fills the rest positionally. A two-level group whose own thresholds are
// non-finite therefore offers a usable-looking 100 at child 0 that belongs to no
// level the artist wrote. Consuming it as LOD1's switch coverage of 1.0 would
// retire LOD0 at every coverage below full screen, so the names branch must
// leave the distances alone and let the default table apply.
TEST(AuthoredLODOrder, NameOrderedGroupDoesNotInheritSyntheticSwitchDistances) {
    Vector<FbxLodGroupChild> children(2);
    children[0].SwitchDistance = 100.0f; // ufbx's synthetic relative level-0 entry
    children[0].Name = "Sibling_LOD1";
    children[1].SwitchDistance = std::numeric_limits<float>::infinity(); // Thresholds|Level0
    children[1].Name = "Sibling_LOD0";

    const auto resolved = ResolveFbxLodGroupOrder(children, true);
    EXPECT_EQ(resolved.Levels, (Vector<uint32>{1u, 0u}));
    EXPECT_FALSE(resolved.FromSwitchDistances)
        << "a non-finite threshold set is not a threshold set; its neighbour is not a switch point";

    // And the assembler must survive a chain that carries no distances at all,
    // falling back to the default table rather than reading past the vector.
    Vector<Mesh> meshes{Level("Sibling", 6, 0, false), Level("Sibling_lower", 4, 1, false)};
    meshes[0].SourceNodeIndex = 0; meshes[1].SourceNodeIndex = 1;
    FbxLodGroup group; group.Lod0Node = 0; group.LowerNodes = {1};
    group.RelativeDistances = true; // SwitchDistances deliberately left empty
    AssembleFbxLodGroupChains(meshes, {group}, "synthetic distances");
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].LODCount(), 2u);
    EXPECT_TRUE(meshes[0].ExtraLODCoverage.empty())
        << "no authored coverage means the default threshold table, not a fabricated one";
}

// A multi-material LOD-group node names its submeshes "<node>_<part>", so when
// the node carries the level tag the tag sits in the middle. Deriving the level
// order changes WHICH child is the base, which changes that tag — and scenes
// bind these renderers by name. The game's Lakeside scenes address these trees
// as "..._LOD1_<part>" because the file stored LOD1 first; the same submesh now
// imports as "..._LOD0_<part>" and must still resolve, or every leaves entity
// silently falls back to submesh 0 and draws bark.
TEST(AuthoredLODOrder, RenamedBaseSubmeshStillResolvesForNameBoundRenderers) {
    using namespace GameEngine::Components;
    const std::string_view authored = "SM_Farm_GenTree_A_FullTree_01_LOD1_1"; // as shipped scenes store it
    const std::string_view imported = "SM_Farm_GenTree_A_FullTree_01_LOD0_1"; // as the fixed import exposes it
    EXPECT_EQ(HashMeshName(authored), HashMeshName(imported));

    // The part index still separates bark from leaves.
    EXPECT_NE(HashMeshName(imported), HashMeshName("SM_Farm_GenTree_A_FullTree_01_LOD0_0"));
    EXPECT_NE(HashMeshName(authored), HashMeshName("SM_Farm_GenTree_A_FullTree_01_LOD1_0"));

    // A group already stored in order keeps binding (the Sapling case).
    EXPECT_EQ(HashMeshName("SM_Farm_GenTree_Sapling_01_LOD0_1"),
              HashMeshName("SM_Farm_GenTree_Sapling_01_LOD0_1"));

    // A TRAILING _LOD<N> is still not folded: these are distinct siblings.
    EXPECT_NE(HashMeshName("Torso"), HashMeshName("Torso_LOD1"));
    // A plain part suffix with no level tag is untouched.
    EXPECT_NE(HashMeshName("Tree_0"), HashMeshName("Tree_1"));
    EXPECT_EQ(HashMeshName("Tree_0"), HashMeshName("tree_0")); // case still folds
}

// Two of the shipped Farmlands groups (A_Young, B_Young) carry finite percentage
// thresholds 4 and 2 alongside the same inverted child order, so the distances
// are usable and agree with the file's mistake. A `_LOD<N>` on a node is what a
// person typed; the positional thresholds are what the exporter emitted around
// whatever order it chose. Names therefore win a disagreement, and the distances
// that lost must not seed switch coverages.
TEST(AuthoredLODOrder, LevelNamesOutrankSwitchDistancesThatContradictThem) {
    Vector<FbxLodGroupChild> young(3);
    young[0].SwitchDistance = 100.0f; young[0].Name = "Young_LOD1"; // ufbx's synthetic level-0 entry
    young[1].SwitchDistance = 4.0f;   young[1].Name = "Young_LOD2";
    young[2].SwitchDistance = 2.0f;   young[2].Name = "Young_LOD0";

    const auto resolved = ResolveFbxLodGroupOrder(young, true);
    EXPECT_EQ(resolved.Levels, (Vector<uint32>{2u, 0u, 1u}));
    EXPECT_FALSE(resolved.FromSwitchDistances);
    EXPECT_EQ(resolved.OverriddenDistanceOrder, (Vector<uint32>{0u, 1u, 2u}))
        << "the caller reports both orders, so the file's disagreement is visible";

    // Agreement leaves the distances in charge: they are the artist's switch points.
    Vector<FbxLodGroupChild> agreeing(3);
    agreeing[0].SwitchDistance = 100.0f; agreeing[0].Name = "Tree_LOD0";
    agreeing[1].SwitchDistance = 4.0f;   agreeing[1].Name = "Tree_LOD1";
    agreeing[2].SwitchDistance = 2.0f;   agreeing[2].Name = "Tree_LOD2";
    const auto kept = ResolveFbxLodGroupOrder(agreeing, true);
    EXPECT_EQ(kept.Levels, (Vector<uint32>{0u, 1u, 2u}));
    EXPECT_TRUE(kept.FromSwitchDistances);
    EXPECT_TRUE(kept.OverriddenDistanceOrder.empty());

    // Unnumbered names leave the distances in charge even when they invert.
    Vector<FbxLodGroupChild> unnamed = young;
    for (auto& child : unnamed) child.Name = "Young";
    const auto distancesOnly = ResolveFbxLodGroupOrder(unnamed, true);
    EXPECT_EQ(distancesOnly.Levels, (Vector<uint32>{0u, 1u, 2u}));
    EXPECT_TRUE(distancesOnly.FromSwitchDistances);
}
