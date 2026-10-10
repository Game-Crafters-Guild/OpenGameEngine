#pragma once

#include "Components/Rendering/SkyEnvironment.h" // SkySunIlluminanceSource
#include "ECS/Entity.h"

#include <vector>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// Switch where the sun light's illuminance comes from, for the sky on `primary` and every sky in
// `extras`, as one undo step, and rebuild their inspectors. A stored curve is never replaced: the
// first switch to the curve, while the curve is still the untouched default, seeds it from the
// physical curve for the sky's path and its light's illuminance (SkySunDrive::SeedCurveIfUnauthored),
// in the same undo step, so a first use looks the same as the light at every hour. The fields that
// start being driven are recorded for undo (SkySunLinkUndo).
void SwitchSkySunIlluminanceSource(ECS::World* world, ECS::EntityHandle primary,
                                   const std::vector<ECS::EntityHandle>& extras,
                                   EditorChangeNotifications* notifications, UndoRedoService* undo,
                                   Components::SkySunIlluminanceSource source);

// Replace the sun illuminance curve of the sky on `primary` and every sky in `extras` with the
// physical curve for its path and date for a clear sun (kClearNoonSunIlluminanceLux), the reference
// line the inspector draws behind the curve, as one undo step named "Replace Sun Illuminance Curve",
// and rebuild their inspectors. The source stays as it is.
void ReplaceSkySunIlluminanceCurve(ECS::World* world, ECS::EntityHandle primary,
                                   const std::vector<ECS::EntityHandle>& extras,
                                   EditorChangeNotifications* notifications, UndoRedoService* undo);

} // namespace GameEngine::Editor
