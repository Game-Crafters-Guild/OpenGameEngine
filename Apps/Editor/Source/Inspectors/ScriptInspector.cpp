#include "Inspectors/ScriptInspector.h"

#include "InspectorRegistry.h"

#include "Assets/ScriptAsset.h"

#include "ExternalScriptEditorLauncher.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/StyleProperties.h"

#include <filesystem>
#include <string>
#include <utility>

namespace GameEngine {

using InspectorUI::AddTextBlock;

namespace
{

static void BuildScriptInspector(UIElement* root,
                                 ScriptAsset* script,
                                 std::function<void(const std::filesystem::path&)> openScript)
{
    if (!root || !script)
        return;

    // The asset title is already rendered by BuildSimpleTopHeader in the panel's
    // fixed top strip — don't duplicate it inside the scroll content.

    // Basic info: path and type on separate blocks so spacing can target the type line.
    AddTextBlock(root, script->GetPath().string(), "inspector-asset-path");
    AddTextBlock(root, "Type: Script", "inspector-asset-type-line");

    const std::filesystem::path scriptPath = script->GetPath();

    auto buttonRow = std::make_unique<UIElement>();
    buttonRow->AddClass("inspector-asset-actions");
    buttonRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::AlignSelf, AlignItems::Stretch);

    if (openScript)
    {
        auto editBtn = std::make_unique<Button>();
        editBtn->SetText("Open In Script Editor");
        editBtn->AddClass("inspector-text");
        editBtn->AddClass("small");
        editBtn->Overrides().Set(Style::Width, StyleLength::Px(160.0f));
        editBtn->RegisterEventHandler(kEventButtonClick, [openScript = std::move(openScript), scriptPath](UIEvent&)
                            { openScript(scriptPath); });
        buttonRow->AddChild(std::move(editBtn));
    }

    {
        auto ideBtn = std::make_unique<Button>();
        ideBtn->SetText("Open In IDE");
        ideBtn->AddClass("inspector-text");
        ideBtn->AddClass("small");
        ideBtn->Overrides().Set(Style::Width, StyleLength::Px(100.0f));
        ideBtn->RegisterEventHandler(kEventButtonClick, [scriptPath](UIEvent&)
                           { ExternalScriptEditorLauncher::OpenScript(scriptPath); });
        buttonRow->AddChild(std::move(ideBtn));
    }

    root->AddChild(std::move(buttonRow));

    InspectorUI::AddSourceFileRow(root, scriptPath);
}

} // namespace

void RegisterScriptInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::Script,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;

            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* script = dynamic_cast<ScriptAsset*>(asset);
            if (!script)
                return;

            BuildScriptInspector(ctx.Parent, script, ctx.OpenScript);
        });
}

} // namespace GameEngine
