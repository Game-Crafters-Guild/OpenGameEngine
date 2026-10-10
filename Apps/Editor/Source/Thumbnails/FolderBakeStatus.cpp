#include "Thumbnails/FolderBakeStatus.h"

#include "Editor/EditorPaths.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIEvents.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace GameEngine
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/thumbnails/FolderBakeStatus/FolderBakeStatus.uxml";
constexpr const char* kStyleAssetPath = "UI/thumbnails/FolderBakeStatus/FolderBakeStatus.css";
constexpr const char* kTextClass = "folder-bake-status-text";
constexpr const char* kDismissClass = "folder-bake-status-dismiss";
constexpr const char* kHiddenClass = "hidden";

std::string ReadLayoutText()
{
    const std::filesystem::path path = Editor::GetEditorGlobalPaths().installAssetsRoot / kLayoutAssetPath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        Logger::Log::Error("FolderBakeStatus: {} did not read, so folder bakes report to the log only. Check "
                           "that it is staged under the editor asset mount.",
                           path.generic_string());
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

FolderBakeStatus::FolderBakeStatus(IThumbnailProvider& provider)
    : m_Self(std::make_shared<FolderBakeStatus*>(this))
{
    AddClass("folder-bake-status");
    AddClass(kHiddenClass);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    const std::string layoutText = ReadLayoutText();
    std::unique_ptr<UIElement> plate;
    if (layoutText.empty() ||
        !UIParsing::XMLParser::ParseLayoutFromString(layoutText, plate, kLayoutAssetPath) || !plate)
        return;
    UIElement* plateRaw = plate.get();
    AddChild(std::move(plate));
    m_Text = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(plateRaw, kTextClass));
    m_Dismiss = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(plateRaw, kDismissClass));
    if (!m_Text || !m_Dismiss)
    {
        Logger::Log::Error("FolderBakeStatus: {} needs a Label '{}' and a Button '{}' in its plate.",
                           kLayoutAssetPath, kTextClass, kDismissClass);
        m_Text = nullptr;
        m_Dismiss = nullptr;
        return;
    }
    m_Dismiss->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Dismiss(); });

    provider.SetFolderBakeListener(
        [self = std::weak_ptr<FolderBakeStatus*>(m_Self)](const FolderBakeReport& report)
        {
            if (const auto alive = self.lock())
                (*alive)->Show(report);
        });
}

void FolderBakeStatus::Show(const FolderBakeReport& report)
{
    if (!m_Text)
        return;
    const std::string text = DescribeFolderBakeReport(report);
    if (text.empty())
    {
        AddClass(kHiddenClass);
        return;
    }
    m_Text->SetText(text);
    if (report.Running)
        m_Dismiss->AddClass(kHiddenClass);
    else
        m_Dismiss->RemoveClass(kHiddenClass);
    RemoveClass(kHiddenClass);
}

void FolderBakeStatus::Dismiss()
{
    AddClass(kHiddenClass);
}

} // namespace GameEngine
