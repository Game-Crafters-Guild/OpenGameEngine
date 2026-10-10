// Switching the Sun path with several skies selected is one undo step that covers every one of
// them, and switching keeps every authored field of both paths. The first switch to Custom starts it
// where the Earth path is; a later switch does not seed again.

#include <gtest/gtest.h>

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Sky/SkySunPathSwitch.h"
#include "UndoRedo/UndoRedoService.h"

using namespace GameEngine;

TEST(SkySunPathSwitch, OneUndoPutsEverySelectedSkyBack)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    Components::SkyEnvironment authored{};
    authored.Latitude = 51.5f;
    authored.CustomAxisAltitude = 35.0f;
    const ECS::EntityHandle first = world.CreateEntity();
    const ECS::EntityHandle second = world.CreateEntity();
    world.AddComponentImmediate(first, authored);
    world.AddComponentImmediate(second, authored);

    Editor::SwitchSkySunPath(&world, first, {second}, nullptr, &undo, Components::SkySunPathKind::Custom);
    for (const ECS::EntityHandle sky : {first, second})
    {
        const auto* switched = world.GetComponent<Components::SkyEnvironment>(sky);
        ASSERT_NE(switched, nullptr);
        EXPECT_EQ(switched->SunPath, Components::SkySunPathKind::Custom);
        EXPECT_EQ(switched->Latitude, 51.5f) << "the Earth fields stay stored";
        EXPECT_EQ(switched->CustomAxisAltitude, 35.0f);
    }

    undo.Undo();
    for (const ECS::EntityHandle sky : {first, second})
        EXPECT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->SunPath, Components::SkySunPathKind::Earth)
            << "the second sky is in the same undo step";

    undo.Redo();
    for (const ECS::EntityHandle sky : {first, second})
        EXPECT_EQ(world.GetComponent<Components::SkyEnvironment>(sky)->SunPath, Components::SkySunPathKind::Custom);
}

TEST(SkySunPathSwitch, TheFirstSwitchSeedsCustomFromEarthAndLaterOnesDoNot)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    Components::SkyEnvironment authored{};
    authored.Latitude = 51.5f;
    authored.DayOfYear = 172;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, authored);

    Editor::SwitchSkySunPath(&world, sky, {}, nullptr, &undo, Components::SkySunPathKind::Custom);
    const Components::SkyEnvironment seeded = *world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(seeded.CustomAxisAltitude, 51.5f) << "seeded from the latitude";

    Editor::SwitchSkySunPath(&world, sky, {}, nullptr, &undo, Components::SkySunPathKind::Earth);
    Components::SkyEnvironment moved = *world.GetComponent<Components::SkyEnvironment>(sky);
    moved.Latitude = -33.9f;
    world.AddComponentImmediate(sky, moved);
    Editor::SwitchSkySunPath(&world, sky, {}, nullptr, &undo, Components::SkySunPathKind::Custom);
    const auto* again = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(again->CustomAxisAltitude, seeded.CustomAxisAltitude) << "the second switch does not seed again";
    EXPECT_EQ(again->CustomNoonHeight, seeded.CustomNoonHeight);

    // Three switches, three undo steps: back to before the first one.
    undo.Undo();
    undo.Undo();
    undo.Undo();
    const auto* back = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(back->SunPath, Components::SkySunPathKind::Earth);
    EXPECT_EQ(back->CustomAxisAltitude, 0.0f) << "undoing the first switch takes the seed back with it";
}
