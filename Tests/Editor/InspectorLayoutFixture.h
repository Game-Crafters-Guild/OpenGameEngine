#pragma once

// A real UIManager over the editor's own stylesheets, so a measurement is the editor's layout and
// not a restatement of a test's own CSS. Shared by the inspector layout tests.

#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace InspectorLayoutTesting
{

inline std::string ReadEditorFile(const std::string& relativePath)
{
    const std::filesystem::path path = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline GameEngine::UIElement* FindByClass(GameEngine::UIElement* root, const std::string& className)
{
    if (!root)
        return nullptr;
    if (root->HasClass(className))
        return root;
    for (const auto& child : root->GetChildren())
    {
        if (GameEngine::UIElement* found = FindByClass(child.get(), className))
            return found;
    }
    return nullptr;
}

struct InspectorLayoutFixture
{
    // Wide enough that a header row has slack to give its title, and tall enough that nothing a
    // test lays out is scrolled out of layout.
    uint32_t ViewportW = 800;
    uint32_t ViewportH = 600;

    // Editor sheets loaded after tokens.css (which carries the tokens the rules resolve, such as
    // --inspector-header-height) and inspector.css. In the editor a control attaches its own sheet
    // below the global theme, so a control's sheet a test depends on goes here, in that order.
    std::vector<std::string> ExtraSheets;

    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    std::unique_ptr<GameEngine::UIManager> Ui;
    std::string Diagnostic;

    bool Build(std::unique_ptr<GameEngine::UIElement> root)
    {
        Device = MakeHeadlessDevice();
        if (!Device)
        {
            Diagnostic = "no Vulkan device";
            return false;
        }
        Ui = std::make_unique<GameEngine::UIManager>(Device.get());
        Ui->SetLayoutSizeOverride(ViewportW, ViewportH);

        std::vector<std::string> sheets{"Assets/UI/theme/tokens.css", "Assets/UI/theme/inspector.css"};
        sheets.insert(sheets.end(), ExtraSheets.begin(), ExtraSheets.end());
        for (const std::string& sheetPath : sheets)
        {
            const std::string css = ReadEditorFile(sheetPath);
            if (css.empty())
            {
                Diagnostic = "stylesheet did not read: " + sheetPath;
                return false;
            }
            GameEngine::Stylesheet sheet{};
            if (!GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, sheet))
            {
                Diagnostic = "stylesheet did not parse: " + sheetPath;
                return false;
            }
            sheet.SourceName = sheetPath;
            Ui->AddStylesheet(std::make_shared<const GameEngine::Stylesheet>(std::move(sheet)));
        }

        Ui->SetRoot(std::move(root));
        // Update owns style resolution and the Yoga solve; nothing here reads a primitive, so the
        // frame is never rendered and no shader path has to resolve.
        for (int i = 0; i < kSettleFrames; ++i)
            Ui->Update(kFrameSeconds, /*interactive=*/true);
        return true;
    }

private:
    static constexpr int kSettleFrames = 3;
    static constexpr float kFrameSeconds = 1.0f / 60.0f;
};

} // namespace InspectorLayoutTesting
