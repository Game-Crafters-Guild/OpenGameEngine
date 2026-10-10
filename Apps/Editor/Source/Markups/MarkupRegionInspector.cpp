#include "Markups/MarkupRegionInspector.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/ConvertMarkupShapeCommand.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupInspectorRows.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRegionPort.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using Components::MarkupMemberMode;
using Components::MarkupRegion;
using Components::MarkupRegionMember;

// Areas from this many square meters up read in hectares.
constexpr float32 kHectare = 10000.0f;

std::string FormatArea(float32 squareMeters)
{
    char text[32];
    if (squareMeters >= kHectare)
        std::snprintf(text, sizeof(text), "%.2f ha", squareMeters / kHectare);
    else
        std::snprintf(text, sizeof(text), "%.0f m\xC2\xB2", squareMeters);
    return text;
}

using Edit = UndoRedoService::InteractiveEdit;

// The height row's slider and the drag it holds (empty between drags).
struct HeightRow
{
    Slider* Control = nullptr;
    FloatField* Value = nullptr;
    std::shared_ptr<Edit> Drag;
};

// The height: previewed on the region every frame of a drag, one undo step on release.
HeightRow AddHeightRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const MarkupRegion& region)
{
    auto edit = std::make_shared<Edit>();
    const auto write = [ctx](float32 height) {
        if (MarkupRegion* edited = ctx.World->GetComponentForWrite<MarkupRegion>(ctx.Entity))
            edited->ExtrudeHeight = std::clamp(height, kMinExtrudeHeight, kMaxExtrudeHeight);
    };
    const auto changing = [ctx, &bridge, edit, write](float height) {
        if (bridge.IsInPlayMode())
            return;
        if (!*edit && ctx.Undo)
            *edit = ctx.Undo->BeginInteractiveEdit(
                "Set Region Height", InspectorDrag::MakeComponentSnapshotTarget<MarkupRegion>(
                                         ctx.World, ctx.Entity, ctx.ChangeNotifications, "Region Height"));
        if (*edit)
            edit->Preview([&] { write(height); });
    };
    const auto changed = [ctx, &bridge, edit, write](float height) {
        if (bridge.IsInPlayMode())
            return;
        if (*edit)
        {
            edit->Preview([&] { write(height); });
            edit->Commit();
            *edit = Edit{};
            return;
        }
        InspectorDrag::CommitComponentWithUndo<MarkupRegion>(
            ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Set Region Height",
            [height](MarkupRegion& edited) {
                edited.ExtrudeHeight = std::clamp(height, kMinExtrudeHeight, kMaxExtrudeHeight);
            });
    };
    const InspectorDrag::SliderWithFloatValueRow row = InspectorDrag::AddFloatSliderRow(
        ctx.Parent, "Height", region.ExtrudeHeight, kMinExtrudeHeight, kMaxExtrudeHeight, changing, changed,
        "How tall the walls stand above the ground, in meters", "m");
    row.Slider->AddClass("markup-region-height");
    // A drag lands between whole meters; the field reads it to a tenth.
    row.ValueField->SetFixedDecimalPlaces(1);
    if (bridge.IsInPlayMode())
        InspectorUI::SetRowOfControlEnabled(row.Slider, false);
    return HeightRow{row.Slider, row.ValueField, edit};
}

// The outline's knot count, perimeter and area (members not applied: the base outline's).
void AddOutlineRows(const InspectorContext& ctx)
{
    const auto* spline = ctx.World->GetComponent<Components::SplineComponent>(ctx.Entity);
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    const Spline::SplineData* data =
        spline && splines
            ? splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration))
            : nullptr;
    AddValueRow(ctx.Parent, "Points", std::to_string(data ? data->Points.size() : 0u),
                "The outline's knots; Edit outline moves, adds and deletes them");
    const std::optional<MarkupECS::MarkupFootprint> footprint = MarkupECS::ReadMarkupFootprint(*ctx.World, ctx.Entity);
    if (!footprint || footprint->IsCircle())
        return;
    AddValueRow(ctx.Parent, "Perimeter", FormatMeters(MarkupECS::RegionOutlinePerimeter(footprint->Ring)),
                "The outline's length on the ground plane");
    AddValueRow(ctx.Parent, "Outline area", FormatArea(MarkupECS::RegionOutlineArea(footprint->Ring)),
                "The area the outline encloses on the ground plane, before its members apply");
}

// The region's members as they are now.
std::vector<MarkupRegionMember> CurrentMembers(const MarkupRegion& region)
{
    return std::vector<MarkupRegionMember>(region.Members,
                                           region.Members + std::min(region.MemberCount, Components::kMaxRegionMembers));
}

// Writes `members` as the region's list, one undo step.
void CommitMembers(const InspectorContext& ctx, const std::vector<MarkupRegionMember>& members, const char* label)
{
    InspectorDrag::CommitComponentWithUndo<MarkupRegion>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, label, [&members](MarkupRegion& edited) {
            edited.MemberCount = static_cast<uint32>(std::min<std::size_t>(members.size(), Components::kMaxRegionMembers));
            std::fill(std::begin(edited.Members), std::end(edited.Members), MarkupRegionMember{});
            std::copy_n(members.begin(), edited.MemberCount, edited.Members);
        });
    if (ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

bool BoundsOverlap(const MarkupECS::MarkupFootprint& a, const MarkupECS::MarkupFootprint& b)
{
    return a.Min.x <= b.Max.x && b.Min.x <= a.Max.x && a.Min.y <= b.Max.y && b.Min.y <= a.Max.y;
}

// One mark-up the list offers: a member now, or one whose footprint overlaps the region.
struct MemberCandidate
{
    ECS::EntityHandle Entity{};
    std::optional<std::size_t> MemberIndex; // its place in the list when it is a member
    bool Live = true;                       // false for a deleted member (undo revives it)
    bool Overlaps = true;
};

std::vector<MemberCandidate> CollectCandidates(const InspectorContext& ctx, const MarkupRegion& region)
{
    std::vector<MemberCandidate> candidates;
    const std::vector<MarkupRegionMember> members = CurrentMembers(region);
    const std::optional<MarkupECS::MarkupFootprint> base = MarkupECS::ReadMarkupFootprint(*ctx.World, ctx.Entity);
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        MemberCandidate& candidate = candidates.emplace_back();
        candidate.Entity = members[i].Entity;
        candidate.MemberIndex = i;
        const std::optional<MarkupECS::MarkupFootprint> footprint =
            MarkupECS::ReadMarkupFootprint(*ctx.World, members[i].Entity);
        candidate.Live = footprint.has_value();
        candidate.Overlaps = footprint && base && BoundsOverlap(*base, *footprint);
    }
    if (!base)
        return candidates;
    ctx.World->Query<ECS::Read<Components::Markup>>().Each(
        [&](ECS::EntityHandle entity, const Components::Markup&) {
            if (entity == ctx.Entity ||
                std::any_of(members.begin(), members.end(),
                            [entity](const MarkupRegionMember& member) { return member.Entity == entity; }))
                return;
            const std::optional<MarkupECS::MarkupFootprint> footprint = MarkupECS::ReadMarkupFootprint(*ctx.World, entity);
            if (footprint && BoundsOverlap(*base, *footprint))
                candidates.push_back(MemberCandidate{entity, std::nullopt, true, true});
        });
    return candidates;
}

// What the row says beside the title: a deleted member, one that no longer overlaps, a region
// member's base outline rule.
std::string MemberNote(const InspectorContext& ctx, const MemberCandidate& candidate)
{
    if (!candidate.Live)
        return "deleted (undo restores it)";
    if (!candidate.Overlaps)
        return "outside the region";
    if (const auto* memberRegion = ctx.World->GetComponent<MarkupRegion>(candidate.Entity);
        memberRegion && memberRegion->MemberCount > 0)
        return "base outline only: its own " + std::to_string(memberRegion->MemberCount) + " members do not apply here";
    if (ctx.World->GetComponent<MarkupRegion>(candidate.Entity))
        return "base outline only";
    return {};
}

// One segment of the member's Include | Exclude control, filled when it is the member's mode; a click
// on the other segment switches the mode, one undo step.
std::unique_ptr<Button> BuildMemberModeSegment(const InspectorContext& ctx, const std::vector<MarkupRegionMember>& members,
                                               const MemberCandidate& candidate, MarkupMemberMode segmentMode,
                                               bool enabled)
{
    const bool exclude = segmentMode == MarkupMemberMode::Exclude;
    auto segment = std::make_unique<Button>();
    segment->AddClass("small");
    segment->AddClass(exclude ? "markup-member-mode-exclude" : "markup-member-mode-include");
    segment->SetText(exclude ? "Exclude" : "Include");
    segment->SetTooltip(exclude ? "Exclude cuts its footprint out of the region's area"
                                : "Include adds its footprint to the region's area");
    if (candidate.MemberIndex && members[*candidate.MemberIndex].Mode == segmentMode)
        segment->AddClass("active");
    segment->SetEnabled(enabled);
    segment->RegisterEventHandler(kEventButtonClick, [ctx, members, candidate, segmentMode](UIEvent&) {
        if (!candidate.MemberIndex || members[*candidate.MemberIndex].Mode == segmentMode)
            return;
        std::vector<MarkupRegionMember> edited = members;
        edited[*candidate.MemberIndex].Mode = segmentMode;
        CommitMembers(ctx, edited, "Switch Region Member");
    });
    return segment;
}

// The member's mode as two segments, Include | Exclude; a candidate that is not a member shows
// neither filled and cannot set one.
std::unique_ptr<UIElement> BuildMemberModeControl(const InspectorContext& ctx,
                                                  const std::vector<MarkupRegionMember>& members,
                                                  const MemberCandidate& candidate, bool enabled)
{
    auto control = std::make_unique<UIElement>();
    control->AddClass("markup-member-mode");
    control->AddChild(BuildMemberModeSegment(ctx, members, candidate, MarkupMemberMode::Include, enabled));
    control->AddChild(BuildMemberModeSegment(ctx, members, candidate, MarkupMemberMode::Exclude, enabled));
    return control;
}

void AddMemberRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, UIElement& list, const MarkupRegion& region,
                  const MemberCandidate& candidate)
{
    const std::vector<MarkupRegionMember> members = CurrentMembers(region);
    const bool member = candidate.MemberIndex.has_value();
    auto row = std::make_unique<UIElement>();
    row->AddClass("markup-member-row");
    row->AddClass(member ? "markup-member-in" : "markup-member-out");

    auto toggle = std::make_unique<Toggle>();
    toggle->AddClass("markup-member-toggle");
    toggle->SetValueWithoutNotify(member);
    toggle->SetTooltip(member ? "A member: uncheck to take it out of the region's area"
                              : "Check to make it a member of the region's area");
    const bool full = !member && members.size() >= Components::kMaxRegionMembers;
    if (full || bridge.IsInPlayMode())
    {
        toggle->SetEnabled(false);
        if (full)
            toggle->SetTooltip("A region holds at most 32 members; take one out first");
    }
    toggle->SetOnValueChanged([ctx, members, candidate](const bool& checked) {
        std::vector<MarkupRegionMember> edited = members;
        if (checked && !candidate.MemberIndex)
            edited.push_back({candidate.Entity, MarkupMemberMode::Include});
        else if (!checked && candidate.MemberIndex)
            edited.erase(edited.begin() + static_cast<std::ptrdiff_t>(*candidate.MemberIndex));
        else
            return;
        CommitMembers(ctx, edited, checked ? "Add Region Member" : "Remove Region Member");
    });
    row->AddChild(std::move(toggle));

    row->AddChild(BuildMarkupKindGlyph(candidate.Live ? ReadMarkupKind(*ctx.World, candidate.Entity) : MarkupKind::None));
    auto title = std::make_unique<Label>();
    title->AddClass("markup-member-title");
    if (!candidate.Live)
        title->AddClass("markup-member-title-deleted");
    else if (!member)
        title->AddClass("markup-member-title-out");
    title->SetText(candidate.Live ? MarkupTitle(*ctx.World, candidate.Entity) : std::string("Deleted mark-up"));
    if (candidate.Live)
    {
        title->SetTooltip("Click to select it");
        title->RegisterEventHandler(kEventMouseUp, [ctx, &bridge, entity = candidate.Entity](UIEvent& event) {
            if (event.Button == 0)
                bridge.SelectMarkup(*ctx.World, entity, false);
        });
    }
    row->AddChild(std::move(title));
    if (const std::string note = MemberNote(ctx, candidate); !note.empty())
    {
        auto noteLabel = std::make_unique<Label>();
        noteLabel->AddClass("markup-member-note");
        noteLabel->SetText(note);
        row->AddChild(std::move(noteLabel));
    }

    row->AddChild(BuildMemberModeControl(ctx, members, candidate, member && !bridge.IsInPlayMode()));
    list.AddChild(std::move(row));
}

// The member list under its title: the members first, in their order, then the other mark-ups
// that overlap the region.
void AddMemberList(const InspectorContext& ctx, MarkupEditorBridge& bridge, const MarkupRegion& region)
{
    auto group = std::make_unique<UIElement>();
    group->AddClass("markup-member-group");
    auto heading = std::make_unique<Label>();
    heading->AddClass("markup-member-heading");
    heading->SetText("Members");
    heading->SetTooltip("Inside the region = its outline or an Include member, and not an Exclude member");
    group->AddChild(std::move(heading));
    auto list = std::make_unique<UIElement>();
    list->AddClass("markup-member-list");
    const std::vector<MemberCandidate> candidates = CollectCandidates(ctx, region);
    for (const MemberCandidate& candidate : candidates)
        AddMemberRow(ctx, bridge, *list, region, candidate);
    if (candidates.empty())
    {
        auto empty = std::make_unique<Label>();
        empty->AddClass("markup-member-empty");
        empty->SetText("No other mark-up overlaps this region");
        list->AddChild(std::move(empty));
    }
    group->AddChild(std::move(list));
    ctx.Parent->AddChild(std::move(group));
}

// The Height row follows the region's height while no drag holds it.
std::function<void()> FollowHeight(const InspectorContext& ctx, const HeightRow& height)
{
    return [world = ctx.World, entity = ctx.Entity, height]() {
        if (const auto* region = world->GetComponent<MarkupRegion>(entity); region && !*height.Drag)
        {
            height.Control->SetValueWithoutNotify(region->ExtrudeHeight);
            height.Value->SetValueWithoutNotify(region->ExtrudeHeight);
        }
    };
}

} // namespace

void AddMarkupRegionBlock(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;
    const MarkupRegion* region = ctx.World->GetComponent<MarkupRegion>(ctx.Entity);
    if (!region)
        return;
    auto title = std::make_unique<Label>();
    title->AddClass("markup-region-title");
    title->SetText("Region");
    ctx.Parent->AddChild(std::move(title));
    if (const RegionOutlineProblem problem = ReadRegionOutlineProblem(*ctx.World, ctx.Entity);
        problem != RegionOutlineProblem::None)
        ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
            RegionOutlineProblemFix(problem), EditorUI::InspectorNotice::Kind::Warning));
    const HeightRow height = AddHeightRow(ctx, bridge, *region);
    AddOutlineRows(ctx);
    AddEditKnotsRow(ctx, bridge, "Edit outline", "markup-region-edit-outline",
                    "Drag, add and delete the outline's points in the Scene View");
    AddMemberList(ctx, bridge, *region);
    FollowMarkupShapeChanges(ctx, FollowHeight(ctx, height));
}

bool ConvertBoxMarkupToRegion(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                              EditorChangeNotifications* notifications)
{
    std::optional<BoxRegionShape> box = BoxAsRegion(world, entity);
    if (!box)
        return false;
    auto command = std::make_unique<ConvertMarkupShapeCommand>(
        kConvertToRegionLabel, world, entity, notifications,
        ConvertedRegion{std::move(box->Knots), Spline::SplineType::Linear, box->ExtrudeHeight});
    if (undo)
        undo->Execute(std::move(command));
    else
        command->Do();
    return true;
}

void AddConvertToRegionRow(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    const auto* volume = ctx.World->GetComponent<Components::MarkupVolume>(ctx.Entity);
    if (!volume || volume->Shape != Components::MarkupVolumeShape::Box)
        return;
    UIElement* cell = InspectorUI::AddActionRow(ctx.Parent, "markup-region-actions");
    auto button = std::make_unique<Button>();
    button->SetText("Convert to region");
    button->AddClass("small");
    button->AddClass("markup-convert-to-region");
    button->SetTooltip("Make this box a region with the box's outline and height; its notes and thread stay");
    button->RegisterEventHandler(kEventButtonClick, [ctx, &bridge](UIEvent&) {
        if (bridge.IsInPlayMode())
            return;
        if (ConvertBoxMarkupToRegion(*ctx.World, ctx.Entity, ctx.Undo, ctx.ChangeNotifications) &&
            ctx.RequestInspectorRefresh)
            ctx.RequestInspectorRefresh();
    });
    Button* raw = button.get();
    cell->AddChild(std::move(button));
    if (bridge.IsInPlayMode())
        InspectorUI::SetRowOfControlEnabled(raw, false);
}

void RegisterMarkupRegionTraits()
{
    EditorComponentTraits traits;
    const ECS::ComponentTypeId type = ECS::GetComponentTypeId<MarkupRegion>();
    EditorComponentTraitsRegistry::Get().TryGet(type, traits);
    traits.DisplayName = "Mark-up Region";
    // The Mark-up section holds the Region block (AddMarkupRegionBlock).
    traits.HideInInspector = true;
    // A region's entity is placed at its label point (NewRegionPlacement): the move gizmo pivots
    // at the entity, inside the region, not at the spline's first knot on its edge.
    traits.GizmoPivotAtEntityOrigin = true;
    EditorComponentTraitsRegistry::Get().Register(type, std::move(traits));
}

} // namespace GameEngine::Editor
