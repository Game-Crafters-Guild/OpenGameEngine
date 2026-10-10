#include <gtest/gtest.h>

#include "EngineLogCapture.h"

#include "Ocean/OceanPresetAsset.h"
#include "Ocean/OceanPresetOverrideValidator.h"
#include "Components/Rendering/Ocean.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Types/StringId.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace GameEngine::Ocean
{
namespace
{
struct PresetFields
{
    float First = 1.0f;
    float Removed = 2.0f;
    float Middle = 3.0f;
    float Last = 4.0f;
};

struct RemainingPresetFields
{
    float Last = 4.0f;
    float First = 1.0f;
    float Middle = 3.0f;
};

class OceanPresetOverrideTests : public testing::Test
{
protected:
    static constexpr ECS::ComponentTypeId kComponentType = HashStringId("OceanPresetOverrideTests.Fields");

    void TearDown() override
    {
        ECS::ComponentFieldRegistry::UnregisterForTests(kComponentType);
    }
};

TEST_F(OceanPresetOverrideTests, RemovingAndReorderingFieldsPreservesEveryRemainingOverride)
{
    const std::array fields = {
        ECS::FieldInfo{"First", offsetof(PresetFields, First), sizeof(float), ECS::FieldTypeIdOf<float>()},
        ECS::FieldInfo{"Removed", offsetof(PresetFields, Removed), sizeof(float), ECS::FieldTypeIdOf<float>()},
        ECS::FieldInfo{"Middle", offsetof(PresetFields, Middle), sizeof(float), ECS::FieldTypeIdOf<float>()},
        ECS::FieldInfo{"Last", offsetof(PresetFields, Last), sizeof(float), ECS::FieldTypeIdOf<float>()}
    };
    ECS::ComponentFieldRegistry::RegisterOwned(kComponentType, fields, "PresetFields");
    OceanPresetAsset preset;
    preset.Set("Surface.First", 10.0);
    preset.Set("Surface.Removed", 20.0);
    preset.Set("Surface.Middle", 30.0);
    preset.Set("Surface.Last", 40.0);
    const Components::OceanPresetOverride overrides[] = {
        {HashStringId("Surface.Removed")}, {HashStringId("Surface.Middle")}, {HashStringId("Surface.Last")}
    };
    PresetFields original;
    EXPECT_EQ(preset.Apply("Surface", kComponentType, &original, sizeof(original), overrides), 1u);
    EXPECT_FLOAT_EQ(original.First, 10.0f);
    EXPECT_FLOAT_EQ(original.Removed, 2.0f);
    EXPECT_FLOAT_EQ(original.Middle, 3.0f);
    EXPECT_FLOAT_EQ(original.Last, 4.0f);

    const std::array remainingFields = {
        ECS::FieldInfo{"Last", offsetof(RemainingPresetFields, Last), sizeof(float), ECS::FieldTypeIdOf<float>()},
        ECS::FieldInfo{"First", offsetof(RemainingPresetFields, First), sizeof(float), ECS::FieldTypeIdOf<float>()},
        ECS::FieldInfo{"Middle", offsetof(RemainingPresetFields, Middle), sizeof(float), ECS::FieldTypeIdOf<float>()}
    };
    ECS::ComponentFieldRegistry::RegisterOwned(kComponentType, remainingFields, "PresetFields");
    RemainingPresetFields remaining;
    EXPECT_EQ(preset.Apply("Surface", kComponentType, &remaining, sizeof(remaining), overrides), 1u);
    EXPECT_FLOAT_EQ(remaining.First, 10.0f);
    EXPECT_FLOAT_EQ(remaining.Middle, 3.0f);
    EXPECT_FLOAT_EQ(remaining.Last, 4.0f);
}

TEST_F(OceanPresetOverrideTests, SameFieldNameInAnotherComponentIsNotOverridden)
{
    const std::array fields = {
        ECS::FieldInfo{"First", offsetof(PresetFields, First), sizeof(float), ECS::FieldTypeIdOf<float>()}
    };
    ECS::ComponentFieldRegistry::RegisterOwned(kComponentType, fields, "PresetFields");
    OceanPresetAsset preset;
    preset.Set("Surface.First", 10.0);
    preset.Set("Renderer.First", 20.0);
    Components::OceanPresetBinding binding{};
    binding.Overrides[Components::kOceanPresetOverrideCapacity - 1u].FieldIdentifier = HashStringId("Surface.First");
    PresetFields surface;
    PresetFields renderer;
    EXPECT_EQ(preset.Apply("Surface", kComponentType, &surface, sizeof(surface), binding.Overrides), 0u);
    EXPECT_FLOAT_EQ(surface.First, 1.0f);
    EXPECT_EQ(preset.Apply("Renderer", kComponentType, &renderer, sizeof(renderer), binding.Overrides), 1u);
    EXPECT_FLOAT_EQ(renderer.First, 20.0f);
}

TEST(OceanPresetOverridesOfTests, ReadsTheBindingInPlace)
{
    ECS::World world;
    const ECS::EntityHandle bound = world.CreateEntity();
    Components::OceanPresetBinding binding{};
    binding.Overrides[3].FieldIdentifier = HashStringId("Renderer.LodCount");
    world.AddComponentImmediate(bound, binding);

    const std::span<const Components::OceanPresetOverride> overrides = PresetOverridesOf(world, bound);
    EXPECT_EQ(overrides.data(), world.GetComponent<Components::OceanPresetBinding>(bound)->Overrides);
    ASSERT_EQ(overrides.size(), Components::kOceanPresetOverrideCapacity);
    EXPECT_EQ(overrides[3].FieldIdentifier, HashStringId("Renderer.LodCount"));
    EXPECT_TRUE(PresetOverridesOf(world, world.CreateEntity()).empty());
}

TEST(OceanPresetOverrideValidatorTests, UnknownOverrideIsReportedOnceWhenItsBindingIsWritten)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::OceanPresetBinding binding{};
    binding.Overrides[0].FieldIdentifier = HashStringId("Surface.FoamRoughness");
    binding.Overrides[7].FieldIdentifier = HashStringId("Surface.RemovedField");
    world.AddComponentImmediate(entity, binding);
    const StringId unstamped = HashStringId("Surface.AnotherRemovedField");

    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
    OceanPresetOverrideValidator validator;
    validator.Validate(world);
    Logger::Log::Flush();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("entity " + std::to_string(entity.id) + ":"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find(std::format("OceanPresetBinding.Overrides7.FieldIdentifier = {}",
                                        HashStringId("Surface.RemovedField"))),
              std::string::npos)
        << lines[0];

    // A write that takes no write grant leaves the column unchanged to the validator,
    // so the next run reads no binding.
    const_cast<Components::OceanPresetBinding*>(world.GetComponent<Components::OceanPresetBinding>(entity))
        ->Overrides[8]
        .FieldIdentifier = unstamped;
    validator.Validate(world);
    Logger::Log::Flush();
    EXPECT_EQ(lines.size(), 1u);

    // A granted write re-reads the binding: the new identity is reported, the one
    // already reported is not.
    world.Query<ECS::Write<Components::OceanPresetBinding>>().Each(
        [unstamped](Components::OceanPresetBinding& written)
        {
            written.Overrides[8].FieldIdentifier = unstamped;
        });
    validator.Validate(world);
    Logger::Log::Flush();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[1].find(std::format("OceanPresetBinding.Overrides8.FieldIdentifier = {}", unstamped)),
              std::string::npos)
        << lines[1];
}
} // namespace
} // namespace GameEngine::Ocean
