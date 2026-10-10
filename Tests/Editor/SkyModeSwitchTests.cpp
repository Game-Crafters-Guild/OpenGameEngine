// Switching the Sky Mode with several skies selected is one undo step that covers every one of them.

#include <gtest/gtest.h>

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Sky/SkyEnvironmentEdit.h"
#include "UndoRedo/UndoRedoService.h"

using namespace GameEngine;

TEST(SkyModeSwitch, OneUndoPutsEverySelectedSkyBack)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle first = world.CreateEntity();
    const ECS::EntityHandle second = world.CreateEntity();
    world.AddComponentImmediate(first, Components::SkyEnvironment{});
    world.AddComponentImmediate(second, Components::SkyEnvironment{});

    Editor::SwitchSkyMode(&world, first, {second}, nullptr, &undo, Components::SkyMode::Gradient);
    for (const ECS::EntityHandle sky : {first, second})
        EXPECT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->Mode, Components::SkyMode::Gradient);

    undo.Undo();
    for (const ECS::EntityHandle sky : {first, second})
        EXPECT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->Mode, Components::SkyMode::Physical)
            << "the second sky is in the same undo step";

    undo.Redo();
    for (const ECS::EntityHandle sky : {first, second})
        EXPECT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->Mode, Components::SkyMode::Gradient);
}
