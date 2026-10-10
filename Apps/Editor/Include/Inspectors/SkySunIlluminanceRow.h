#pragma once

#include "ECS/ECS.h"

#include <functional>

namespace GameEngine
{
class UIElement;
struct InspectorContext;

// The sky inspector's "Sun illuminance" row: the linked sun light's illuminance with the sun
// overhead, in lux, edited on that light as one undo step, with a hint beneath it when that sun is
// far dimmer than a clear one. With no light linked it shows the sun the sky reads, read-only, and a
// Link action that calls `linkSun` with that light. Resolution and unit conversion are
// SkySunIlluminance's; this is presentation.
void AddSkySunIlluminanceRow(UIElement* parent, const InspectorContext& ctx,
                             std::function<void(ECS::EntityHandle)> linkSun);

} // namespace GameEngine
