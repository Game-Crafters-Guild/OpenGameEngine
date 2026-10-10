#pragma once

#include "Mathematics/Vector3.h"

#include <vector>

namespace GameEngine::Editor::SceneTools
{

// The freehand stroke the spline tool's brush and the mark-up tool's lasso both capture: a sample
// is kept once the pointer is `minSpacing` meters from the last one (1 m by default for both),
// and the release point ends the stroke, so a loop released next to its start can close.

// Appends `point` to `stroke` when the stroke is empty or the point is at least `minSpacing` from
// its last sample; true when appended.
bool CaptureSplineStroke(std::vector<Mathematics::Vector3>& stroke, const Mathematics::Vector3& point,
                         float minSpacing);
// Ends a started stroke at `release`, appended unless it repeats the last sample: the spacing
// rule above drops the release point, which close-loop detection needs.
void EndSplineStroke(std::vector<Mathematics::Vector3>& stroke, const Mathematics::Vector3& release);

} // namespace GameEngine::Editor::SceneTools
