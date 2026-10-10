// Undo and redo of the edits that start and end a sky's drive of its sun light, through the real
// undo service and the production sky system: the light's color, and under the sky's illuminance
// curve its Intensity, must come back as the author left them, and the drive must resume or end with
// the edit.
//
// The sky system hands a light it stops driving back white by itself (EngineRenderServicesWorld
// MaterialTests: SkyLinkedSun.EndingTheDriveHandsTheLightBackWhite covers every way a drive ends).
// What only the editor can do is remember the author's values when a drive starts, so undoing the
// link restores them; SkySunLinkUndo does that inside the edit's undo step, for the sky inspector's
// own edits and, through the sky's EditorComponentTraits::BeforeGenericEdit, for the generic edits
// that start a drive too.

#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Editor/Entities/ComponentEnabledToggle.h"
#include "Editor/Entities/ComponentRemoval.h"
#include "Engine/Rendering/RenderServices.h"
#include "Sky/SkyEnvironmentComponentTraits.h"
#include "Sky/SkyEnvironmentCreation.h"
#include "Sky/SkySunLinkUndo.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstring>
#include <string>

using namespace GameEngine;

namespace
{

constexpr float kDuskHours = 18.5f;
constexpr float kAuthoredColor[3] = {1.0f, 0.5f, 0.25f};
constexpr const char* kHdriAssetGuid = "0b1c2d3e-4f50-6172-8394-a5b6c7d8e9f0";

struct SkyUndoRig
{
    ECS::World World;
    Engine::Renderer::RenderServices Services;
    Engine::Renderer::SkyEnvironmentSystem System{&Services};
    Editor::UndoRedoService Undo;
    ECS::EntityHandle Sky;
    ECS::EntityHandle Sun;

    SkyUndoRig()
    {
        Editor::RegisterSkyEnvironmentComponentTraits();

        Sun = World.CreateEntity();
        Components::Light light{};
        light.Type = Components::LightType::Directional;
        light.Intensity = Components::kClearNoonSunIlluminanceLux;
        light.IntensityUnit = Components::LightUnit::Lux;
        for (int c = 0; c < 3; ++c)
            light.Color[c] = kAuthoredColor[c];
        World.AddComponentImmediate(Sun, light);
        World.AddComponentImmediate(Sun, Components::Transform{});

        Sky = World.CreateEntity();
        Components::SkyEnvironment sky{};
        sky.TimeOfDayHours = kDuskHours;
        World.AddComponentImmediate(Sky, sky);
    }

    void Tick() { System.Update(World, 0.0f); }

    const Components::Light& Light() const { return *World.GetComponent<Components::Light>(Sun); }

    bool HasAuthoredColor() const
    {
        return Light().Color[0] == kAuthoredColor[0] && Light().Color[1] == kAuthoredColor[1] &&
               Light().Color[2] == kAuthoredColor[2];
    }

    bool IsWhite() const { return Light().Color[0] == 1.0f && Light().Color[1] == 1.0f && Light().Color[2] == 1.0f; }

    // The sky inspector's discrete commit: one compound step holding the sky's snapshot edit and,
    // when a drive starts, the light's fields to put back on undo.
    template <typename MutateFn>
    void Commit(const std::string& label, MutateFn&& mutate)
    {
        const Editor::SkySunLinkUndo::DriveBefore driveBefore = Editor::SkySunLinkUndo::CaptureDrive(World);
        Undo.BeginCompound(label);
        auto edit = Undo.BeginInteractiveEdit(
            label, Editor::MultiEntityUndo::MakeComponentSnapshotTarget(
                       &World, {Sky}, ECS::GetComponentTypeId<Components::SkyEnvironment>(), nullptr, label));
        Components::SkyEnvironment updated = *World.GetComponent<Components::SkyEnvironment>(Sky);
        mutate(updated);
        World.AddComponentImmediate(Sky, updated);
        edit.Commit();
        Editor::SkySunLinkUndo::RecordDrivenFieldsForUndo(World, driveBefore, &Undo, label);
        Undo.EndCompound();
    }
};

} // namespace

// Link a light the author colored, let the sky drive it, undo: the author's color comes back and
// stays; redo: the sky drives it again.
TEST(SkySunLinkUndo, UndoingALinkPutsTheAuthorsColorBack)
{
    SkyUndoRig rig;
    rig.Tick();
    ASSERT_TRUE(rig.HasAuthoredColor()) << "an unlinked sky must not touch the light";

    rig.Commit("Change Sky Sun Light", [&](Components::SkyEnvironment& sky) { sky.SunLight = rig.Sun; });
    rig.Tick();
    ASSERT_FALSE(rig.HasAuthoredColor()) << "the linked light must be driven at dusk";

    rig.Undo.Undo();
    EXPECT_TRUE(rig.HasAuthoredColor()) << "undoing the link must restore the author's color";
    rig.Tick();
    rig.Tick();
    EXPECT_TRUE(rig.HasAuthoredColor()) << "the sky must not overwrite the restored color";

    rig.Undo.Redo();
    rig.Tick();
    EXPECT_FALSE(rig.HasAuthoredColor()) << "redoing the link must drive the light again";
}

// Turn the drive off, then undo and redo it: the light goes white, is driven again, goes white.
TEST(SkySunLinkUndo, UndoingDriveOffResumesTheDrive)
{
    SkyUndoRig rig;
    rig.Commit("Change Sky Sun Light", [&](Components::SkyEnvironment& sky) { sky.SunLight = rig.Sun; });
    rig.Tick();
    const float drivenBlue = rig.Light().Color[2];
    ASSERT_LT(drivenBlue, 0.5f) << "dusk must tint the driven light";

    rig.Commit("Change Sky Time Drives Light Source",
               [](Components::SkyEnvironment& sky) { sky.TimeOfDayDrivesSunLight = false; });
    rig.Tick();
    EXPECT_TRUE(rig.IsWhite()) << "ending the drive must hand the light back white";

    rig.Undo.Undo();
    rig.Tick();
    EXPECT_EQ(rig.Light().Color[2], drivenBlue) << "undoing drive-off must resume the drive";

    rig.Undo.Redo();
    rig.Tick();
    EXPECT_TRUE(rig.IsWhite()) << "redoing drive-off must hand the light back white again";
}

// A second sky the system does not render never drives its light; turning it off through the same
// commit changes nothing on that light.
TEST(SkySunLinkUndo, ASecondSkyNeverTouchesItsLight)
{
    SkyUndoRig rig;
    rig.Commit("Change Sky Sun Light", [&](Components::SkyEnvironment& sky) { sky.SunLight = rig.Sun; });
    rig.Tick();

    const ECS::EntityHandle fill = rig.World.CreateEntity();
    Components::Light fillLight{};
    fillLight.Type = Components::LightType::Directional;
    fillLight.Color[0] = 0.2f;
    fillLight.Color[1] = 0.4f;
    fillLight.Color[2] = 1.0f;
    rig.World.AddComponentImmediate(fill, fillLight);
    rig.World.AddComponentImmediate(fill, Components::Transform{});
    const ECS::EntityHandle second = rig.World.CreateEntity();
    Components::SkyEnvironment secondSky{};
    secondSky.SunLight = fill;
    rig.World.AddComponentImmediate(second, secondSky);
    rig.Tick();

    const ECS::EntityHandle primary = rig.Sky;
    rig.Sky = second;
    rig.Commit("Change Sky Time Drives Light Source",
               [](Components::SkyEnvironment& sky) { sky.TimeOfDayDrivesSunLight = false; });
    rig.Sky = primary;
    rig.Tick();

    const auto* after = rig.World.GetComponent<Components::Light>(fill);
    EXPECT_EQ(after->Color[0], 0.2f);
    EXPECT_EQ(after->Color[1], 0.4f);
    EXPECT_EQ(after->Color[2], 1.0f);
    EXPECT_EQ(rig.Undo.GetUndoCount(), 2u) << "the second sky's edit is one step and records no color";
}

// The inspector's component-header enable toggle (CommitComponentEnabledToggle), which edits any
// component without knowing it: re-enabling a linked sky starts its drive, and undoing the toggle
// must hand the light back the author's color, not white.
TEST(SkySunLinkUndo, UndoingTheEnableToggleThatStartsADrivePutsTheAuthorsColorBack)
{
    SkyUndoRig rig;
    Components::SkyEnvironment disabled = *rig.World.GetComponent<Components::SkyEnvironment>(rig.Sky);
    disabled.SunLight = rig.Sun;
    rig.World.AddComponentImmediate(rig.Sky, disabled);
    ECS::Entity(&rig.World, rig.Sky).SetEnabled<Components::SkyEnvironment>(false);
    rig.Tick();
    ASSERT_TRUE(rig.HasAuthoredColor()) << "a disabled sky must not touch the light";

    Editor::CommitComponentEnabledToggle(rig.World, &rig.Undo, nullptr, rig.Sky, {},
                                         ECS::GetComponentTypeId<Components::SkyEnvironment>(), "Sky Environment",
                                         true);
    rig.Tick();
    ASSERT_FALSE(rig.HasAuthoredColor()) << "the re-enabled sky must drive the light at dusk";
    EXPECT_EQ(rig.Undo.GetUndoCount(), 1u) << "the toggle and the color record are one step";

    rig.Undo.Undo();
    EXPECT_TRUE(rig.HasAuthoredColor()) << "undoing the toggle must restore the author's color";
    rig.Tick();
    rig.Tick();
    EXPECT_TRUE(rig.HasAuthoredColor()) << "the disabled sky must leave the restored color alone";

    rig.Undo.Redo();
    rig.Tick();
    EXPECT_FALSE(rig.HasAuthoredColor()) << "redoing the toggle must drive the light again";
}

// Hierarchy > Create > Sky Environment links the first directional light and starts driving it.
// The creation is one undo step, and undoing it removes the sky and hands the light back the
// author's color.
TEST(SkySunLinkUndo, UndoingTheCreationOfASkyPutsTheAuthorsColorBack)
{
    SkyUndoRig rig;
    rig.World.DestroyEntityImmediate(rig.Sky);
    rig.Tick();
    ASSERT_TRUE(rig.HasAuthoredColor()) << "with no sky nothing touches the light";

    const ECS::EntityHandle sky = Editor::CreateSkyEnvironmentEntity(rig.World, &rig.Undo, nullptr);
    ASSERT_TRUE(rig.World.IsValid(sky));
    EXPECT_EQ(rig.World.GetComponent<Components::SkyEnvironment>(sky)->SunLight, rig.Sun)
        << "the new sky links the first directional light";
    rig.Tick();
    ASSERT_FALSE(rig.HasAuthoredColor()) << "the new sky must drive the light";
    EXPECT_EQ(rig.Undo.GetUndoCount(), 1u) << "the creation and the color record are one step";

    rig.Undo.Undo();
    EXPECT_FALSE(rig.World.IsValid(sky)) << "undoing the creation must remove the sky";
    EXPECT_TRUE(rig.HasAuthoredColor()) << "undoing the creation must restore the author's color";
    rig.Tick();
    rig.Tick();
    EXPECT_TRUE(rig.HasAuthoredColor()) << "nothing may overwrite the restored color once the sky is gone";

    rig.Undo.Redo();
    ASSERT_TRUE(rig.World.IsValid(sky)) << "redoing the creation must revive the same sky";
    rig.Tick();
    EXPECT_FALSE(rig.HasAuthoredColor()) << "redoing the creation must drive the light again";
}

// Inspector > Remove Component on an HDRI Skybox hands the sun to the sky, which starts driving it.
// Undoing the removal brings the skybox back and must hand the light back the author's color.
// The sky system is not ticked while the skybox is present: its skybox branch needs a GPU texture
// this headless rig cannot upload, so the drive state is read from DrivenSunLight there instead.
TEST(SkySunLinkUndo, UndoingTheRemovalOfAnHdriSkyboxPutsTheAuthorsColorBack)
{
    SkyUndoRig rig;
    Components::SkyEnvironment linked = *rig.World.GetComponent<Components::SkyEnvironment>(rig.Sky);
    linked.SunLight = rig.Sun;
    rig.World.AddComponentImmediate(rig.Sky, linked);
    const ECS::EntityHandle skybox = rig.World.CreateEntity();
    Components::Skybox hdri{};
    std::strncpy(hdri.HDRIAssetGuid, kHdriAssetGuid, sizeof(hdri.HDRIAssetGuid) - 1);
    rig.World.AddComponentImmediate(skybox, hdri);
    ASSERT_FALSE(Components::SkySunIlluminance::DrivenSunLight(rig.World).IsValid())
        << "an HDRI skybox is drawn instead of the sky, so the light is not driven";

    Editor::CommitComponentRemoval(rig.World, &rig.Undo, nullptr, skybox, ECS::GetComponentTypeId<Components::Skybox>(),
                                   "Remove Skybox Component");
    rig.Tick();
    ASSERT_FALSE(rig.HasAuthoredColor()) << "with the HDRI skybox removed the sky must drive the light at dusk";
    EXPECT_EQ(rig.Undo.GetUndoCount(), 1u) << "the removal and the color record are one step";

    rig.Undo.Undo();
    EXPECT_NE(rig.World.GetComponent<Components::Skybox>(skybox), nullptr) << "undo must bring the skybox back";
    EXPECT_FALSE(Components::SkySunIlluminance::DrivenSunLight(rig.World).IsValid())
        << "the restored skybox keeps the sky off the light";
    EXPECT_TRUE(rig.HasAuthoredColor()) << "undoing the removal must restore the author's color";

    rig.Undo.Redo();
    rig.Tick();
    EXPECT_FALSE(rig.HasAuthoredColor()) << "redoing the removal must drive the light again";
}

// Switching the sky to its illuminance curve starts writing the light's Intensity. Undoing the switch
// puts the author's Intensity back in the same step, before the sky runs again, and the sky then
// leaves it.
TEST(SkySunLinkUndo, UndoingTheSwitchToACurvePutsTheAuthorsIntensityBack)
{
    SkyUndoRig rig;
    rig.Commit("Change Sky Sun Light", [&rig](Components::SkyEnvironment& sky) { sky.SunLight = rig.Sun; });
    rig.Tick();
    ASSERT_EQ(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux);

    rig.Commit("Change Sun Illuminance Source", [](Components::SkyEnvironment& sky) {
        sky.SunIlluminanceSource = Components::SkySunIlluminanceSource::Curve;
    });
    rig.Tick();
    ASSERT_LT(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux * 0.01f) << "dusk under the curve";

    rig.Undo.Undo();
    EXPECT_EQ(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux)
        << "the undo itself must put the author's Intensity back";
    rig.Tick();
    rig.Tick();
    EXPECT_EQ(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux);
}

// The same through the component header's enable toggle, which edits the sky without knowing it: a sky
// set to its curve starts writing the Intensity when it is enabled, and undoing the toggle puts the
// author's Intensity back.
TEST(SkySunLinkUndo, UndoingTheEnableToggleThatStartsACurvePutsTheAuthorsIntensityBack)
{
    SkyUndoRig rig;
    Components::SkyEnvironment disabled = *rig.World.GetComponent<Components::SkyEnvironment>(rig.Sky);
    disabled.SunLight = rig.Sun;
    disabled.SunIlluminanceSource = Components::SkySunIlluminanceSource::Curve;
    rig.World.AddComponentImmediate(rig.Sky, disabled);
    ECS::Entity(&rig.World, rig.Sky).SetEnabled<Components::SkyEnvironment>(false);
    rig.Tick();
    ASSERT_EQ(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux);

    Editor::CommitComponentEnabledToggle(rig.World, &rig.Undo, nullptr, rig.Sky, {},
                                         ECS::GetComponentTypeId<Components::SkyEnvironment>(), "Sky Environment",
                                         true);
    rig.Tick();
    ASSERT_LT(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux * 0.01f) << "dusk under the curve";

    rig.Undo.Undo();
    EXPECT_EQ(rig.Light().Intensity, Components::kClearNoonSunIlluminanceLux)
        << "the undo itself must put the author's Intensity back";
}
