#pragma once

#include <optional>

namespace GameEngine
{
/// What decides the AI Assistant panel's mode control minimum on its control line, in
/// logical px (PromptPanel::FitModeControl measures them).
struct ModeControlFit
{
    /// The control showing the longest label it offers now, whole.
    float WholeLabelPx = 0.0f;
    /// The control showing the longest of its compact labels ("Chat", "Auto"), whole.
    float CompactLabelPx = 0.0f;
    /// The line's controls that keep their width (Session…, Send) and the margins between
    /// all of them.
    float OthersPx = 0.0f;
    /// The line's inner width.
    float AvailablePx = 0.0f;
    /// The model button's minimum width.
    float ModelMinimumPx = 0.0f;
    /// The connection showing the first word of its name.
    float ConnectionReadablePx = 0.0f;
    /// The controls sit on a line of their own under the field; beside it they never shrink.
    bool Stacked = false;
};

/// How the mode control shows on its line.
struct ModeControlLayout
{
    /// The control's minimum width in logical px; nullopt leaves the stylesheet's minimum.
    std::optional<float> MinimumPx;
    /// The control shows its compact labels.
    bool Compact = false;
};

/// The mode control's labels and minimum width: its whole label while the line still leaves
/// the model button its minimum and the connection its first word (always, beside the
/// field); on a narrower line its compact labels, whole while they fit; on a line too narrow
/// for those, the compact labels at the stylesheet's minimum so the connection stays readable.
ModeControlLayout ModeControlLayoutFor(const ModeControlFit& fit);
} // namespace GameEngine
