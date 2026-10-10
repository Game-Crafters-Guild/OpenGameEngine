// Switching where the sun light's illuminance comes from, and replacing the sky's curve with the
// physical one, are each one undo step. The first switch to the curve seeds an untouched curve in the
// same step, so undoing the switch takes the seed back; a replace that is undone gives the author's
// curve back exactly.

#include <gtest/gtest.h>

#include <cstring>

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Sky/SkySunIlluminanceSwitch.h"
#include "UndoRedo/UndoRedoService.h"

using namespace GameEngine;

namespace
{
bool SameCurve(const Math::Curve& a, const Math::Curve& b)
{
    return std::memcmp(&a, &b, sizeof(Math::Curve)) == 0;
}
} // namespace

TEST(SkySunIlluminanceSwitch, TheFirstSwitchSeedsTheCurveInTheSameUndoStep)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    Components::SkyEnvironment authored{};
    authored.Latitude = 51.5f;
    authored.DayOfYear = 172;
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, authored);

    Editor::SwitchSkySunIlluminanceSource(&world, sky, {}, nullptr, &undo, Components::SkySunIlluminanceSource::Curve);
    const auto* switched = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(switched->SunIlluminanceSource, Components::SkySunIlluminanceSource::Curve);
    EXPECT_FALSE(Components::SkySunDrive::IsDefaultSunIlluminanceCurve(switched->SunIlluminanceCurve))
        << "an untouched curve is seeded from the sky's path";

    undo.Undo();
    const auto* back = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(back->SunIlluminanceSource, Components::SkySunIlluminanceSource::Light);
    EXPECT_TRUE(Components::SkySunDrive::IsDefaultSunIlluminanceCurve(back->SunIlluminanceCurve))
        << "undoing the switch takes the seed back with it";
}

TEST(SkySunIlluminanceSwitch, ReplacingWithThePhysicalCurveIsOneUndoStep)
{
    ECS::World world;
    Editor::UndoRedoService undo;
    Components::SkyEnvironment authored{};
    authored.Latitude = 51.5f;
    authored.DayOfYear = 355;
    authored.SunIlluminanceSource = Components::SkySunIlluminanceSource::Curve;
    authored.SunIlluminanceCurve = Math::Curve{};
    authored.SunIlluminanceCurve.TryInsert(Math::CurveKey{6.0f, 500.0f});
    authored.SunIlluminanceCurve.TryInsert(Math::CurveKey{12.0f, 30000.0f});
    const ECS::EntityHandle sky = world.CreateEntity();
    world.AddComponentImmediate(sky, authored);

    Editor::ReplaceSkySunIlluminanceCurve(&world, sky, {}, nullptr, &undo);
    const auto* replaced = world.GetComponent<Components::SkyEnvironment>(sky);
    EXPECT_EQ(replaced->SunIlluminanceSource, Components::SkySunIlluminanceSource::Curve) << "not a mode switch";
    EXPECT_FALSE(SameCurve(replaced->SunIlluminanceCurve, authored.SunIlluminanceCurve));
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.GetUndoNameAt(0), "Replace Sun Illuminance Curve");

    undo.Undo();
    EXPECT_TRUE(SameCurve(world.GetComponent<Components::SkyEnvironment>(sky)->SunIlluminanceCurve,
                          authored.SunIlluminanceCurve))
        << "undo restores the author's curve";
}
