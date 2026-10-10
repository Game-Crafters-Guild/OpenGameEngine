#include "AgentCallPieceView.h"

#include "Core/Engine.h"
#include "ECS/World.h"
#include "UI/Controls/EntityLink.h"
#include "UI/Controls/Label.h"
#include "Types/FormatNumber.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr std::array<const char*, 4> kAxes = {"x", "y", "z", "w"};

std::unique_ptr<Label> MakeWords(const std::string& text, const char* cssClass)
{
    auto label = std::make_unique<Label>();
    label->SetText(text);
    label->AddClass(cssClass);
    return label;
}

std::unique_ptr<UIElement> MakeEntity(uint32_t id)
{
    const EditorUI::EntityLinkMissing missing{std::to_string(id),
                                              "No entity with id " + std::to_string(id) + " is in the scene"};
    const ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return MakeWords(missing.Text, "agent-call-muted");
    std::unique_ptr<UIElement> link =
        EditorUI::MakeEntityLink(ECS::EntityHandle(id), *world, EditorUI::EditorEntityLinkActions(), missing);
    // The summary's face: the row reads its action first.
    link->AddClass("agent-call-link");
    return link;
}

// The class of words and of muted words on `surface`.
const char* WordsClass(AgentCallPieceSurface surface)
{
    return surface == AgentCallPieceSurface::Details ? "agent-call-detail-value" : "agent-call-words";
}

const char* MutedClass(AgentCallPieceSurface surface)
{
    return surface == AgentCallPieceSurface::Details ? "agent-call-detail-key" : "agent-call-muted";
}

std::unique_ptr<UIElement> MakeVector(const std::vector<double>& components)
{
    auto vector = std::make_unique<UIElement>();
    vector->AddClass("agent-call-vector");
    for (size_t axis = 0; axis < components.size() && axis < kAxes.size(); ++axis)
    {
        vector->AddChild(MakeWords(kAxes[axis], "agent-call-vector-axis"));
        vector->AddChild(MakeWords(FormatFixed(components[axis], kAgentCallDecimals), "agent-call-vector-number"));
    }
    return vector;
}

// A Details column: the script face's advance at 12 px.
constexpr float kColumnPx = 7.0f;
// The gap between a Details line's pieces (.agent-call-details-line), which follows the indent.
constexpr float kLineGapPx = 6.0f;

std::unique_ptr<UIElement> MakeIndent(size_t columns)
{
    auto indent = std::make_unique<UIElement>();
    indent->AddClass("agent-call-indent");
    indent->Overrides().Set(Style::Width, StyleLength::Px(static_cast<float>(columns) * kColumnPx - kLineGapPx));
    return indent;
}

bool IsMuted(const AgentCallPiece& piece, std::string_view text)
{
    return piece.Type == AgentCallPiece::Kind::Muted && piece.Text == text;
}

// `piece` stays on one line with the piece before it: a "," after the piece it follows, any
// piece after a "·" (the separator leads what it separates), a vector after its key.
bool JoinsPrevious(const AgentCallPiece& previous, const AgentCallPiece& piece)
{
    return IsMuted(piece, ",") || IsMuted(previous, "·") ||
           (previous.Type == AgentCallPiece::Kind::Muted && piece.Type == AgentCallPiece::Kind::Vector);
}

std::unique_ptr<UIElement> MakePiece(const AgentCallPiece& piece, AgentCallPieceSurface surface)
{
    switch (piece.Type)
    {
    case AgentCallPiece::Kind::Text:
        return MakeWords(piece.Text, WordsClass(surface));
    case AgentCallPiece::Kind::Muted:
        return MakeWords(piece.Text, piece.Text == "," ? "agent-call-comma" : MutedClass(surface));
    case AgentCallPiece::Kind::Entity:
        return MakeEntity(piece.EntityId);
    case AgentCallPiece::Kind::Indent:
        return MakeIndent(piece.Text.size());
    case AgentCallPiece::Kind::Vector:
        break;
    }
    return MakeVector(piece.Components);
}
} // namespace

void ShowAgentCallPieces(UIElement& row, const AgentCallPieces& pieces, AgentCallPieceSurface surface)
{
    row.RemoveAllChildren();
    UIElement* unit = nullptr;
    for (size_t index = 0; index < pieces.size(); ++index)
    {
        if (!unit || !JoinsPrevious(pieces[index - 1], pieces[index]))
        {
            auto next = std::make_unique<UIElement>();
            next->AddClass("agent-call-unit");
            unit = next.get();
            row.AddChild(std::move(next));
        }
        unit->AddChild(MakePiece(pieces[index], surface));
    }
}
} // namespace GameEngine
