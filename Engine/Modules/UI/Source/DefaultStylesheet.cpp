#include "DefaultStylesheet.h"

#include "Logger/Logger.h"
#include "UI/Parsers/CSSParser.h"

#include <cassert>
#include <memory>
#include <string>

namespace GameEngine::UI
{
namespace
{

// Assets/defaults.css, embedded by the build (cmake/EmbedTextFile.cmake).
#include "DefaultStylesheetText.h"

constexpr const char* kDefaultStylesheetSourceName = "Engine/Modules/UI/Assets/defaults.css";

// A pseudo-class the engine does not implement parses as a custom state, so in this sheet it can
// only be a typo (".toggle:chekced"): no engine control sets a custom state.
bool UsesCustomStatePseudoClass(const Stylesheet& sheet)
{
    for (const CSSRule& rule : sheet.Rules)
    {
        for (const SelectorTerm& term : rule.Selector.Terms)
        {
            for (const PseudoClass& pseudo : term.Selector.Pseudos)
            {
                if (pseudo.PseudoKind == PseudoClass::Kind::Custom)
                    return true;
            }
        }
    }
    return false;
}

bool HasImportantDeclaration(const Stylesheet& sheet)
{
    for (const CSSRule& rule : sheet.Rules)
    {
        for (const StyleProperty& property : rule.Properties)
        {
            if (property.Important)
                return true;
        }
    }
    return false;
}

StylesheetHandle ParseDefaultStylesheet()
{
    const std::string text(reinterpret_cast<const char*>(kDefaultStylesheetText),
                           sizeof(kDefaultStylesheetText) - 1);
    auto sheet = std::make_shared<Stylesheet>();
    if (!UIParsing::CSSParser::ParseStylesFromString(text, *sheet))
    {
        Logger::Log::Error("UI: the default stylesheet '{}' did not parse, so engine controls such as "
                           "sliders and toggles draw without their default look. Fix the sheet and "
                           "rebuild; it is compiled into the UI module.",
                           kDefaultStylesheetSourceName);
        assert(false && "the UI default stylesheet did not parse");
        return nullptr;
    }
    // A declaration no parser row claims is dropped whole; in engine code that is a defect.
    for (const std::string& name : sheet->UnknownProperties)
    {
        Logger::Log::Error("UI: the default stylesheet '{}' uses '{}', which the UI module does not "
                           "support, so that declaration is dropped.",
                           kDefaultStylesheetSourceName, name);
    }
    assert(sheet->UnknownProperties.empty() && "the UI default stylesheet uses an unsupported property");
    // The cascade does not reverse origin order for !important, so an important default would
    // beat an author's important rule instead of losing to it.
    const bool hasImportant = HasImportantDeclaration(*sheet);
    if (hasImportant)
    {
        Logger::Log::Error("UI: the default stylesheet '{}' uses !important, which a user-agent sheet "
                           "may not; remove it.",
                           kDefaultStylesheetSourceName);
    }
    assert(!hasImportant && "the UI default stylesheet uses !important");
    const bool usesCustomState = UsesCustomStatePseudoClass(*sheet);
    if (usesCustomState)
    {
        Logger::Log::Error("UI: the default stylesheet '{}' uses a pseudo-class the UI module does not "
                           "implement; check the selector for a typo.",
                           kDefaultStylesheetSourceName);
    }
    assert(!usesCustomState && "the UI default stylesheet uses an unknown pseudo-class");
    sheet->Origin = StyleOrigin::UserAgent;
    sheet->SourceName = kDefaultStylesheetSourceName;
    return sheet;
}

} // namespace

StylesheetHandle GetDefaultStylesheet()
{
    static const StylesheetHandle sheet = ParseDefaultStylesheet();
    return sheet;
}

} // namespace GameEngine::UI
