// The panel launcher's icon image comes from CSS, not from a table in C++.
//
// EditorTopToolbar used to carry two hand-maintained tables: a
// "<name>-icon" -> "Icons/<name>.png" convention with six authored-filename
// exceptions, and a panelId -> path table for panels with no tab icon. Both
// restated declarations that already existed in the editor stylesheets, and
// both could drift from them silently — a test that only asserted "the path is
// non-empty" would have passed against either.
//
// So the load-bearing case here changes the stylesheet and requires the
// resolved path to change with it. Nothing hardcoded can satisfy that.
// (Which class each panel wears is the panel type's DeclaredTabIconClass();
// EditorTests' PanelDefaultTabIcon suite holds those classes to account
// against the authored cascade.)

#include "UI/Parsers/CSSParser.h"
#include "UI/ClassBackgroundImageQuery.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using GameEngine::PathUtils;
using GameEngine::Stylesheet;
using GameEngine::StylesheetHandle;
using GameEngine::UIParsing::CSSParser;
using GameEngine::UIStyleQuery::ResolveClassBackgroundImageUrl;

namespace
{

StylesheetHandle ParseSheet(const std::string& cssText)
{
    auto sheet = std::make_shared<Stylesheet>();
    if (!CSSParser::ParseStylesFromString(cssText, *sheet))
        return nullptr;
    return sheet;
}

std::string Resolve(const std::vector<StylesheetHandle>& sheets, std::string_view className)
{
    return ResolveClassBackgroundImageUrl(sheets, className);
}

std::string ResolveIn(const std::string& cssText, std::string_view className)
{
    StylesheetHandle sheet = ParseSheet(cssText);
    EXPECT_TRUE(static_cast<bool>(sheet)) << "CSS failed to parse";
    return Resolve({sheet}, className);
}

// The editor's UI assets as the build stages them next to the test exe, at the
// same relative path the editor reads them from under its own exe. Anchored to
// the executable directory, never back into the source tree.
std::filesystem::path ShippedUiRoot()
{
    return PathUtils::GetExecutableDirectory() / "Assets" / "UI";
}

std::string ReadShipped(const std::filesystem::path& relative)
{
    std::ifstream in(ShippedUiRoot() / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// theme.css is the editor's one global stylesheet; UIManager flattens its
// @imports into the global cascade in source order, and this mirrors that so
// the test sees the same sheets, in the same order, that the launcher does.
std::vector<StylesheetHandle> ShippedGlobalCascade()
{
    const std::string master = ReadShipped("theme.css");
    EXPECT_FALSE(master.empty()) << "theme.css was not staged next to the test exe";

    std::vector<StylesheetHandle> cascade;
    std::istringstream lines(master);
    std::string line;
    while (std::getline(lines, line))
    {
        const size_t at = line.find("@import");
        if (at == std::string::npos)
            continue;
        const size_t open = line.find('"', at);
        const size_t close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
        if (close == std::string::npos)
            continue;
        const std::string relative = line.substr(open + 1, close - open - 1);
        const std::string css = ReadShipped(relative);
        EXPECT_FALSE(css.empty()) << "imported sheet not staged: " << relative;
        if (StylesheetHandle sheet = ParseSheet(css))
            cascade.push_back(std::move(sheet));
    }
    // theme.css is @import statements only, so it contributes no rules itself.
    return cascade;
}

} // namespace

// The load-bearing case. One class name, two stylesheets, two answers — a
// table keyed on the class name could only ever give one.
TEST(ClassBackgroundImageQueryTests, ResolvedUrlFollowsTheStylesheetThatDeclaredIt)
{
    EXPECT_EQ(ResolveIn(R"(.checkmark-icon { background-image: url("editor:Icons/todolist.png"); })",
                        "checkmark-icon"),
              "editor:Icons/todolist.png");

    // Same class, stylesheet moved elsewhere. Both the old convention
    // ("checkmark-icon" -> Icons/checkmark.png) and the old hardcoded
    // exception (-> Icons/todolist.png) fail this line.
    EXPECT_EQ(ResolveIn(R"(.checkmark-icon { background-image: url("editor:Icons/moved-away.svg"); })",
                        "checkmark-icon"),
              "editor:Icons/moved-away.svg");

    // And for a class the old convention got right, so the convention cannot
    // be what is passing above either.
    EXPECT_EQ(ResolveIn(R"(.stats-icon { background-image: url("project:Art/stats-replacement.png"); })",
                        "stats-icon"),
              "project:Art/stats-replacement.png");
}

TEST(ClassBackgroundImageQueryTests, KeepsTheSourceAliasAndOmitsItWhenUnauthored)
{
    EXPECT_EQ(ResolveIn(R"(.a-icon { background-image: url("editor:Icons/a.png"); })", "a-icon"),
              "editor:Icons/a.png");
    EXPECT_EQ(ResolveIn(R"(.b-icon { background-image: url("Icons/b.png"); })", "b-icon"),
              "Icons/b.png");
}

// A rule that only says how the class looks in some *context* does not say what
// the class is, so the probe denies every such context: it has no parent, no
// second class, no id, and rests in the default interaction state.
TEST(ClassBackgroundImageQueryTests, IgnoresStateDescendantAndForeignClassForms)
{
    EXPECT_TRUE(ResolveIn(R"(.a-icon:hover { background-image: url("editor:Icons/a.png"); })", "a-icon")
                    .empty());
    EXPECT_TRUE(ResolveIn(R"(.tab .a-icon { background-image: url("editor:Icons/a.png"); })", "a-icon")
                    .empty());
    EXPECT_TRUE(ResolveIn(R"(.a-icon.b-icon { background-image: url("editor:Icons/a.png"); })", "a-icon")
                    .empty());
    EXPECT_TRUE(ResolveIn(R"(#panel.a-icon { background-image: url("editor:Icons/a.png"); })", "a-icon")
                    .empty());

    // A selector group still resolves through its bare member — that is the
    // shape core.css ships (".x-icon, button.x-icon, .button.x-icon { ... }").
    EXPECT_EQ(ResolveIn(R"(.a-icon, button.a-icon, .button.a-icon { background-image: url("editor:Icons/a.png"); })",
                        "a-icon"),
              "editor:Icons/a.png");
}

// A type-qualified declaration is ordinary authoring here, not a corner case:
// core.css spells nearly every icon `.foo, button.foo, .button.foo`, so a class
// whose only declaration carries the tag or the `button` class still says which
// image the class is. Resolving it needs the real cascade — matching on the bare
// `.foo` spelling alone cannot see these rules, and cannot rank them either.
TEST(ClassBackgroundImageQueryTests, ResolvesTypeQualifiedDeclarations)
{
    EXPECT_EQ(ResolveIn(R"(button.a-icon { background-image: url("editor:Icons/a.png"); })", "a-icon"),
              "editor:Icons/a.png");
    EXPECT_EQ(ResolveIn(R"(.button.a-icon { background-image: url("editor:Icons/b.png"); })", "a-icon"),
              "editor:Icons/b.png");

    // Specificity, not source order, decides between them: `button.a-icon` is
    // (0,1,1) and outranks the bare (0,1,0) even when the bare one comes last.
    EXPECT_EQ(ResolveIn(R"(button.a-icon { background-image: url("editor:Icons/wins.png"); }
                           .a-icon { background-image: url("editor:Icons/loses.png"); })",
                        "a-icon"),
              "editor:Icons/wins.png");
}

// Every candidate is a bare class, so all carry specificity (0,1,0) and the
// cascade reduces to source order — later wins, within a sheet and across them.
TEST(ClassBackgroundImageQueryTests, LastDeclarationWinsWithinAndAcrossSheets)
{
    EXPECT_EQ(ResolveIn(R"(.a-icon { background-image: url("editor:Icons/first.png"); }
                           .a-icon { background-image: url("editor:Icons/second.png"); })",
                        "a-icon"),
              "editor:Icons/second.png");

    StylesheetHandle early = ParseSheet(R"(.a-icon { background-image: url("editor:Icons/early.png"); })");
    StylesheetHandle late = ParseSheet(R"(.a-icon { background-image: url("editor:Icons/late.png"); })");
    ASSERT_TRUE(early);
    ASSERT_TRUE(late);
    EXPECT_EQ(Resolve({early, late}, "a-icon"), "editor:Icons/late.png");
    EXPECT_EQ(Resolve({late, early}, "a-icon"), "editor:Icons/early.png");
}

TEST(ClassBackgroundImageQueryTests, ImportantOutranksALaterPlainDeclaration)
{
    EXPECT_EQ(ResolveIn(R"(.a-icon { background-image: url("editor:Icons/pinned.png") !important; }
                           .a-icon { background-image: url("editor:Icons/later.png"); })",
                        "a-icon"),
              "editor:Icons/pinned.png");
}

TEST(ClassBackgroundImageQueryTests, EmptyWhenNothingDeclaresAPath)
{
    EXPECT_TRUE(ResolveIn(R"(.a-icon { background-color: #ff0000; })", "a-icon").empty());
    EXPECT_TRUE(ResolveIn(R"(.b-icon { background-image: url("editor:Icons/b.png"); })", "a-icon").empty());
    EXPECT_TRUE(Resolve({}, "a-icon").empty());
    EXPECT_TRUE(ResolveIn(R"(.a-icon { background-image: url("editor:Icons/a.png"); })", "").empty());

    // A GUID reference resolves to an asset, not to an authored path, so there
    // is nothing to hand a context menu that takes a path string.
    EXPECT_TRUE(ResolveIn(R"(.a-icon { background-image: url("guid:00000000-0000-0000-0000-000000000001"); })",
                          "a-icon")
                    .empty());
}

// The six filenames the deleted table spelled out by hand. They were correct;
// this pins that reading them from CSS returns the same bytes, so the launcher
// did not quietly change icon when the table went away.
TEST(ClassBackgroundImageQueryTests, ShippedIconsMatchTheDeletedTablesValues)
{
    const std::vector<StylesheetHandle> cascade = ShippedGlobalCascade();
    ASSERT_GE(cascade.size(), 10u) << "theme.css cascade did not parse; the rest is vacuous";

    EXPECT_EQ(Resolve(cascade, "checkmark-icon"), "editor:Icons/todolist.png");
    EXPECT_EQ(Resolve(cascade, "script-icon"), "editor:Icons/log.png");
    EXPECT_EQ(Resolve(cascade, "dock-log-icon"), "editor:Icons/DockLog.svg");
    EXPECT_EQ(Resolve(cascade, "dock-script-errors-icon"), "editor:Icons/DockScriptErrors.svg");
    EXPECT_EQ(Resolve(cascade, "dock-shader-errors-icon"), "editor:Icons/DockShaderErrors.svg");
    EXPECT_EQ(Resolve(cascade, "dock-clip-editor-icon"), "editor:Icons/DockClipEditor.svg");

    // The panels the panelId -> path table used to cover, now authored classes.
    EXPECT_EQ(Resolve(cascade, "dock-assets-icon"), "editor:Icons/folder.png");
    EXPECT_EQ(Resolve(cascade, "dock-asset-view-icon"), "editor:Icons/eye.png");
    EXPECT_EQ(Resolve(cascade, "dock-hierarchy-icon"), "editor:Icons/list.png");
    EXPECT_EQ(Resolve(cascade, "dock-inspector-icon"), "editor:Icons/options.png");
    EXPECT_EQ(Resolve(cascade, "dock-scene-view-icon"), "editor:Icons/sceneicon.png");
    EXPECT_EQ(Resolve(cascade, "dock-game-view-icon"), "editor:Icons/game-controller.png");
    EXPECT_EQ(Resolve(cascade, "dock-ui-demo-icon"), "editor:Icons/desktop.png");
    EXPECT_EQ(Resolve(cascade, "dock-diff-icon"), "editor:Icons/codeicon.png");
    EXPECT_EQ(Resolve(cascade, "dock-web-icon"), "editor:Icons/computer.png");

    // The launcher's generic mark, for a row whose panel declares no class.
    EXPECT_EQ(Resolve(cascade, "menulist-icon"), "editor:Icons/menulist.png");
}
