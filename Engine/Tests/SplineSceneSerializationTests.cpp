#include <gtest/gtest.h>

#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineWall.h"
#include "Components/Transform.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Reflection.h"
#include "EngineLogCapture.h"
#include "Logger/Logger.h"
#include "Scene/ReflectionSceneSchema.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "SplineECS/SplineService.h"
#include "SplineECS/Systems/SplineExtractionSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
using Handle = SplineECS::SplineHandle;

// A saved spline must reload as its own service spline. The Spline schema writes the control
// points and binds a fresh handle on load; the SplineComponent's handle words are runtime-only,
// so no schema may write them into a file and no file may write them into a component.
class SplineSceneSerialization : public testing::Test
{
  protected:
    bool ownsService = false;
    std::filesystem::path directory;
    std::vector<Handle> allocated;

    void SetUp() override
    {
        Scene::EnsureBuiltInSchemasRegistered();
        ASSERT_NE(ECS::ComponentFieldRegistry::FindByName("SplineComponent"), 0u);
        ownsService = !SplineECS::SplineService::IsInitialized();
        if (ownsService)
            SplineECS::SplineService::Initialize();
        directory = std::filesystem::temp_directory_path() /
            ("GameEngine_SplineScene_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
    }

    void TearDown() override
    {
        if (auto* service = SplineECS::SplineService::TryGet())
            for (const auto handle : allocated)
                if (service->IsValid(handle))
                    service->DestroySpline(handle);
        if (ownsService)
            SplineECS::SplineService::Shutdown();
        std::error_code ignored;
        if (!directory.empty())
            std::filesystem::remove_all(directory, ignored);
    }

    Handle Create(float offset = 0)
    {
        auto& service = SplineECS::SplineService::Get();
        const auto handle = service.CreateSpline(Spline::SplineType::Linear, false);
        allocated.push_back(handle);
        auto* data = service.GetSplineData(handle);
        data->AddPoint({offset - 4, 2, -8}, .25f);
        data->AddPoint({offset + 7, 5, 3}, .75f);
        data->Points[0].Rotation = {10, 20, 30};
        data->Points[0].Scale = {1, 2, 3};
        data->Points[0].Roll = .5f;
        data->Points[0].TangentIn = {-1, 2, 3};
        data->Points[0].TangentOut = {4, 5, -6};
        service.RebuildCache(handle);
        return handle;
    }

    ECS::EntityHandle Attach(ECS::World& world, Handle handle)
    {
        Components::SceneEntityTag tag{"curve"};
        Components::SplineComponent component;
        component.SplineDataIndex = handle.Index();
        component.SplineDataGeneration = handle.Generation();
        component.DefaultRadius = 2.5f;
        return world.Create(tag, Components::Transform{}, component).GetHandle();
    }

    ECS::EntityHandle Find(ECS::World& world)
    {
        ECS::EntityHandle result{};
        world.Query<ECS::Read<Components::SceneEntityTag>>().Each(
            [&](ECS::EntityHandle entity, const Components::SceneEntityTag& tag)
            { if (tag.View() == "curve") result = entity; });
        return result;
    }

    Handle Stored(ECS::World& world, ECS::EntityHandle entity)
    {
        const auto* component = world.GetComponent<Components::SplineComponent>(entity);
        if (!component)
            return {};
        const Handle handle{component->SplineDataIndex, component->SplineDataGeneration};
        allocated.push_back(handle);
        return handle;
    }

    std::string Portable(const ECS::World& world, ECS::EntityHandle entity)
    {
        std::vector<std::string> lines;
        Scene::SceneSchemaRegistry::Find("Spline")->Serialize(world, entity, {}, lines);
        std::string result;
        for (const auto& line : lines)
            result += line + '\n';
        return result;
    }

    // The block a build without the type claim wrote beside the Spline block: the reflected
    // component name with the live service handle and the two settings the Spline block also
    // carries (here deliberately different, so a test can tell which block won).
    static std::string StaleReflectedBlock(Handle handle)
    {
        return "SplineComponent.SplineDataIndex = " + std::to_string(handle.Index()) +
            "\nSplineComponent.SplineDataGeneration = " + std::to_string(handle.Generation()) +
            "\nSplineComponent.DefaultRadius = 99\nSplineComponent.Enabled = false\n";
    }

    bool Write(const std::filesystem::path& path, const std::string& body)
    {
        std::ofstream file(path);
        file << "[scene name=\"Spline serialization\" version=1]\n[entity id=\"curve\"]\n" << body;
        file.close();
        return file.good();
    }

    static std::string Read(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    bool Load(ECS::World& world, const std::filesystem::path& path)
    {
        Scene::SceneLoadDegradation degradation;
        Scene::LoadOptions options;
        options.outDegradation = &degradation;
        const bool loaded = Scene::LoadSceneFromFile(world, path, options);
        EXPECT_TRUE(loaded) << Scene::GetLastSceneIOError().message;
        EXPECT_FALSE(degradation.IsDegraded());
        SplineECS::SplineExtractionSystem{}.Update(world, 0);
        return loaded && !degradation.IsDegraded();
    }
};

TEST_F(SplineSceneSerialization, SaveIsPortableAndReloadOwnsIndependentGeometry)
{
    ECS::World original;
    const auto originalHandle = Create();
    const auto originalEntity = Attach(original, originalHandle);
    const auto expected = Portable(original, originalEntity);
    const auto path = directory / "saved.scene";
    ASSERT_TRUE(Scene::SaveSceneToFile(original, path));
    const auto saved = Read(path);
    EXPECT_EQ(saved.find("SplineComponent."), std::string::npos) << saved;
    EXPECT_NE(saved.find("Spline.point1.position"), std::string::npos);
    EXPECT_EQ(Scene::SceneSchemaRegistry::ReflectionSchemaForUnhandledType(
        ECS::ComponentFieldRegistry::FindByName("SplineComponent")), nullptr);

    std::vector<Handle> protectedHandles{originalHandle};
    for (int i = 0; i < 7; ++i)
        protectedHandles.push_back(Create(100.f + i));
    ECS::World loaded;
    ASSERT_TRUE(Load(loaded, path));
    const auto entity = Find(loaded);
    ASSERT_TRUE(entity.IsValid());
    const auto loadedHandle = Stored(loaded, entity);
    auto& service = SplineECS::SplineService::Get();
    ASSERT_TRUE(service.IsValid(loadedHandle));
    for (const auto handle : protectedHandles)
        ASSERT_FALSE(loadedHandle == handle) << "Reload acquired a live spline's service handle";
    EXPECT_EQ(Portable(loaded, entity), expected);

    std::vector<float> before;
    for (const auto handle : protectedHandles)
        before.push_back(service.GetSplineData(handle)->Points[0].Position.x);
    std::string error;
    ASSERT_TRUE(Scene::SceneSchemaRegistry::Find("Spline")->ApplyProperty(
        loaded, entity, {}, "point0.position", "(300, 4, 500)", &error)) << error;
    SplineECS::SplineExtractionSystem{}.Update(loaded, 0);
    EXPECT_FLOAT_EQ(service.GetSplineData(loadedHandle)->Points[0].Position.x, 300);
    for (size_t i = 0; i < protectedHandles.size(); ++i)
        EXPECT_FLOAT_EQ(service.GetSplineData(protectedHandles[i])->Points[0].Position.x, before[i]);
    ASSERT_TRUE(Scene::SceneSchemaRegistry::Find("Spline")->Remove(loaded, entity));
    EXPECT_FALSE(service.IsValid(loadedHandle));
    EXPECT_EQ(Portable(original, originalEntity), expected);
    for (const auto handle : protectedHandles)
        EXPECT_TRUE(service.IsValid(handle));
}

// A wall's corner is derived from its spline's type whenever the component is
// added, a scene load included; the saved value is applied over it, so a Round
// wall on a spline of straight segments, which would be derived Mitre, loads
// Round.
TEST_F(SplineSceneSerialization, AWallsSavedCornerWinsOverTheOneItsSplineWouldGive)
{
    ECS::World original;
    const auto entity = Attach(original, Create());
    Components::SplineWall wall{};
    wall.Corner = Components::SplineWallCorner::Round;
    original.AddComponentImmediate(entity, wall);
    const auto path = directory / "wall.scene";
    ASSERT_TRUE(Scene::SaveSceneToFile(original, path));

    ECS::World loaded;
    ASSERT_TRUE(Load(loaded, path));
    const auto reloaded = Find(loaded);
    ASSERT_TRUE(reloaded.IsValid());
    Stored(loaded, reloaded);
    const auto* loadedWall = loaded.GetComponent<Components::SplineWall>(reloaded);
    ASSERT_NE(loadedWall, nullptr);
    EXPECT_EQ(loadedWall->Corner, Components::SplineWallCorner::Round);
}

// The derivation runs at load too, before the saved fields: a wall on a curved
// spline whose file carries no Corner line loads Round, the corner its spline
// gives, not the component's static Mitre. With the saved value applied after
// it (the test above), the order is pinned from both sides.
TEST_F(SplineSceneSerialization, AWallSavedWithoutACornerTakesTheOneItsSplineGives)
{
    auto& service = SplineECS::SplineService::Get();
    const auto handle = service.CreateSpline(Spline::SplineType::CatmullRom, false);
    allocated.push_back(handle);
    auto* data = service.GetSplineData(handle);
    data->AddPoint({0, 0, 0}, 1.0f);
    data->AddPoint({4, 0, 3}, 1.0f);
    data->AddPoint({8, 0, 0}, 1.0f);
    service.RebuildCache(handle);

    ECS::World original;
    const auto entity = Attach(original, handle);
    original.AddComponentImmediate(entity, Components::SplineWall{});
    const auto path = directory / "wall-no-corner.scene";
    ASSERT_TRUE(Scene::SaveSceneToFile(original, path));
    std::string text = Read(path);
    const size_t line = text.find("SplineWall.Corner = ");
    ASSERT_NE(line, std::string::npos) << text;
    text.erase(line, text.find('\n', line) + 1u - line);
    {
        std::ofstream file(path, std::ios::trunc);
        file << text;
    }

    ECS::World loaded;
    ASSERT_TRUE(Load(loaded, path));
    const auto reloaded = Find(loaded);
    ASSERT_TRUE(reloaded.IsValid());
    Stored(loaded, reloaded);
    const auto* loadedWall = loaded.GetComponent<Components::SplineWall>(reloaded);
    ASSERT_NE(loadedWall, nullptr);
    EXPECT_EQ(loadedWall->Corner, Components::SplineWallCorner::Round);
}

TEST_F(SplineSceneSerialization, StaleReflectedBlockIsDroppedWithAWarningInEitherOrder)
{
    ECS::World original;
    const auto originalHandle = Create();
    const auto originalEntity = Attach(original, originalHandle);
    const auto portable = Portable(original, originalEntity);
    for (const bool staleFirst : {true, false})
    {
        SCOPED_TRACE(staleFirst);
        const auto path = directory / (staleFirst ? "stale-first.scene" : "portable-first.scene");
        ASSERT_TRUE(Write(path, staleFirst ? StaleReflectedBlock(originalHandle) + portable
                                           : portable + StaleReflectedBlock(originalHandle)));
        std::vector<std::string> log;
        ECS::World loaded;
        {
            TestLog::ScopedEngineLogCapture capture(&log, Logger::LogLevel::Warning);
            ASSERT_TRUE(Load(loaded, path));
            Logger::Log::Flush();
        }
        EXPECT_EQ(TestLog::CountLinesContaining(log, "dropping 'SplineComponent' on 'curve'"), 1u);
        EXPECT_EQ(TestLog::CountLinesContaining(log, "saves the component as 'Spline'"), 1u);
        const auto entity = Find(loaded);
        ASSERT_TRUE(entity.IsValid());
        ASSERT_FALSE(Stored(loaded, entity) == originalHandle);
        EXPECT_EQ(Portable(loaded, entity), portable);
        const auto resaved = directory / "canonical.scene";
        ASSERT_TRUE(Scene::SaveSceneToFile(loaded, resaved));
        EXPECT_EQ(Read(resaved).find("SplineComponent."), std::string::npos);
        EXPECT_EQ(Portable(original, originalEntity), portable);
    }
}

TEST_F(SplineSceneSerialization, RawHandleOnlyNeverAcquiresALiveSpline)
{
    const auto handle = Create();
    const auto path = directory / "raw-only.scene";
    ASSERT_TRUE(Write(path, StaleReflectedBlock(handle)));
    ECS::World loaded;
    ASSERT_TRUE(Load(loaded, path));
    const auto entity = Find(loaded);
    ASSERT_TRUE(entity.IsValid());
    EXPECT_FALSE(loaded.HasComponent<Components::SplineComponent>(entity));
    EXPECT_TRUE(SplineECS::SplineService::Get().IsValid(handle));
}

TEST_F(SplineSceneSerialization, ServiceHandleWordsAreTransientInReflection)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("SplineComponent");
    size_t transientFields = 0;
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        const bool handleWord = field.Name == "SplineDataIndex" || field.Name == "SplineDataGeneration";
        EXPECT_EQ(ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::Transient), handleWord) << field.Name;
        transientFields += handleWord ? 1u : 0u;
    }
    EXPECT_EQ(transientFields, 2u);

    // Reflection itself, independent of the registry's type claim: a synthesized schema for the
    // reflected name neither writes the handle nor applies one from a file.
    ECS::World world;
    const auto handle = Create();
    const auto entity = Attach(world, handle);
    const Scene::ReflectionSceneSchema reflected(typeId, "SplineComponent");
    std::vector<std::string> lines;
    reflected.Serialize(world, entity, {}, lines);
    for (const auto& line : lines)
    {
        EXPECT_EQ(line.find("SplineDataIndex"), std::string::npos) << line;
        EXPECT_EQ(line.find("SplineDataGeneration"), std::string::npos) << line;
    }
    EXPECT_FALSE(lines.empty());
    const auto other = Create(50.f);
    std::string error;
    EXPECT_TRUE(reflected.ApplyProperty(world, entity, {}, "splinedataindex",
                                        std::to_string(other.Index()), &error)) << error;
    EXPECT_TRUE(reflected.ApplyProperty(world, entity, {}, "splinedatageneration",
                                        std::to_string(other.Generation()), &error)) << error;
    EXPECT_TRUE(Stored(world, entity) == handle);
}
} // namespace
