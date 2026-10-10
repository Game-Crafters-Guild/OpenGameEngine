#include "Placement/FenceSpanOverrideRemap.h"

#include "EditorChangeNotifications.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"

#include <algorithm>
#include <limits>

namespace GameEngine::Editor
{
namespace
{

using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

// The run a removed point's run folds into: the one opening at the nearest
// surviving point before it, wrapping round a closed loop, and how many spans
// stood before the removed point's run in the merged run — the spans of every
// run from that survivor up to it, as the fence laid them before the edit. An
// override keeps its place along the wall by adding them to its ordinal. False
// when an open spline has no survivor before it — its run is gone.
bool FoldedRun(const SplinePointRenumbering& renumbering, std::span<const uint32> runSpanCounts,
               uint32 removed, uint32& outPoint, uint32& outSpansBefore)
{
    const uint32 count = static_cast<uint32>(renumbering.OldToNew.size());
    outSpansBefore = 0;
    for (uint32 step = 1; step < count; ++step)
    {
        if (!renumbering.ClosedAfter && step > removed)
            return false;
        const uint32 before = (removed + count - step) % count;
        if (before < runSpanCounts.size())
            outSpansBefore += runSpanCounts[before];
        if (renumbering.OldToNew[before] != SplinePointRenumbering::kRemoved)
        {
            outPoint = renumbering.OldToNew[before];
            return true;
        }
    }
    return false;
}

// Whether the edit took out every point after `point` on an open spline, and
// with them the run `point` opened.
bool RunLostItsEnd(const SplinePointRenumbering& renumbering, uint32 point)
{
    if (renumbering.ClosedAfter)
        return false;
    for (uint32 after = point + 1u; after < renumbering.OldToNew.size(); ++after)
    {
        if (renumbering.OldToNew[after] != SplinePointRenumbering::kRemoved)
            return false;
    }
    return point + 1u < renumbering.OldToNew.size();
}

} // namespace

bool RemapSpanOverrides(Components::SplineFence& recipe, const SplinePointRenumbering& renumbering,
                        std::span<const uint32> runSpanCounts)
{
    const uint32 countBefore = static_cast<uint32>(renumbering.OldToNew.size());
    bool changed = false;
    for (SplineSpanOverride& entry : recipe.Overrides)
    {
        if (entry.Kind == SplineSpanOverrideKind::None || entry.PointIndex >= countBefore)
            continue;
        uint32 point = renumbering.OldToNew[entry.PointIndex];
        uint32 spansBefore = 0;
        const bool folded = point == SplinePointRenumbering::kRemoved &&
                            FoldedRun(renumbering, runSpanCounts, entry.PointIndex, point, spansBefore);
        const bool runGone = (point == SplinePointRenumbering::kRemoved && !folded) ||
                             (!folded && RunLostItsEnd(renumbering, entry.PointIndex));
        if (runGone)
        {
            entry = SplineSpanOverride{};
            changed = true;
            continue;
        }
        if (folded && spansBefore > 0u)
        {
            entry.SpanOrdinal = static_cast<uint16>(
                std::min<uint32>(entry.SpanOrdinal + spansBefore, std::numeric_limits<uint16>::max()));
            changed = true;
        }
        if (point != entry.PointIndex)
        {
            entry.PointIndex = point;
            changed = true;
        }
    }
    return changed;
}

void RemapFenceOverridesOnPointEdit(const SplinePointEdit& edit, std::span<const uint32> runSpanCounts)
{
    if (!edit.World || !edit.Renumbering)
        return;
    const auto* fence = edit.World->GetComponent<Components::SplineFence>(edit.Entity);
    if (!fence)
        return;
    Components::SplineFence remapped = *fence;
    if (!RemapSpanOverrides(remapped, *edit.Renumbering, runSpanCounts))
        return;

    const auto typeId = ECS::GetComponentTypeId<Components::SplineFence>();
    if (edit.Undo)
    {
        UndoRedoService::InteractiveEdit record = edit.Undo->BeginInteractiveEdit(
            "Re-address Span Overrides",
            MultiEntityUndo::MakeComponentSnapshotTarget(edit.World, {edit.Entity}, typeId,
                                                         edit.Notifications,
                                                         "Re-address Span Overrides"));
        edit.World->AddComponentImmediate(edit.Entity, remapped);
        record.Commit();
        return;
    }
    edit.World->AddComponentImmediate(edit.Entity, remapped);
    if (edit.Notifications)
        edit.Notifications->NotifyComponentChange<Components::SplineFence>(
            edit.World, edit.Entity, EditorChangeNotifications::ChangeKind::Commit);
}

} // namespace GameEngine::Editor
