// CommitEntityComponentsEdit (UndoRedo/EntityComponentsEdit.h): a write that changes one
// component and adds another is one undo step; a refused or empty write records none.

#include "UndoRedo/EntityComponentsEdit.h"

#include "Components/Audio/AudioEmitter.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace GameEngine;

namespace
{

Components::Name MakeName(const char* text)
{
    Components::Name name{};
    std::strncpy(name.value, text, sizeof(name.value) - 1);
    return name;
}

} // namespace

// Undo puts back the changed component's bytes and removes the one the write added; redo
// brings both back. The step carries the caller's name.
TEST(EntityComponentsEditTest, AChangedAndAnAddedComponentAreOneStep)
{
    ECS::World world;
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle entity = world.CreateEntity();
    const Mathematics::Vector3 unitScale(1.0f, 1.0f, 1.0f);
    world.AddComponentImmediate(
        entity, Components::Transform::FromTRS(Mathematics::Vector3(1.0f, 2.0f, 3.0f), Mathematics::Quaternion{}, unitScale));

    const bool applied = Editor::CommitEntityComponentsEdit(world, entity, &undo, &notifications, "Set Name", [&]() {
        world.GetComponentForWrite<Components::Transform>(entity)->Translate(3.0f, 3.0f, 3.0f);
        world.AddComponentImmediate(entity, MakeName("Tower"));
        return true;
    });
    ASSERT_TRUE(applied);
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Set Name");

    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::Transform>(entity)->GetPosition().x, 1.0f);
    EXPECT_FALSE(world.HasComponent<Components::Name>(entity));

    undo.Redo();
    EXPECT_EQ(world.GetComponent<Components::Transform>(entity)->GetPosition().x, 4.0f);
    ASSERT_TRUE(world.HasComponent<Components::Name>(entity));
    EXPECT_EQ(world.GetComponent<Components::Name>(entity)->View(), "Tower");
}

// A write that changed nothing records no step, refused or not; a refused write that changed
// something before it refused records that change, so undo takes it back.
TEST(EntityComponentsEditTest, AStepCoversWhatLandedRefusedOrNot)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, MakeName("Tower"));

    EXPECT_FALSE(Editor::CommitEntityComponentsEdit(world, entity, &undo, nullptr, "Set Name", []() { return false; }));
    EXPECT_TRUE(Editor::CommitEntityComponentsEdit(world, entity, &undo, nullptr, "Set Name", []() { return true; }));
    EXPECT_EQ(undo.GetUndoCount(), 0u);

    EXPECT_FALSE(Editor::CommitEntityComponentsEdit(world, entity, &undo, nullptr, "Set Name", [&]() {
        world.AddComponentImmediate(entity, MakeName("Keep"));
        return false;
    }));
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::Name>(entity)->View(), "Tower");
}

// The enable state a write switches (the component's ComponentDisabled tag) is part of the step:
// undo and redo switch it back and forth.
TEST(EntityComponentsEditTest, ASwitchedEnableStateIsPartOfTheStep)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, Components::AudioEmitter{});
    const ECS::ComponentTypeId emitterType = ECS::GetComponentTypeId<Components::AudioEmitter>();

    ASSERT_TRUE(Editor::CommitEntityComponentsEdit(world, entity, &undo, nullptr, "Set AudioEmitter", [&]() {
        return world.SetComponentEnabledImmediate(entity, emitterType, false);
    }));
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_FALSE(world.IsComponentEnabled(entity, emitterType));
    undo.Undo();
    EXPECT_TRUE(world.IsComponentEnabled(entity, emitterType));
    undo.Redo();
    EXPECT_FALSE(world.IsComponentEnabled(entity, emitterType));
}
