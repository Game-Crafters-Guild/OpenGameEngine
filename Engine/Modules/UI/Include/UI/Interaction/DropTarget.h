#pragma once

#include "UI/Interaction/Payload.h"
#include "UI/Interaction/Types.h"

namespace GameEngine::UI::Interaction
{
struct DropRequest
{
    DragPayload payload{};
    DropHit hit{};
    int mods = 0; // input modifier bitmask (Input::ModifierMask)
};

// Small, capability-driven interface. Implemented by any element/control that can accept a drop:
// - items views (Tree/List/Grid)
// - Inspector fields (asset references, etc.)
// - custom editor widgets
struct IDropTarget
{
    virtual ~IDropTarget() = default;

    // Fast prefilter for per-frame hover checks (integer compare).
    virtual bool AcceptsPayload(PayloadTypeId typeId) const = 0;

    // Resolve the precise drop location at the given pointer position.
    //
    // NOTE: UIElement layout rects are stored in absolute coordinates, and UIEvent x/y are absolute.
    // For consistency, DragDropManager passes absolute coordinates here as well.
    // Returns false when the pointer is not over a valid drop zone for this target.
    virtual bool HitTestDropTarget(float x, float y, DropHit& out) const = 0;

    // Validation for current hover candidate (may be called per-frame).
    virtual DropFeedback CanDrop(const DropRequest& request) const = 0;

    // Execute a drop (called on mouse-up / commit).
    virtual void PerformDrop(const DropRequest& request) = 0;

    // Render-time preview state (insertion indicator, highlight, etc.).
    virtual void SetDropPreview(const DropPreviewState& state) = 0;
};

// Optional capability for drop targets hosted inside a ScrollView:
// while a drag is active and the pointer is near the top/bottom edges,
// the target may request smooth auto-scroll.
struct IDragAutoScrollTarget
{
    virtual ~IDragAutoScrollTarget() = default;
    virtual void AutoScrollDuringDrag(float mouseX, float mouseY) = 0;
};
} // namespace GameEngine::UI::Interaction

