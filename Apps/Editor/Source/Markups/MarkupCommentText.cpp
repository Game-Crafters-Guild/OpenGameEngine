#include "Markups/MarkupCommentText.h"

#include "ECS/Entity.h"
#include "Markups/MarkupEditorBridge.h"
#include "Platform/Shell.h"
#include "UI/Controls/EntityLink.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <memory>

namespace GameEngine::Editor
{

namespace
{

// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

constexpr std::string_view kEntityOpen = "[[entity:";
constexpr std::string_view kEntityClose = "]]";
constexpr std::string_view kTrailingPunctuation = ".,;:!?)";

bool StartsWith(std::string_view text, std::size_t at, std::string_view prefix)
{
    return text.substr(at, prefix.size()) == prefix;
}

bool IsLinkEnd(char c)
{
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

// The length of the web link starting at `at`, 0 when none starts there.
std::size_t UrlLength(std::string_view text, std::size_t at)
{
    if (!StartsWith(text, at, "http://") && !StartsWith(text, at, "https://"))
        return 0;
    std::size_t end = at;
    while (end < text.size() && !IsLinkEnd(text[end]))
        ++end;
    while (end > at && kTrailingPunctuation.find(text[end - 1]) != std::string_view::npos)
        --end;
    const std::size_t scheme = StartsWith(text, at, "https://") ? 8 : 7;
    return end - at > scheme ? end - at : 0;
}

// The length of the entity token starting at `at` and its id, 0 when none starts there.
std::size_t EntityTokenLength(std::string_view text, std::size_t at, uint32& outId)
{
    if (!StartsWith(text, at, kEntityOpen))
        return 0;
    const std::size_t digits = at + kEntityOpen.size();
    const std::size_t close = text.find(kEntityClose, digits);
    if (close == std::string_view::npos || close == digits || close - digits > 10)
        return 0;
    uint64 id = 0;
    for (std::size_t index = digits; index < close; ++index)
    {
        if (text[index] < '0' || text[index] > '9')
            return 0;
        id = id * 10 + static_cast<uint64>(text[index] - '0');
    }
    if (id > 0xFFFFFFFFull)
        return 0;
    outId = static_cast<uint32>(id);
    return close + kEntityClose.size() - at;
}

std::unique_ptr<Label> MakeWord(std::string text, const char* cssClass)
{
    auto label = std::make_unique<Label>();
    label->SetText(std::move(text));
    label->AddClass(cssClass);
    return label;
}

// Appends `text`'s words to `row` as labels, a line break as a full-width break.
void AppendWords(UIElement& row, std::string_view text)
{
    std::size_t start = 0;
    while (start < text.size())
    {
        if (text[start] == '\n')
        {
            auto lineBreak = std::make_unique<UIElement>();
            lineBreak->AddClass("markup-comment-break");
            row.AddChild(std::move(lineBreak));
            ++start;
            continue;
        }
        // A word keeps its trailing space, so the row's words read spaced as typed.
        std::size_t end = start;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n')
            ++end;
        while (end < text.size() && text[end] == ' ')
            ++end;
        row.AddChild(MakeWord(std::string(text.substr(start, end - start)), "markup-comment-word"));
        start = end;
    }
}

std::unique_ptr<UIElement> MakeUrlLink(const std::string& url)
{
    auto link = MakeWord(url, "markup-link");
    link->SetTooltip("Open " + url + " in the browser");
    link->RegisterEventHandler(kEventMouseUp, [url](UIEvent& event) {
        if (event.Button != kLeftButton)
            return;
        event.Stop();
        (void)Platform::OpenUrl(url);
    });
    return link;
}

// The shared entity link (EditorUI::MakeEntityLink): selected and framed through `select`,
// highlighted in the Scene View through the bridge while hovered.
std::unique_ptr<UIElement> MakeEntityLink(uint32 id, MarkupEditorBridge& bridge, const ECS::World& world,
                                          const MarkupEntitySelect& select)
{
    EditorUI::EntityLinkActions actions;
    actions.Select = select;
    actions.Hover = [&bridge](ECS::EntityHandle entity) { bridge.HoverMarkup(entity); };
    const EditorUI::EntityLinkMissing missing{
        "deleted entity", "The entity this link named (id " + std::to_string(id) + ") is no longer in the scene"};
    return EditorUI::MakeEntityLink(ECS::EntityHandle(id), world, actions, missing);
}

} // namespace

std::vector<MarkupCommentPiece> ParseMarkupComment(std::string_view text)
{
    std::vector<MarkupCommentPiece> pieces;
    std::size_t plainStart = 0;
    const auto flushPlain = [&](std::size_t end) {
        if (end > plainStart)
            pieces.push_back({MarkupCommentPiece::Kind::Text, std::string(text.substr(plainStart, end - plainStart)), 0});
    };
    for (std::size_t at = 0; at < text.size();)
    {
        uint32 id = 0;
        if (const std::size_t length = EntityTokenLength(text, at, id); length > 0)
        {
            flushPlain(at);
            pieces.push_back({MarkupCommentPiece::Kind::Entity, std::string(text.substr(at, length)), id});
            at += length;
            plainStart = at;
            continue;
        }
        const bool wordStart = at == 0 || IsLinkEnd(text[at - 1]) || text[at - 1] == '(';
        if (const std::size_t length = wordStart ? UrlLength(text, at) : 0; length > 0)
        {
            flushPlain(at);
            pieces.push_back({MarkupCommentPiece::Kind::Url, std::string(text.substr(at, length)), 0});
            at += length;
            plainStart = at;
            continue;
        }
        ++at;
    }
    flushPlain(text.size());
    return pieces;
}

MarkupEntitySelect SelectThroughBridge(MarkupEditorBridge& bridge, ECS::World& world)
{
    return [&bridge, &world](ECS::EntityHandle entity, bool frame) { bridge.SelectMarkup(world, entity, frame); };
}

std::unique_ptr<UIElement> BuildMarkupCommentText(const std::string& text, const char* textClass,
                                                  MarkupEditorBridge& bridge, const ECS::World& world,
                                                  const MarkupEntitySelect& select)
{
    const std::vector<MarkupCommentPiece> pieces = ParseMarkupComment(text);
    const bool plain = std::all_of(pieces.begin(), pieces.end(), [](const MarkupCommentPiece& piece) {
        return piece.Type == MarkupCommentPiece::Kind::Text;
    });
    if (plain)
        return MakeWord(text, textClass);

    // The row takes the text's class (its column, padding and color); its words take the row's
    // look through markup-comment-word.
    auto row = std::make_unique<UIElement>();
    row->AddClass(textClass);
    row->AddClass("markup-comment-text");
    for (const MarkupCommentPiece& piece : pieces)
    {
        switch (piece.Type)
        {
        case MarkupCommentPiece::Kind::Text: AppendWords(*row, piece.Text); break;
        case MarkupCommentPiece::Kind::Url: row->AddChild(MakeUrlLink(piece.Text)); break;
        case MarkupCommentPiece::Kind::Entity: row->AddChild(MakeEntityLink(piece.EntityId, bridge, world, select)); break;
        }
    }
    return row;
}

} // namespace GameEngine::Editor
