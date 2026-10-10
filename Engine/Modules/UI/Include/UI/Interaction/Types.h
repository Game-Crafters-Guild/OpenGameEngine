#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace GameEngine::UI::Interaction
{
// Stable per-item IDs used by items views (Tree/List/Grid).
// In v1 we keep this as u64 (matches TreeId/ListId/GridId).
using ItemId = std::uint64_t;

// Runtime payload type id (fast integer comparison during hover validation).
using PayloadTypeId = std::uint32_t;

enum class HitZone : std::uint8_t
{
    Unknown = 0,
    Body,
    Label,
    Icon,
    Expander,

    // Drop-oriented zones (typically derived from Y bands within an item rect).
    DropBefore,
    DropAfter,
    DropOnto,
};

struct ItemHit
{
    ItemId Id = 0;
    HitZone Zone = HitZone::Unknown;
};

enum class DropLocation : std::uint8_t
{
    OnItem = 0,
    BeforeItem,
    AfterItem,
    OnEmptySpace,
};

struct DropHit
{
    ItemId TargetId = 0; // 0 => empty space / root
    DropLocation Location = DropLocation::OnItem;
    int IndentDepth = 0; // useful for tree insertion guides
};

struct DropFeedback
{
    bool Allowed = false;
    std::string Reason;         // optional user-readable reason (tooltip/log)
    bool SuppressGhost = false; // hide the drag ghost (drop target provides its own visual)
    std::string CssClass;       // optional CSS class applied to the drop target preview (e.g. "external" for blue outline)
};

struct DropPreviewState
{
    bool Visible = false;
    bool Allowed = false;
    DropHit Hit{};
    std::string CssClass;       // forwarded from DropFeedback for custom styling
};

// Manager-owned context about the active drag session.
// This is not part of the payload; drop targets should not need source coupling.
struct DragSessionContext
{
    std::uint64_t SourceWidgetId = 0; // opaque stable id for internal bookkeeping (e.g. internal reorder)

    // Invoked once when the drag session ends (commit or cancel), after any PerformDrop has run.
    // Lets the drag SOURCE clear transient drag state (e.g. a drag-source row highlight) regardless
    // of where the drop landed — the source otherwise gets no end-of-drag signal.
    std::function<void()> OnDragEnded;
};
} // namespace GameEngine::UI::Interaction

