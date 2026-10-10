// Tests for UIManager font-family resolver integration.
// Verifies that missing font families trigger a host callback once (deduped),
// and that async completion installs a FontAtlas and becomes visible to style resolution.

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "Rendering/Core/Device.h"
#include "RobotoTestFont.h"
#include "UIRgTestHarness.h"

#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{

static std::vector<uint8_t> LoadRobotoBytes()
{
    return UITesting::LoadStagedFontBytes("Roboto-Regular.ttf");
}

static std::vector<uint8_t> LoadRobotoMonoBytes()
{
    return UITesting::LoadStagedFontBytes("RobotoMono-Regular.ttf");
}
} // namespace

TEST(UIFontLoadingTests, RequestsFamilyOnceAndAppliesOnCompletion)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    // Minimal layout with a label so geometry generation attempts to resolve font-family.
    const std::string xml = R"(<uielement id='root'>
        <Label id='lbl' text='Hello' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    // Style: explicitly request Roboto so the family resolver is exercised.
    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto cssPath = tmpDir / "ui_font_loading_resolver_test.css";
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 200px; height: 60px; }
#lbl { font-size: 14px; font-family: "Roboto", sans-serif; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    int calls = 0;
    std::unordered_map<std::string, int> callsByFamily;
    std::mutex mu;
    UIManager::FontResolveCallback pending;
    std::string pendingFamily;

    ui.SetFontResolver([&](const std::string& family,
                           int /*weight*/,
                           FontStyle /*style*/,
                           FontVariant /*variant*/,
                           UIManager::FontResolveCallback onReady)
                       {
                           ++calls;
                           std::lock_guard<std::mutex> lk(mu);
                           ++callsByFamily[family];
                           pending = std::move(onReady);
                           pendingFamily = family;
                       });

    // First update should request missing families (deduped per family within the frame).
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_FALSE(pendingFamily.empty());

    // Second update (still in-flight) must not request any family again.
    ui.Update(0.0f, /*interactive=*/true);
    for (const auto& kv : callsByFamily)
    {
        EXPECT_EQ(kv.second, 1) << "Family '" << kv.first << "' was requested more than once";
    }

    // While the font is still unresolved, layout must not collapse to 0 height.
    // This prevents overlap/stacking during async font resolution.
    UIElement* rootEl = ui.GetRootElement();
    ASSERT_NE(rootEl, nullptr);
    UIElement* lblEl = rootEl->FindById("lbl");
    ASSERT_NE(lblEl, nullptr);
    EXPECT_GT(lblEl->GetLayoutHeight(), 0.0f);

    // Complete the request with real font bytes. A missing staged font is a
    // broken build, not a capability this machine lacks, so it FAILS: skipping
    // here is indistinguishable from passing.
    auto robotoBytes = LoadRobotoBytes();
    ASSERT_FALSE(robotoBytes.empty())
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf")
        << " — the build did not stage it beside the test executable";

    UIManager::FontResolveCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu);
        cb = std::move(pending);
    }
    ASSERT_TRUE((bool)cb);

    UIManager::FontResolveResult res{};
    res.Bytes = std::move(robotoBytes);
    res.DebugName = "UIFontLoadingTests.Roboto-Regular";
    cb(std::move(res));

    // The completion is marshaled via the UI dispatcher; drain and update so the atlas is installed.
    ui.DrainDeferredActionsOnce();
    ui.Update(0.0f, /*interactive=*/true);

    ResolvedStyle style{};
    style.Visual.FontFamily = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"Roboto"});
    EXPECT_NE(ui.ResolveFontForStyle(style), nullptr);
}

TEST(UIFontLoadingTests, PassesWeightAndStyleToResolver)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<uielement id='root'>
        <Label id='lbl' text='Hello' />
    </uielement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto tmpDir = std::filesystem::temp_directory_path();
    const auto cssPath = tmpDir / "ui_font_loading_weight_style_test.css";
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 200px; height: 60px; }
#lbl { font-size: 14px; font-family: "Roboto"; font-weight: 700; font-style: italic; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    int seenWeight = 0;
    FontStyle seenStyle = FontStyle::Normal;
    FontVariant seenVariant = FontVariant::Normal;
    UIManager::FontResolveCallback pending;

    ui.SetFontResolver([&](const std::string&,
                           int weight,
                           FontStyle style,
                           FontVariant variant,
                           UIManager::FontResolveCallback onReady)
                       {
                           seenWeight = weight;
                           seenStyle = style;
                           seenVariant = variant;
                           pending = std::move(onReady);
                       });

    ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(seenWeight, 700);
    EXPECT_EQ(seenStyle, FontStyle::Italic);
    EXPECT_EQ(seenVariant, FontVariant::Normal);

    auto robotoBytes = LoadRobotoBytes();
    ASSERT_FALSE(robotoBytes.empty())
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf")
        << " — the build did not stage it beside the test executable";

    ASSERT_TRUE((bool)pending);
    UIManager::FontResolveResult res{};
    res.Bytes = std::move(robotoBytes);
    res.DebugName = "UIFontLoadingTests.Roboto-Regular";
    pending(std::move(res));

    ui.DrainDeferredActionsOnce();
    ui.Update(0.0f, /*interactive=*/true);

    ResolvedStyle rst{};
    rst.Visual.FontFamily = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"Roboto"});
    rst.Visual.FontWeight = 700;
    rst.Visual.FontStyle = FontStyle::Italic;
    EXPECT_NE(ui.ResolveFontForStyle(rst), nullptr);
}

TEST(UIFontLoadingTests, PendingFamilyUsesExplicitDefaultInsteadOfArbitraryLoadedAtlas)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIManager ui(dev);

    auto robotoBytes = LoadRobotoBytes();
    auto monoBytes = LoadRobotoMonoBytes();
    ASSERT_FALSE(robotoBytes.empty())
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf")
        << " — the build did not stage it beside the test executable";
    ASSERT_FALSE(monoBytes.empty())
        << "staged RobotoMono-Regular.ttf not found at "
        << UITesting::StagedFontPath("RobotoMono-Regular.ttf")
        << " — the build did not stage it beside the test executable";

    ASSERT_TRUE(ui.SetDefaultFontBytes(robotoBytes));
    Rendering::Text::FontAtlas* defaultAtlas = ui.GetDefaultFontAtlas();
    ASSERT_NE(defaultAtlas, nullptr);

    UIManager::FontResolveCallback pendingRoboto;
    ui.SetFontResolver([&](const std::string& family,
                           int,
                           FontStyle,
                           FontVariant,
                           UIManager::FontResolveCallback onReady)
                       {
                           if (family == "Roboto Mono")
                           {
                               UIManager::FontResolveResult res{};
                               res.Bytes = monoBytes;
                               res.DebugName = "UIFontLoadingTests.RobotoMono-Regular";
                               onReady(std::move(res));
                               return;
                           }

                           if (family == "Roboto")
                           {
                               pendingRoboto = std::move(onReady);
                               return;
                           }

                           onReady(UIManager::FontResolveResult{});
                       });

    ui.RequestFontFamily("Roboto Mono");
    ui.DrainDeferredActionsOnce();

    ResolvedStyle style{};
    style.Visual.FontFamily = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"Roboto"});
    style.Visual.FontWeight = 600;

    EXPECT_EQ(ui.ResolveFontForStyle(style), defaultAtlas);
    EXPECT_TRUE((bool)pendingRoboto);
}
