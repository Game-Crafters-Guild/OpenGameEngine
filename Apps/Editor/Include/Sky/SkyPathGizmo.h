#pragma once

namespace GameEngine::Editor
{

// Registers the Sky Environment's Scene View overlay with the component gizmo registry: while a
// physical sky is selected or hovered, a dome around it shows the sun's path for the day, drawn from
// the sky's own fields. A horizon ring with a tick where north (or the Custom axis) points, the axis
// the sun circles, the day's circle (bright above the horizon, dim below), a ring at noon, rings at
// sunrise and sunset on a day that has them (SkySunDayKind.h), and a dot where the sun is now.
// Read-only: the fields are edited in the inspector.
void RegisterSkyPathGizmo();

} // namespace GameEngine::Editor
