#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// A thread comment's text as the editor shows it: plain words, web links and entity links.
//
// A web link is a run starting "http://" or "https://" up to the next space or line break,
// less any trailing ".,;:!?)" (the sentence's punctuation, not the link's). An entity link is
// the token "[[entity:N]]", N the entity's id as markup_list and get_scene_hierarchy give it:
// the agent writes it to point at an entity, and the editor resolves it when it shows the
// comment, so a renamed entity's link shows its new name and a deleted one's says so.
struct MarkupCommentPiece
{
    enum class Kind : uint8
    {
        Text,
        Url,
        Entity,
    };

    Kind Type = Kind::Text;
    std::string Text; // the text, the URL, or the entity token as written
    uint32 EntityId = 0;
};

// What an entity link's click does: selects `entity`, and frames it when `frame` (its frame glyph).
using MarkupEntitySelect = std::function<void(ECS::EntityHandle entity, bool frame)>;

// The select every comment surface hands BuildMarkupCommentText: MarkupEditorBridge::SelectMarkup,
// the Mark-ups panel's own select and frame, in `world`.
MarkupEntitySelect SelectThroughBridge(MarkupEditorBridge& bridge, ECS::World& world);

// `text` cut into pieces in order; plain text between links is one piece, line breaks kept.
std::vector<MarkupCommentPiece> ParseMarkupComment(std::string_view text);

// The comment as an element: one wrapping label with `textClass` when it holds no link; else a
// wrapping row of its words, a link in the theme's link style that opens the browser on a
// click, and an entity link as a chip with the entity's name that highlights it in the Scene View
// while hovered (through `bridge`) and selects it on a click, with a frame glyph at its end that
// frames it (both through `select`); a link to an entity that is gone is a muted "deleted entity".
// `world` is the edit world the ids name.
std::unique_ptr<UIElement> BuildMarkupCommentText(const std::string& text, const char* textClass,
                                                  MarkupEditorBridge& bridge, const ECS::World& world,
                                                  const MarkupEntitySelect& select);

} // namespace GameEngine::Editor
