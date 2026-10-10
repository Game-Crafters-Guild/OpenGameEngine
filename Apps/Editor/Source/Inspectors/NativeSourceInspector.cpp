#include "Inspectors/NativeSourceInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"

#include "Core/Engine.h"
#include "NativeScripting/NativeScriptManager.h"
#include "Platform/Shell.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace GameEngine {

using InspectorUI::AddTextBlock;

namespace
{

// Header-vs-source + C-vs-C++ label the central NativeSource type collapses (presentation only).
const char* NativeSourceTypeLabel(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh")
        return "C++ Header";
    if (ext == ".c")
        return "C Source";
    return "C++ Source";
}

static void BuildNativeSourceInspector(UIElement* root, Asset* asset, const InspectorContext& ctx)
{
    if (!root || !asset)
        return;

    const std::filesystem::path path = asset->GetPath();

    // Basic info: path + friendly type on separate blocks (the title is rendered by the panel header).
    AddTextBlock(root, path.string(), "inspector-asset-path");
    AddTextBlock(root, std::string("Type: ") + NativeSourceTypeLabel(path), "inspector-asset-type-line");

    auto buttonRow = std::make_unique<UIElement>();
    buttonRow->AddClass("inspector-asset-actions");
    buttonRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::AlignSelf, AlignItems::Stretch);

    // Internal: open in the shared Script Editor panel (handles any text).
    if (ctx.OpenScript)
    {
        auto editBtn = std::make_unique<Button>();
        editBtn->SetText("Open In Editor");
        editBtn->AddClass("inspector-text");
        editBtn->AddClass("small");
        editBtn->Overrides().Set(Style::Width, StyleLength::Px(120.0f));
        auto openScript = ctx.OpenScript;
        editBtn->RegisterEventHandler(kEventButtonClick, [openScript, path](UIEvent&) { openScript(path); });
        buttonRow->AddChild(std::move(editBtn));
    }

    // External: open in the user's IDE with the native project folder for context.
    {
        auto ideBtn = std::make_unique<Button>();
        ideBtn->SetText("Open In IDE");
        ideBtn->AddClass("inspector-text");
        ideBtn->AddClass("small");
        ideBtn->Overrides().Set(Style::Width, StyleLength::Px(100.0f));
        ideBtn->RegisterEventHandler(kEventButtonClick, [path](UIEvent&)
                           {
            std::filesystem::path projectDir;
            if (auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager())
                projectDir = nativeScripts->WatchDirectory();
            Platform::OpenSourceWithProject(path, projectDir); });
        buttonRow->AddChild(std::move(ideBtn));
    }

    root->AddChild(std::move(buttonRow));

    InspectorUI::AddSourceFileRow(root, path);
}

} // namespace

void RegisterNativeSourceInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::NativeSource,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;
            BuildNativeSourceInspector(ctx.Parent, static_cast<Asset*>(ctx.Object), ctx);
        });
}

} // namespace GameEngine
