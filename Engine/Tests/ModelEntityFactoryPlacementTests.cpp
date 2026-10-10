// Mesh placements in ModelEntityFactory: a mesh that several nodes draw spawns
// one entity per node, at that node's transform, all on the one GPU mesh; a
// placement whose node is an imported scene entity sits below that entity with
// its local transform. Device-gated: the factory registers GPU meshes through
// RenderServices.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"

#include "TestDeviceHelper.h"

#include <string>
#include <unordered_map>

namespace
{

// One triangle drawn by node 0 and node 1, each at the given x in model space
// (SourceNodeTransform) and relative to its node (SourceNodeLocalTransform).
GameEngine::Mesh SharedTriangle(float firstWorldX, float firstLocalX, float secondWorldX, float secondLocalX)
{
    GameEngine::Mesh mesh;
    mesh.Name = "Triangle";
    mesh.Vertices.resize(3);
    mesh.Indices = {0u, 1u, 2u};
    mesh.SourceNodeIndex = 0;
    mesh.SourceNodeTransform[12] = firstWorldX;
    mesh.SourceNodeLocalTransform[12] = firstLocalX;
    GameEngine::MeshPlacement second;
    second.SourceNodeIndex = 1;
    second.SourceNodeTransform[12] = secondWorldX;
    second.SourceNodeLocalTransform[12] = secondLocalX;
    mesh.ExtraPlacements.push_back(second);
    return mesh;
}

GameEngine::Vector<GameEngine::Mesh> OneMesh(GameEngine::Mesh mesh)
{
    GameEngine::Vector<GameEngine::Mesh> meshes;
    meshes.push_back(std::move(mesh));
    return meshes;
}

} // namespace

// Two nodes draw one triangle: the model spawns two entities under its root, at
// x = 1 and x = 4, rendering the same GPU mesh.
TEST(ModelEntityFactoryPlacements, EveryPlacementSpawnsAnEntityAtItsTransform)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "shared.gltf");
    model.SetMeshesForTest(OneMesh(SharedTriangle(1.0f, 0.0f, 4.0f, 0.0f)));

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Shared");
    ASSERT_TRUE(result.rootEntity.IsValid());
    ASSERT_EQ(result.submeshEntities.size(), 2u);

    const float expectedX[2] = {1.0f, 4.0f};
    for (size_t index = 0; index < 2; ++index)
    {
        const auto entity = result.submeshEntities[index];
        EXPECT_NE(entity, result.rootEntity);
        const auto* parent = world.GetComponent<GameEngine::Components::Parent>(entity);
        ASSERT_NE(parent, nullptr);
        EXPECT_EQ(parent->parent, result.rootEntity);
        const auto* xf = world.GetComponent<GameEngine::Components::Transform>(entity);
        ASSERT_NE(xf, nullptr);
        EXPECT_FLOAT_EQ(xf->matrix[12], expectedX[index]);
    }
    const auto* first = world.GetComponent<GameEngine::Components::MeshRenderer>(result.submeshEntities[0]);
    const auto* second = world.GetComponent<GameEngine::Components::MeshRenderer>(result.submeshEntities[1]);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->meshGpuHandleId, second->meshGpuHandleId);

    device->Shutdown();
}

// The placements' nodes are imported scene entities (an FBX's helper nodes): each
// mesh entity sits below its node's entity with the node-relative transform.
TEST(ModelEntityFactoryPlacements, APlacementBelowAnImportedNodeTakesItsLocalTransform)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "shared.fbx");
    model.SetMeshesForTest(OneMesh(SharedTriangle(1.0f, 0.25f, 4.0f, 0.75f)));
    model.SetSceneNodesForTest([] {
        GameEngine::Vector<GameEngine::ImportedSceneNodeData> nodes;
        for (const int32_t index : {0, 1})
        {
            GameEngine::ImportedSceneNodeData node;
            node.Name = "Node" + std::to_string(index);
            node.SourceNodeIndex = index;
            nodes.push_back(node);
        }
        return nodes;
    }());

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Shared");
    ASSERT_TRUE(result.rootEntity.IsValid());
    ASSERT_EQ(result.submeshEntities.size(), 2u);

    std::unordered_map<std::string, GameEngine::ECS::EntityHandle> byName;
    world.Query<GameEngine::ECS::Read<GameEngine::Components::Name>>().Each(
        [&](GameEngine::ECS::EntityHandle entity, const GameEngine::Components::Name& name) {
            byName.emplace(std::string(name.View()), entity);
        });
    ASSERT_TRUE(byName.count("Node0"));
    ASSERT_TRUE(byName.count("Node1"));

    const GameEngine::ECS::EntityHandle expectedParent[2] = {byName.at("Node0"), byName.at("Node1")};
    const float expectedLocalX[2] = {0.25f, 0.75f};
    for (size_t index = 0; index < 2; ++index)
    {
        const auto entity = result.submeshEntities[index];
        const auto* parent = world.GetComponent<GameEngine::Components::Parent>(entity);
        ASSERT_NE(parent, nullptr);
        EXPECT_EQ(parent->parent, expectedParent[index]);
        const auto* xf = world.GetComponent<GameEngine::Components::Transform>(entity);
        ASSERT_NE(xf, nullptr);
        EXPECT_FLOAT_EQ(xf->matrix[12], expectedLocalX[index]);
    }

    device->Shutdown();
}
