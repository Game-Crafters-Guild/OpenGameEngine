#pragma once

namespace GameEngine
{
class UIElement;
struct InspectorContext;

// The sky inspector's sun path rows, placed under its Sun heading: the Sun path choice, then the
// active path's fields (Latitude, Day of year and North for Earth; Axis heading, Axis altitude and
// Height at noon for Custom), each edited on the sky as one undo step with its meaning as a caption,
// and "Over the day", a read-only graph of what the physical sun delivers from midnight to midnight
// (the readout card beneath it is SkySunReadoutCard's). The path and the illuminance come from
// Components::SkySunPath, the functions the sky system uses; this is presentation.
void AddSkySunPathRows(UIElement* parent, const InspectorContext& ctx);

} // namespace GameEngine
