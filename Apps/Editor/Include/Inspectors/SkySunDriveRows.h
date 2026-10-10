#pragma once

#include <functional>
#include <memory>

namespace GameEngine
{
class UIElement;
struct InspectorContext;

// The sky inspector's drive set: whether the sky drives its sun light at all (the direction always),
// whether the light's illuminance comes from a custom curve of the sky's rather than from the light,
// and, under the curve only, whether the sky sets the light's colour (from the light it always does).
// Disabled, with the reason, while the sky has no light to drive. What the sky writes is the engine's
// (SkySunIlluminance::FieldsSkyDrives); this is presentation and the edits.
void AddSkySunDriveRows(UIElement* parent, const InspectorContext& ctx);

// Under the custom curve: the sun illuminance curve, on a logarithmic axis with the physical curve
// drawn behind it as a reference, and the action that replaces the curve with the physical one.
// `refreshTimeOfDayControls` and `curvePlaybackScrubActive` are the sky inspector's, shared by every
// day curve so scrubbing one moves the others' playheads.
void AddSkySunCurveRows(UIElement* parent, const InspectorContext& ctx, std::function<void()> refreshTimeOfDayControls,
                        std::shared_ptr<bool> curvePlaybackScrubActive);

// The moonlight in lux, in the sky inspector's moon rows.
void AddSkyMoonlightRow(UIElement* parent, const InspectorContext& ctx);

} // namespace GameEngine
