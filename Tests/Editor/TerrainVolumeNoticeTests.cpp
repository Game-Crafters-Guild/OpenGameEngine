// The Inspector notices for TerrainModifierVolume's Shape::Global.
//
// Shape::Global makes several fields stop meaning what the widget beside them
// implies, and the notices are the only thing that says so. They are therefore
// load-bearing UI, not decoration, and each one here is pinned against the
// specific wrong statement it replaced:
//
//   * "at full strength" contradicted the volume's own Weight, which still
//     multiplies into every effect and whose row sits directly beneath.
//   * The transform rows stay live and editable while the region stops being
//     scoped by them, so a drag changes nothing visible.
//   * The Stamp notice fired on component PRESENCE while the bake refuses on
//     ENABLED, so parking an effect the correct way produced a warning naming a
//     cause that was not the reason.
//
// Copy is asserted by phrase, not by whole string, so rewording stays cheap
// while the CLAIMS stay pinned.

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Inspectors/TerrainVolumeNotices.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Editor::AddGlobalVolumeScopeNotice;
using GameEngine::Editor::AddStampInGlobalVolumeNotice;

namespace Components = GameEngine::Components;

namespace
{

// The whole subtree, not the host's own row: an info card is a control that owns
// the elements its copy lives on, so what a notice added is not always a direct
// child of the host. Both readings below walk it through here, so neither can
// miss a set of elements the other sees.
template <typename Fn>
void ForEachDescendant(const UIElement& parent, Fn& fn)
{
    for (const auto& child : parent.GetChildren())
    {
        if (!child)
            continue;
        fn(*child);
        ForEachDescendant(*child, fn);
    }
}

// Every label the notice added, warnings and plain lines alike, lowercased so a
// phrase assertion does not also pin capitalization.
std::string AllTextIn(const UIElement& parent)
{
    std::string all;
    auto collect = [&all](const UIElement& element)
    {
        if (const auto* label = dynamic_cast<const Label*>(&element))
        {
            all += label->GetText();
            all += "\n";
        }
    };
    ForEachDescendant(parent, collect);
    for (auto& c : all)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return all;
}

std::size_t WarningCountIn(const UIElement& parent)
{
    std::size_t n = 0;
    auto count = [&n](const UIElement& element)
    {
        if (dynamic_cast<const GameEngine::EditorUI::InspectorNotice*>(&element))
            ++n;
    };
    ForEachDescendant(parent, count);
    return n;
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

Entity MakeVolume(World& world, Components::TerrainVolumeShape shape, float weight = 1.0f)
{
    Entity e = world.Create();
    Components::TerrainModifierVolume vol{};
    vol.Shape = shape;
    vol.Weight = weight;
    e.Set(vol);
    world.ProcessCommands();
    return e;
}

void AddStamp(World& world, Entity e, bool enabled)
{
    Components::TerrainStampEffect stamp{};
    stamp.Enabled = enabled;
    e.Set(stamp);
    world.ProcessCommands();
}

} // namespace

// ---- Scope notice ----------------------------------------------------------

// The notice has to carry BOTH things the shape takes away. The falloff rows are
// hidden for a global volume, so "no edge" is the only place that fact survives;
// the transform rows are NOT hidden, so without the second sentence a drag looks
// broken rather than inapplicable.
TEST(TerrainVolumeNotice, GlobalScopeNoticeNamesTheEdgeAndTheTransform)
{
    World world;
    Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global);

    UIElement host;
    AddGlobalVolumeScopeNotice(&host, world, e.GetHandle(), /*anySphericalTerrain=*/false);

    const std::string text = AllTextIn(host);
    ASSERT_FALSE(text.empty()) << "no scope notice was added for a global volume";
    EXPECT_TRUE(Contains(text, "every terrain")) << text;
    EXPECT_TRUE(Contains(text, "no edge")) << text;
    EXPECT_TRUE(Contains(text, "no falloff")) << text;
    EXPECT_TRUE(Contains(text, "moving or resizing")) << text;
}

// The regression this test exists for: the notice said "at full strength" while
// ComputeWeight returns the volume's Weight for a global, and the Weight row sits
// directly beneath it. The notice must make NO strength claim at all — which also
// means its wording cannot depend on Weight.
TEST(TerrainVolumeNotice, GlobalScopeNoticeMakesNoStrengthClaimAtAnyWeight)
{
    for (const float weight : {1.0f, 0.5f, 0.0f})
    {
        World world;
        Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global, weight);

        UIElement host;
        AddGlobalVolumeScopeNotice(&host, world, e.GetHandle(), /*anySphericalTerrain=*/false);

        const std::string text = AllTextIn(host);
        EXPECT_FALSE(Contains(text, "full strength"))
            << "weight " << weight << " still claims full strength: " << text;
        EXPECT_FALSE(Contains(text, "at full")) << text;
    }
}

// The region copy is a property of the SHAPE, so it must read identically
// whatever the master weight is. Pins the invariant the test above only implies.
TEST(TerrainVolumeNotice, GlobalScopeNoticeIsIdenticalAcrossWeights)
{
    auto textAtWeight = [](float weight) {
        World world;
        Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global, weight);
        UIElement host;
        AddGlobalVolumeScopeNotice(&host, world, e.GetHandle(), /*anySphericalTerrain=*/false);
        return AllTextIn(host);
    };

    EXPECT_EQ(textAtWeight(1.0f), textAtWeight(0.35f));
}

// The notice must not describe the region as unaffected by POSITION outright: a
// Flatten in "Relative To Volume" mode still reads the entity's Y through
// ComputeVolumeReferenceHeight. The claim is about what the volume COVERS.
TEST(TerrainVolumeNotice, GlobalScopeNoticeDoesNotClaimPositionIsIgnored)
{
    World world;
    Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global);

    UIElement host;
    AddGlobalVolumeScopeNotice(&host, world, e.GetHandle(), /*anySphericalTerrain=*/false);

    const std::string text = AllTextIn(host);
    EXPECT_FALSE(Contains(text, "position is ignored")) << text;
    EXPECT_FALSE(Contains(text, "position has no effect")) << text;
}

TEST(TerrainVolumeNotice, PlanetCaveatAppearsOnlyWithASphericalTerrain)
{
    World world;
    Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global);

    UIElement without;
    AddGlobalVolumeScopeNotice(&without, world, e.GetHandle(), /*anySphericalTerrain=*/false);
    EXPECT_FALSE(Contains(AllTextIn(without), "spherical"));

    UIElement with;
    AddGlobalVolumeScopeNotice(&with, world, e.GetHandle(), /*anySphericalTerrain=*/true);
    EXPECT_TRUE(Contains(AllTextIn(with), "spherical"));
}

// Every other shape has a real footprint, so none of this applies to it.
TEST(TerrainVolumeNotice, NonGlobalShapesGetNoScopeNotice)
{
    for (const auto shape : {Components::TerrainVolumeShape::Rectangle,
                             Components::TerrainVolumeShape::Circle,
                             Components::TerrainVolumeShape::SplinePath,
                             Components::TerrainVolumeShape::SplineArea})
    {
        World world;
        Entity e = MakeVolume(world, shape);

        UIElement host;
        AddGlobalVolumeScopeNotice(&host, world, e.GetHandle(), /*anySphericalTerrain=*/true);
        EXPECT_TRUE(AllTextIn(host).empty())
            << "shape " << static_cast<int>(shape) << " got a global-scope notice";
    }
}

// ---- Stamp refusal notice --------------------------------------------------

TEST(TerrainVolumeNotice, EnabledStampInAGlobalVolumeIsFlagged)
{
    World world;
    Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global);
    AddStamp(world, e, /*enabled=*/true);

    UIElement host;
    AddStampInGlobalVolumeNotice(&host, world, e.GetHandle());

    const std::string text = AllTextIn(host);
    ASSERT_FALSE(text.empty()) << "an enabled stamp on a global volume was not flagged";
    EXPECT_EQ(WarningCountIn(host), 1u);
    EXPECT_TRUE(Contains(text, "ignored")) << text;
    // Names the cause and BOTH ways out, not just the diagnosis.
    EXPECT_TRUE(Contains(text, "footprint")) << text;
    EXPECT_TRUE(Contains(text, "circle or rectangle")) << text;
    EXPECT_TRUE(Contains(text, "height offset")) << text;
}

// The regression this test exists for. The gather takes a stamp only when it is
// Enabled, so a DISABLED stamp is never refused and never warned about by the
// engine. Flagging it here would name a cause that is not why it is inactive,
// and prescribe a shape change that is not the fix — the effect's own Enabled
// toggle already says everything true about that state.
TEST(TerrainVolumeNotice, DisabledStampInAGlobalVolumeIsNotFlagged)
{
    World world;
    Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global);
    AddStamp(world, e, /*enabled=*/false);

    UIElement host;
    AddStampInGlobalVolumeNotice(&host, world, e.GetHandle());

    EXPECT_TRUE(AllTextIn(host).empty())
        << "a disabled stamp was flagged as ignored-because-Global; it is inactive because it "
           "is disabled, and re-enabling it is what the notice should then answer";
}

// The complement, so the test above cannot pass by the notice never firing:
// re-enabling the same stamp on the same entity brings the notice back.
TEST(TerrainVolumeNotice, ReEnablingTheStampBringsTheNoticeBack)
{
    World world;
    Entity e = MakeVolume(world, Components::TerrainVolumeShape::Global);
    AddStamp(world, e, /*enabled=*/false);

    UIElement off;
    AddStampInGlobalVolumeNotice(&off, world, e.GetHandle());
    ASSERT_TRUE(AllTextIn(off).empty());

    auto* stamp = world.GetComponentForWrite<Components::TerrainStampEffect>(e.GetHandle());
    ASSERT_NE(stamp, nullptr);
    stamp->Enabled = true;

    UIElement on;
    AddStampInGlobalVolumeNotice(&on, world, e.GetHandle());
    EXPECT_FALSE(AllTextIn(on).empty())
        << "the notice did not return when the stamp was re-enabled";
}

// A stamp is only refused because the volume is global; on any shape with a
// footprint it bakes, so there is nothing to say.
TEST(TerrainVolumeNotice, StampInANonGlobalVolumeIsNotFlagged)
{
    for (const auto shape : {Components::TerrainVolumeShape::Rectangle,
                             Components::TerrainVolumeShape::Circle,
                             Components::TerrainVolumeShape::SplineArea})
    {
        World world;
        Entity e = MakeVolume(world, shape);
        AddStamp(world, e, /*enabled=*/true);

        UIElement host;
        AddStampInGlobalVolumeNotice(&host, world, e.GetHandle());
        EXPECT_TRUE(AllTextIn(host).empty())
            << "shape " << static_cast<int>(shape) << " flagged a stamp that bakes fine";
    }
}

// An effect entity with no volume at all is the orphan case, which has its own
// notice. This one must stay quiet rather than blaming a shape that is absent.
TEST(TerrainVolumeNotice, StampWithNoVolumeIsNotFlagged)
{
    World world;
    Entity e = world.Create();
    Components::TerrainStampEffect stamp{};
    e.Set(stamp);
    world.ProcessCommands();

    UIElement host;
    AddStampInGlobalVolumeNotice(&host, world, e.GetHandle());
    EXPECT_TRUE(AllTextIn(host).empty());
}
