// Spawn topology in ModelEntityFactory. Helper nodes: an imported scene node
// must attach to its actual imported parent regardless of the order the
// importer listed the nodes in. A parent index that names no imported node is
// a skeleton bone (bones never appear in importedNodes) and roots the child
// with its world transform; a parent that is merely listed later must defer
// the child to a following pass, then attach it with its LOCAL transform.
//
// Headless: a zero-mesh model registers no GPU resources, so an uninitialized
// RenderServices suffices (the AnimationClockPersistenceTests precedent).
//
// The model root: its Transform starts at identity, so a mesh its node turns
// sits below it. Device-gated: the factory registers GPU meshes through
// RenderServices.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"

#include "TestDeviceHelper.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace
{

constexpr uint32_t kTestSkeletonId = 42;
constexpr float kChildLocalX = 1.5f;
constexpr float kChildWorldX = 7.5f;

GameEngine::ImportedSceneNodeData MakeNode(const char* name,
                                           int32_t sourceIndex,
                                           int32_t parentIndex,
                                           float worldX,
                                           float localX)
{
    GameEngine::ImportedSceneNodeData node;
    node.Name = name;
    node.SourceNodeIndex = sourceIndex;
    node.ParentSourceNodeIndex = parentIndex;
    node.Transform[12] = worldX;      // column-major: [12] = translation.x
    node.LocalTransform[12] = localX;
    return node;
}

std::unordered_map<std::string, GameEngine::ECS::EntityHandle> CollectByName(GameEngine::ECS::World& world)
{
    std::unordered_map<std::string, GameEngine::ECS::EntityHandle> byName;
    world.Query<GameEngine::ECS::Read<GameEngine::Components::Name>>().Each(
        [&](GameEngine::ECS::EntityHandle entity, const GameEngine::Components::Name& name) {
            byName.emplace(std::string(name.View()), entity);
        });
    return byName;
}

} // namespace

// A child listed before its imported parent must wait for the parent, not be
// misread as bone-parented and re-rooted with its world transform. Node order
// in importedNodes is importer-dependent, so both orders must produce the
// same hierarchy.
TEST(ModelEntityFactoryHelperNodes, ChildListedBeforeParentAttachesToParentWithLocalTransform)
{
    GameEngine::Engine::Renderer::RenderServices rs;
    GameEngine::ECS::World world;

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "helpers.fbx");
    model.SetSkeletonIdForTest(kTestSkeletonId);
    model.SetSceneNodesForTest([] {
        GameEngine::Vector<GameEngine::ImportedSceneNodeData> nodes;
        nodes.push_back(MakeNode("Child", 5, 3, kChildWorldX, kChildLocalX));
        nodes.push_back(MakeNode("ParentNode", 3, -1, 2.5f, 2.5f));
        return nodes;
    }());

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Rig");
    ASSERT_TRUE(result.rootEntity.IsValid());

    const auto byName = CollectByName(world);
    ASSERT_TRUE(byName.count("Child"));
    ASSERT_TRUE(byName.count("ParentNode"));

    const auto* parentRef = world.GetComponent<GameEngine::Components::Parent>(byName.at("Child"));
    ASSERT_NE(parentRef, nullptr);
    EXPECT_EQ(parentRef->parent, byName.at("ParentNode"))
        << "child listed before its parent was re-rooted instead of deferred";

    const auto* xf = world.GetComponent<GameEngine::Components::Transform>(byName.at("Child"));
    ASSERT_NE(xf, nullptr);
    EXPECT_FLOAT_EQ(xf->matrix[12], kChildLocalX)
        << "a helper-node-parented child must carry its local transform, not its world transform";

    const auto* parentParentRef = world.GetComponent<GameEngine::Components::Parent>(byName.at("ParentNode"));
    ASSERT_NE(parentParentRef, nullptr);
    EXPECT_EQ(parentParentRef->parent, result.rootEntity);
}

// A parent index absent from importedNodes IS a bone: the node roots under the
// container with its world transform and gets the animation-follow components.
TEST(ModelEntityFactoryHelperNodes, BoneParentedNodeRootsWithWorldTransform)
{
    GameEngine::Engine::Renderer::RenderServices rs;
    GameEngine::ECS::World world;

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "helpers.fbx");
    model.SetSkeletonIdForTest(kTestSkeletonId);
    model.SetSceneNodesForTest([] {
        GameEngine::Vector<GameEngine::ImportedSceneNodeData> nodes;
        nodes.push_back(MakeNode("Socket", 5, 99, kChildWorldX, kChildLocalX));
        return nodes;
    }());

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Rig");
    ASSERT_TRUE(result.rootEntity.IsValid());

    const auto byName = CollectByName(world);
    ASSERT_TRUE(byName.count("Socket"));
    const auto socket = byName.at("Socket");

    const auto* parentRef = world.GetComponent<GameEngine::Components::Parent>(socket);
    ASSERT_NE(parentRef, nullptr);
    EXPECT_EQ(parentRef->parent, result.rootEntity);

    const auto* xf = world.GetComponent<GameEngine::Components::Transform>(socket);
    ASSERT_NE(xf, nullptr);
    EXPECT_FLOAT_EQ(xf->matrix[12], kChildWorldX)
        << "a bone-parented node must carry its world transform";

    const auto* skeletonRef = world.GetComponent<GameEngine::Components::SkeletonRef>(socket);
    ASSERT_NE(skeletonRef, nullptr) << "a bone-parented node must follow the skeleton";
    EXPECT_EQ(skeletonRef->skeletonId, kTestSkeletonId);

    const auto* nodeRef = world.GetComponent<GameEngine::Components::AnimatedNodeRef>(socket);
    ASSERT_NE(nodeRef, nullptr);
    EXPECT_EQ(nodeRef->nodeIndex, 5u);
}

// One mesh whose node turns it 90 degrees about X (the Damaged Helmet's +X-up
// correction): the root is a container at identity and the mesh entity below it
// carries the node's turn, so a game turning the root turns the whole model.
TEST(ModelEntityFactoryRoot, ASoleMeshTurnedByItsNodeSitsBelowAnIdentityRoot)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    // Column-major 90 degrees about X: +Y onto +Z, +Z onto -Y.
    const float turnedAboutX[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "helmet.gltf");
    model.SetMeshesForTest([&] {
        GameEngine::Mesh mesh;
        mesh.Name = "Helmet";
        mesh.Vertices.resize(3);
        mesh.Indices = {0u, 1u, 2u};
        mesh.SourceNodeIndex = 0;
        std::memcpy(mesh.SourceNodeTransform, turnedAboutX, sizeof(turnedAboutX));
        GameEngine::Vector<GameEngine::Mesh> meshes;
        meshes.push_back(std::move(mesh));
        return meshes;
    }());

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Helmet");
    ASSERT_TRUE(result.rootEntity.IsValid());
    ASSERT_EQ(result.submeshEntities.size(), 1u);
    const auto mesh = result.submeshEntities[0];
    ASSERT_NE(mesh, result.rootEntity) << "the node's turn landed on the transform a game sets";

    const auto* rootTransform = world.GetComponent<GameEngine::Components::Transform>(result.rootEntity);
    ASSERT_NE(rootTransform, nullptr);
    const GameEngine::Components::Transform identity{};
    for (int i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(rootTransform->matrix[i], identity.matrix[i]) << "root matrix element " << i;

    const auto* parent = world.GetComponent<GameEngine::Components::Parent>(mesh);
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->parent, result.rootEntity);
    const auto* meshTransform = world.GetComponent<GameEngine::Components::Transform>(mesh);
    ASSERT_NE(meshTransform, nullptr);
    for (int i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(meshTransform->matrix[i], turnedAboutX[i]) << "mesh matrix element " << i;

    device->Shutdown();
}

// A single animated mesh beside an imported helper node: the helper node needs a
// container root, which takes the model's one Animator; the mesh entity takes none.
TEST(ModelEntityFactoryRoot, AContainerRootTakesTheOnlyAnimator)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    auto& store = GameEngine::Engine::Renderer::SkeletonStore::Instance();
    const uint32_t skeletonId = store.CreateSkeleton(1);
    ASSERT_NE(store.Get(skeletonId), nullptr);

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "rig.fbx");
    model.SetMeshesForTest([] {
        GameEngine::Mesh mesh;
        mesh.Name = "Body";
        mesh.Vertices.resize(3);
        mesh.Indices = {0u, 1u, 2u};
        mesh.Skinned = true;
        mesh.Joints0.resize(mesh.Vertices.size() * 4);
        mesh.Weights0.resize(mesh.Vertices.size() * 4);
        GameEngine::Vector<GameEngine::Mesh> meshes;
        meshes.push_back(std::move(mesh));
        return meshes;
    }());
    model.SetSkeletonIdForTest(skeletonId);
    model.SetHasAnimationsForTest(true);
    model.SetSceneNodesForTest([] {
        GameEngine::Vector<GameEngine::ImportedSceneNodeData> nodes;
        nodes.push_back(MakeNode("Socket", 0, -1, 0.0f, 0.0f));
        return nodes;
    }());

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Rig");
    ASSERT_TRUE(result.rootEntity.IsValid());
    ASSERT_EQ(result.submeshEntities.size(), 1u);
    ASSERT_NE(result.submeshEntities[0], result.rootEntity);

    EXPECT_NE(world.GetComponent<GameEngine::Components::Animator>(result.rootEntity), nullptr);
    EXPECT_EQ(world.GetComponent<GameEngine::Components::Animator>(result.submeshEntities[0]), nullptr)
        << "the mesh entity took a second Animator beside the root's";

    device->Shutdown();
}

// One rigid mesh on an animated node at an identity rest pose: the clip drives the
// mesh entity's Transform through its AnimatedNodeRef, so that entity sits below a
// container root that the clip never writes.
TEST(ModelEntityFactoryRoot, ASoleMeshOnAnAnimatedNodeSitsBelowTheRoot)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    auto& store = GameEngine::Engine::Renderer::SkeletonStore::Instance();
    const uint32_t skeletonId = store.CreateSkeleton(1);
    ASSERT_NE(store.Get(skeletonId), nullptr);

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "spinner.gltf");
    model.SetMeshesForTest([] {
        GameEngine::Mesh mesh;
        mesh.Name = "Blade";
        mesh.Vertices.resize(3);
        mesh.Indices = {0u, 1u, 2u};
        mesh.SourceNodeIndex = 0;
        GameEngine::Vector<GameEngine::Mesh> meshes;
        meshes.push_back(std::move(mesh));
        return meshes;
    }());
    model.SetSkeletonIdForTest(skeletonId);

    const auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Spinner");
    ASSERT_TRUE(result.rootEntity.IsValid());
    ASSERT_EQ(result.submeshEntities.size(), 1u);
    const auto mesh = result.submeshEntities[0];
    ASSERT_NE(mesh, result.rootEntity) << "the clip would write the transform a game sets";
    EXPECT_EQ(world.GetComponent<GameEngine::Components::AnimatedNodeRef>(result.rootEntity), nullptr);
    EXPECT_NE(world.GetComponent<GameEngine::Components::AnimatedNodeRef>(mesh), nullptr);

    device->Shutdown();
}
