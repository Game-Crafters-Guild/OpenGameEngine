#pragma once

#include "AgentCallPieces.h"

#include <cstdint>

namespace GameEngine
{
class UIElement;

/// Replaces `row`'s children with `pieces`, each as the call rows draw it: words, muted
/// words, an entity as the editor's entity link (EditorUI::MakeEntityLink over the edit
/// world, selecting and framing through the editor; an id no entity has is its muted
/// number), a vector as its components with muted axis labels and the numbers in the
/// script face. Pieces that read together stay on one line as a unit: a "," with the piece
/// before it, a "·" with the piece after it, a key with its vector; the row wraps between
/// units. One drawing for a row's summary and for its Details lines.
enum class AgentCallPieceSurface : uint8_t
{
    /// A row's summary: the row's words, entity links in its face.
    Summary,
    /// A Details line: the script face, keys muted, values a step brighter.
    Details,
};

void ShowAgentCallPieces(UIElement& row, const AgentCallPieces& pieces, AgentCallPieceSurface surface);
} // namespace GameEngine
