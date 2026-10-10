#pragma once

#include "Components/Spline/SplineFence.h"
#include "UndoRedo/SplinePointEdits.h"

#include <span>

namespace GameEngine::Editor
{

// Re-addresses a fence's span overrides after its spline's authored points
// were renumbered, so each keeps naming the span it named (fence design
// section 5):
// - a surviving point takes its new index, which is all an insert or a
//   resample does (a resample maps every old point to the nearest new one);
// - a removed point's run is folded into the run before it — the run opening
//   at the nearest surviving point before it, wrapping round a closed loop —
//   and the override's ordinal grows by the spans that stood before its run in
//   the merged run (`runSpanCounts`, the fence's last layout, per run by the
//   authored point opening it), so it names the same place along the wall; the
//   merged run is filled anew, and a span past its fill is reported by the
//   layout, never dropped;
// - on an open spline, removing the first point takes its run away, and the
//   overrides on that run go with it, as the wall they named did; removing the
//   last point does the same to the run that closed on it.
// An override naming a point the spline did not have before the edit is left
// as it was: it was already reported, and the edit did not touch it.
// Returns whether any override changed.
bool RemapSpanOverrides(Components::SplineFence& recipe, const SplinePointRenumbering& renumbering,
                        std::span<const uint32> runSpanCounts);

// The fence's subscriber to SplinePointEdited: remaps the SplineFence on the
// edited spline entity, if it has one and anything changes, and records the
// change in the edit's undo service so it undoes with the points.
void RemapFenceOverridesOnPointEdit(const SplinePointEdit& edit, std::span<const uint32> runSpanCounts);

} // namespace GameEngine::Editor
