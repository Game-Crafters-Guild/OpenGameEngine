#pragma once

#include "ECS/Entity.h"
#include "Events/Event.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::Editor
{

class EditorChangeNotifications;
class UndoRedoService;

// How an edit renumbered a spline's authored points: for every point before
// the edit, its index after it. Data addressed by authored point — a fence's
// span overrides — is re-addressed by it.
struct SplinePointRenumbering
{
    // A point the edit took out. What its data becomes is its owner's call: a
    // fence folds the removed point's run into the run before it.
    static constexpr uint32 kRemoved = ~0u;

    std::vector<uint32> OldToNew;
    uint32 PointCountAfter = 0;
    bool ClosedAfter = false;

    // `insertedCount` points inserted at `index` of a spline of `countBefore`
    // points: every point at or after `index` moves up by that many.
    static SplinePointRenumbering Inserted(uint32 countBefore, uint32 index, uint32 insertedCount,
                                           bool closedAfter);
    // The points at `removed` — indices before the edit, in any order — taken
    // out; the survivors close up in order.
    static SplinePointRenumbering Removed(uint32 countBefore, std::span<const uint32> removed,
                                          bool closedAfter);
    // A rewrite of the whole point list — a resample: every point before it maps
    // to the point after it nearest in position, which is where data addressed
    // by that point survives (fence design section 5).
    static SplinePointRenumbering Nearest(std::span<const Mathematics::Vector3> before,
                                          std::span<const Mathematics::Vector3> after, bool closedAfter);
};

// One renumbering edit, handed to every subscriber after the spline's points
// changed, with the undo service the edit is recorded in. A subscriber that
// re-addresses its data records that change there, and the editing site wraps
// the point edit and every such change in one compound step, so one undo
// restores the points and the data addressed by them together.
struct SplinePointEdit
{
    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    UndoRedoService* Undo = nullptr;
    EditorChangeNotifications* Notifications = nullptr;
    const SplinePointRenumbering* Renumbering = nullptr;
};

// The editor's broadcast for edits that renumber a spline's authored points.
// Every site that inserts, removes or rewrites them invokes it; the recipes
// whose data is addressed by point subscribe, so the spline tools never learn
// which recipes those are. Editor main thread only, like the edits themselves.
Event<const SplinePointEdit&>& SplinePointEdited();

} // namespace GameEngine::Editor
