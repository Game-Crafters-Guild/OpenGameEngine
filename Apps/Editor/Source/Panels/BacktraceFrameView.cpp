#include "Panels/BacktraceFrameView.h"

#include <cstdio>
#include <string>
#include <utility>

#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/SyntaxHighlightSettings.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

namespace GameEngine {

namespace {

std::unique_ptr<Label> MakeSpan(const std::string& text, const char* colorClass, std::uint32_t argb)
{
    auto lbl = std::make_unique<Label>();
    lbl->AddClass("stack-trace-frame-span");
    if (colorClass)
        lbl->AddClass(colorClass);
    lbl->SetText(text);
    lbl->Overrides().Set(Style::Color, argb);
    return lbl;
}

} // namespace

std::unique_ptr<UIElement> BuildBacktraceFrameRow(
    std::size_t index,
    const Logger::BacktraceFrame& f,
    std::function<void(const std::string&, int)> onOpenSource)
{
    const auto& s = SyntaxHighlightSettings::Get();
    const std::uint32_t defaultColor = s.DefaultColor;
    const std::uint32_t numberColor  = s.NumberColor;
    const std::uint32_t typeColor    = s.TypeColor;
    const std::uint32_t stringColor  = s.StringColor;
    const std::uint32_t keywordColor = s.KeywordColor;

    auto frame = std::make_unique<UIElement>();
    frame->AddClass("stack-trace-frame-row");

    auto header = std::make_unique<UIElement>();
    header->AddClass("stack-trace-frame-header");

    char idxText[16];
    std::snprintf(idxText, sizeof(idxText), "[%zu]", index);
    header->AddChild(MakeSpan(idxText, "idx", numberColor));
    header->AddChild(MakeSpan(" ", "gap", defaultColor));

    if (!f.Module.empty())
    {
        header->AddChild(MakeSpan(f.Module, "module", typeColor));
        header->AddChild(MakeSpan("!", "punct", defaultColor));
    }

    const std::string& symbolText = f.Symbol.empty() ? std::string("<unknown>") : f.Symbol;
    auto symbolLbl = MakeSpan(symbolText, "symbol", keywordColor);
    symbolLbl->AddClass("wrap");
    header->AddChild(std::move(symbolLbl));
    frame->AddChild(std::move(header));

    if (!f.File.empty())
    {
        auto source = std::make_unique<UIElement>();
        source->AddClass("stack-trace-frame-source");

        std::string pathText = f.File;
        if (f.Line > 0)
        {
            pathText += ":";
            pathText += std::to_string(f.Line);
        }
        auto pathLbl = MakeSpan(pathText, "path", stringColor);
        pathLbl->AddClass("wrap");
        source->AddChild(std::move(pathLbl));
        frame->AddChild(std::move(source));
    }

    if (onOpenSource && !f.File.empty())
    {
        frame->AddClass("stack-trace-frame-row--clickable");
        std::string capturedFile = f.File;
        int capturedLine = f.Line;
        auto cb = std::move(onOpenSource);
        frame->RegisterEventHandler(kEventMouseUp, [cb, capturedFile, capturedLine](UIEvent&) {
            cb(capturedFile, capturedLine);
        });
    }

    return frame;
}

} // namespace GameEngine
