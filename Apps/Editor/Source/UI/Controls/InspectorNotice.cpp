#include "UI/Controls/InspectorNotice.h"

#include "Editor/EditorPaths.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIEvents.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/InspectorNotice/InspectorNotice.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/InspectorNotice/InspectorNotice.css";
constexpr const char* kTextClass = "inspector-notice-text";
constexpr const char* kActionClass = "inspector-notice-action";
constexpr const char* kHeadingClass = "inspector-notice-heading";
constexpr const char* kTitleClass = "inspector-notice-title";
constexpr const char* kHiddenClass = "hidden";

std::string ReadLayoutText()
{
    const std::filesystem::path path = Editor::GetEditorGlobalPaths().installAssetsRoot / kLayoutAssetPath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        Logger::Log::Error("InspectorNotice: {} did not read, so every notice is an empty plate. Check that "
                           "it is staged under the editor asset mount.",
                           path.generic_string());
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Read once per process: a notice is built on every inspector rebuild that shows one, and parsing
// the few nodes of the layout is cheap where a disk read each time is not.
const std::string& LayoutText()
{
    static const std::string s_LayoutText = ReadLayoutText();
    return s_LayoutText;
}

} // namespace

InspectorNotice::InspectorNotice(const std::string& text, Kind kind)
{
    AddClass("inspector-notice");
    AddClass(kind == Kind::Information ? "inspector-notice-information" : "inspector-notice-warning");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    const std::string& layoutText = LayoutText();
    if (layoutText.empty())
        return;

    // The parsed plate is added whole rather than having its children moved onto this element:
    // construction has to work inside an event dispatch too, where TakeChild refuses to move
    // anything.
    std::unique_ptr<UIElement> plate;
    if (!UIParsing::XMLParser::ParseLayoutFromString(layoutText, plate, kLayoutAssetPath) || !plate)
    {
        Logger::Log::Error("InspectorNotice: {} did not parse, so the notice is an empty plate.", kLayoutAssetPath);
        return;
    }
    UIElement* plateRaw = plate.get();
    AddChild(std::move(plate));

    m_Text = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(plateRaw, kTextClass));
    m_Action = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(plateRaw, kActionClass));
    if (!m_Text || !m_Action)
    {
        Logger::Log::Error("InspectorNotice: {} needs a Label '{}' and a Button '{}' in its plate.", kLayoutAssetPath,
                           kTextClass, kActionClass);
        m_Text = nullptr;
        m_Action = nullptr;
        return;
    }

    m_Heading = InspectorDrag::FindChildByClass(plateRaw, kHeadingClass);
    m_Title = m_Heading ? dynamic_cast<Label*>(InspectorDrag::FindChildByClass(m_Heading, kTitleClass)) : nullptr;
    m_Text->SetText(text);
    m_Action->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { InvokeAction(); });
}

void InspectorNotice::SetAction(const std::string& label, const std::string& tooltip, std::function<void()> onInvoke)
{
    m_OnInvoke = std::move(onInvoke);
    if (!m_Action)
        return;
    m_Action->SetText(label);
    m_Action->SetTooltip(tooltip);
    m_Action->RemoveClass(kHiddenClass);
}

void InspectorNotice::SetTitle(const std::string& title)
{
    if (!m_Heading || !m_Title)
        return;
    m_Title->SetText(title);
    m_Heading->RemoveClass(kHiddenClass);
}

void InspectorNotice::SetText(const std::string& text)
{
    if (m_Text)
        m_Text->SetText(text);
}

void InspectorNotice::InvokeAction()
{
    if (m_OnInvoke)
        m_OnInvoke();
}

} // namespace GameEngine::EditorUI
