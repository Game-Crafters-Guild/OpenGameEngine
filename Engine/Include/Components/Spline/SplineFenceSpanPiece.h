#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Which span of its fence a placed span or gate piece is, addressed the way a
// span override addresses one: the authored point opening its run and its
// ordinal in that run's fill. The editor's fence controller writes it on every
// span and gate it places, so a span picked in the scene view can be made a
// gate or pinned from its own inspector without the recipe being searched.
// Generated with the piece and never saved with it.
//
// @ge-no-add  Written by the fence controller on the pieces it places.
// [DoNotSerialize] — generated output, rebuilt with the piece.
struct SplineFenceSpanPiece
{
    uint32 Run = 0;
    uint32 OrdinalInRun = 0;
    // The active pool slot the piece draws: a SpanPool slot, or a GatePool slot
    // when IsGate.
    uint32 PoolSlot = 0;
    bool IsGate = false;

    bool operator==(const SplineFenceSpanPiece&) const = default;
};

static_assert(std::is_trivially_copyable_v<SplineFenceSpanPiece>);
static_assert(std::is_standard_layout_v<SplineFenceSpanPiece>);

} // namespace GameEngine::Components
