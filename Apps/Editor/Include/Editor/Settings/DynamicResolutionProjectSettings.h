#pragma once

namespace GameEngine::Editor
{

// Storage contract for the project's dynamic-resolution target
// (rendering.drsTargetFps in ProjectSettings.json).
//
// The settings widget and every loader share this one range. A widget narrower
// than the stored range is not a cosmetic mismatch: the row seeds itself from
// the stored value clamped to the widget, so the page then displays a value the
// file does not hold — and any write-back persists the clamp over the user's
// setting.
inline constexpr float kMinDrsTargetFps = 15.0f;
inline constexpr float kMaxDrsTargetFps = 240.0f;
inline constexpr float kDefaultDrsTargetFps = 60.0f;

} // namespace GameEngine::Editor
