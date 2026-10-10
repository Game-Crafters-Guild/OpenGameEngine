#include "Inspectors/SplineFenceSpanPieceInspector.h"

#include "Editor/Entities/EditorComponentTraits.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SplineFenceOverrideFields.h"
#include "Placement/FenceSpanOverrides.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Spline/SplineFence.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "UI/Controls/Dropdown.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::SplineFence;
using Components::SplineFenceSpanPiece;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

const char* UndoNameFor(SplineSpanOverrideKind kind)
{
    switch (kind)
    {
    case SplineSpanOverrideKind::Gate:
        return "Make Span a Gate";
    case SplineSpanOverrideKind::ExplicitPiece:
        return "Set Explicit Span Piece";
    case SplineSpanOverrideKind::None:
    default:
        return "Clear Span Override";
    }
}

// What the span becomes when the author picks a kind: a gate keeps the gate
// piece it had, or takes the first; an explicit piece keeps the piece the span
// draws now, so choosing it changes nothing on screen until another piece is
// chosen.
uint8 SlotForKind(SplineSpanOverrideKind kind, const SplineSpanOverride* current,
                  const SplineFenceSpanPiece& piece)
{
    if (current && current->Kind == kind)
        return current->PoolSlot;
    if (kind == SplineSpanOverrideKind::ExplicitPiece && !piece.IsGate)
        return static_cast<uint8>(std::min<uint32>(piece.PoolSlot, Components::kSplineFencePoolCapacity - 1u));
    return 0u;
}

// The fence rebuilds a picked piece in place: a wall made a gate keeps its
// entity and takes the gate's name. The header shows the name the panel was
// built with, so the section asks for one rebuild when the name moves on.
void RefreshWhenTheNameChanges(const InspectorContext& ctx)
{
    if (!ctx.FrameRefreshCallbacks || !ctx.RequestInspectorRefresh || !ctx.World)
        return;
    const auto* name = ctx.World->GetComponent<Components::Name>(ctx.Entity);
    auto shown = std::make_shared<std::string>(name ? name->View() : std::string_view());
    auto requested = std::make_shared<bool>(false);
    UIElement* host = ctx.Parent;
    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    auto refresh = ctx.RequestInspectorRefresh;
    ctx.FrameRefreshCallbacks->push_back(
        [world, entity, shown, requested, host, refresh]()
        {
            if (*requested || !host || !world->IsValid(entity))
                return;
            const auto* current = world->GetComponent<Components::Name>(entity);
            if (!current || *shown == current->View())
                return;
            *requested = true;
            host->PostAction(refresh);
        });
}

void BuildSpanPieceSection(const InspectorContext& ctx)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle pieceEntity = ctx.Entity;
    const auto* piece = w ? w->GetComponent<SplineFenceSpanPiece>(pieceEntity) : nullptr;
    if (!piece)
        return;
    const auto* parent = w->GetComponent<Components::Parent>(pieceEntity);
    const ECS::EntityHandle fenceEntity = parent ? parent->parent : ECS::EntityHandle{};
    const SplineFence* fence =
        fenceEntity.IsValid() ? w->GetComponent<SplineFence>(fenceEntity) : nullptr;
    if (!fence)
    {
        InspectorUI::AddInfoCard(ctx.Parent, "This piece is no longer under a fence recipe.");
        return;
    }
    RefreshWhenTheNameChanges(ctx);

    InspectorUI::AddLine(ctx.Parent, "Span " + std::to_string(piece->OrdinalInRun) +
                                         " of the run from point " + std::to_string(piece->Run) +
                                         ". What it uses is saved on the fence and survives every "
                                         "rebuild; the rest of this piece is rebuilt from the fence.");

    const SplineSpanOverride* current =
        Editor::FindSpanOverride(*fence, piece->Run, piece->OrdinalInRun);
    const SplineSpanOverrideKind kind = current ? current->Kind : SplineSpanOverrideKind::None;

    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const SplineFenceSpanPiece address = *piece;
    const bool tableFull = !current && Editor::CountSpanOverrides(*fence) >=
                                           Components::kSplineFenceMaxSpanOverrides;

    Dropdown* kindField = InspectorUI::AddDropdownRow(
        ctx.Parent, "Use", Editor::SpanOverrideKindOptions("Span Pool"), Editor::SpanOverrideKindIndex(kind),
        "What this span draws. Span Pool takes the fence's own pick from the Span pool; Gate draws "
        "from the Gate pool and clears the crest pieces over it; Explicit Piece keeps the chosen "
        "Span pool piece here whatever the seed picks");
    kindField->SetOnValueChanged(
        [w, fenceEntity, n, undo, address](const std::string& value)
        {
            const auto nextKind = Editor::SpanOverrideKindOfOption(value);
            const auto* recipe = w->GetComponent<SplineFence>(fenceEntity);
            if (!nextKind || !recipe)
                return;
            const SplineSpanOverride* existing =
                Editor::FindSpanOverride(*recipe, address.Run, address.OrdinalInRun);
            const uint8 slot = SlotForKind(*nextKind, existing, address);
            InspectorDrag::CommitComponentWithUndo<SplineFence>(
                w, fenceEntity, n, undo, UndoNameFor(*nextKind),
                [address, kind = *nextKind, slot](SplineFence& updated)
                { Editor::SetSpanOverride(updated, address.Run, address.OrdinalInRun, kind, slot); });
        });
    if (tableFull)
    {
        kindField->SetDisabled(true);
        InspectorUI::AddInfoCard(
            ctx.Parent, "The fence already holds " +
                            std::to_string(Components::kSplineFenceMaxSpanOverrides) +
                            " span overrides; remove one in the fence's Span Overrides to set "
                            "another here.");
    }

    if (kind == SplineSpanOverrideKind::None)
    {
        if (Components::ActivePoolCount(fence->GatePool) == 0u)
        {
            InspectorUI::AddInfoCard(ctx.Parent,
                                     "The fence's Gate pool is empty: a Gate here would leave an "
                                     "opening, which cannot be selected in the scene view again.");
        }
        return;
    }

    const bool gate = kind == SplineSpanOverrideKind::Gate;
    Editor::AddPoolPieceRow(
        ctx.Parent, gate ? "Gate Piece" : "Span Piece", gate ? fence->GatePool : fence->SpanPool,
        current->PoolSlot, gate ? "Gate" : "Span",
        gate ? "Which Gate pool piece stands in this span" : "Which Span pool piece this span keeps",
        [w, fenceEntity, n, undo, address, kind](uint8 slot)
        {
            InspectorDrag::CommitComponentWithUndo<SplineFence>(
                w, fenceEntity, n, undo,
                kind == SplineSpanOverrideKind::Gate ? "Change Gate Piece" : "Change Explicit Span Piece",
                [address, kind, slot](SplineFence& updated)
                { Editor::SetSpanOverride(updated, address.Run, address.OrdinalInRun, kind, slot); });
        });
    for (const std::string& note : Editor::SpanOverrideNotes(*fence, *current, /*spanStands=*/true))
        InspectorUI::AddInfoCard(ctx.Parent, note);
}

} // namespace

void RegisterSplineFenceSpanPieceInspector()
{
    // The section is the one thing on a generated piece an author edits, so it
    // leads the inspector rather than sitting under the long Material section.
    Editor::EditorComponentTraits traits;
    traits.LeadsInspector = true;
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<SplineFenceSpanPiece>(),
                                                          std::move(traits));
    InspectorRegistry::Get().RegisterComponentInspector<SplineFenceSpanPiece>(
        [](const InspectorContext& ctx) { BuildSpanPieceSection(ctx); });
}

} // namespace GameEngine
