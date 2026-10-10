#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Panels/UIDemoPanel.h"
#include "Panels/UndoHistoryPanel.h"
#include "UI/ClassBackgroundImageQuery.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

using namespace GameEngine;

// ---------------------------------------------------------------------------
// Behavioural: a panel constructed the way the built-in panel table constructs
// one (a bare `std::make_unique<T>()`, never touching layout.uxml) must still
// report its type's tab icon.
// ---------------------------------------------------------------------------

TEST(PanelDefaultTabIcon, FallbackConstructedUIDemoPanelReportsAnIcon)
{
    UIDemoPanel panel;
    EXPECT_FALSE(panel.GetTabIcon().empty());
    EXPECT_EQ(panel.GetTabIcon(), "dock-ui-demo-icon");
}

TEST(PanelDefaultTabIcon, FallbackConstructedUndoHistoryPanelReportsAnIcon)
{
    UndoHistoryPanel panel;
    EXPECT_FALSE(panel.GetTabIcon().empty());
    EXPECT_EQ(panel.GetTabIcon(), "undo-icon");
}

// An explicit override (a dock-config icon= attribute or a package panel
// descriptor's TabIconClass) must keep beating the type default.
TEST(PanelDefaultTabIcon, ExplicitIconOverridesRealPanelDefault)
{
    UIDemoPanel panel;
    panel.SetTabIcon("dock-assets-icon");
    EXPECT_EQ(panel.GetTabIcon(), "dock-assets-icon");
}

// ---------------------------------------------------------------------------
// Structural: the type default is the sole authority for which icon class a
// panel wears. Every panel type the built-in panel table can build must have
// one, layout.uxml must not restate them, and every class must resolve to an
// image in the shipped stylesheets. This is what stops a panel added later
// from silently regressing to no icon — or to a class no CSS declares.
// ---------------------------------------------------------------------------

namespace {

// Panel types deliberately shipped without a tab icon anywhere they appear. Keep
// this list short and justify every entry.
//
// This is not the lever for silencing one panel in one layout — icon="" in the
// dock config does that, per instance, without a C++ change. An entry here says
// something stronger: the type itself has no icon to declare, so every instance
// of it is bare. That claim still needs the guard below, or a panel added later
// regresses to no icon by simply forgetting to declare one.
const std::set<std::string> kIntentionallyIconless = {};

// Panel types whose default depends on instance state rather than on the type,
// so the override is defined in a .cpp instead of inline in the header.
// AnimationWindowPanel is directly constructible as any PanelKind, and the two
// derived panels only fix the kind, so one kind-keyed override serves all three.
const std::map<std::string, std::string> kKindResolvedIcons = {
    {"AnimationWindowPanel", "alarm-icon"},
    {"AnimationTimelinePanel", "film-icon"},
    {"AnimationClipEditorPanel", "dock-clip-editor-icon"},
};

std::filesystem::path EditorSourceRoot()
{
    return std::filesystem::path(GE_EDITOR_SOURCE_DIR);
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Built-in panel factories that are named functions rather than MakePanel<Class>,
// with the class each one builds. A row naming a factory missing here is reported
// by SourceScannersFindTheExpectedInputs, so a new one cannot skip the icon checks.
const std::map<std::string, std::string> kNamedPanelFactoryClasses = {
    {"MakeMaterialGraphPanel", "GraphPanel"},
    {"MakeAnimationGraphPanel", "GraphPanel"},
    {"MakeGameLogicGraphPanel", "GraphPanel"},
};

// Body of kBuiltInPanelTypes, the table of every panel type a dock-config row may name.
std::string PanelTypeTableBody()
{
    const std::string src = ReadFile(EditorSourceRoot() / "Source" / "Panels" / "BuiltInPanelTypes.cpp");
    const size_t start = src.find("kBuiltInPanelTypes[] = {");
    if (start == std::string::npos)
        return {};
    const size_t end = src.find("\n};", start);
    return end == std::string::npos ? std::string{} : src.substr(start, end - start);
}

struct PanelTypeRows
{
    std::map<std::string, std::string> classByTypeKey; // typeKey -> concrete panel class
    std::vector<std::string> unknownFactories;         // rows naming an unlisted factory
};

// Rows are `{"TypeKey", &MakePanel<Class>}` or `{"TypeKey", &NamedFactory}`.
PanelTypeRows ParsePanelTypeRows()
{
    PanelTypeRows out;
    const std::string body = PanelTypeTableBody();
    const std::string rowStart = "{\"";
    size_t pos = 0;
    while ((pos = body.find(rowStart, pos)) != std::string::npos)
    {
        pos += rowStart.size();
        const size_t keyEnd = body.find('"', pos);
        if (keyEnd == std::string::npos)
            break;
        const std::string key = body.substr(pos, keyEnd - pos);

        const size_t amp = body.find('&', keyEnd);
        const size_t rowEnd = body.find('}', keyEnd);
        if (amp == std::string::npos || rowEnd == std::string::npos || amp > rowEnd)
            break;
        const std::string factory = body.substr(amp + 1, rowEnd - amp - 1);

        const std::string templatePrefix = "MakePanel<";
        if (factory.compare(0, templatePrefix.size(), templatePrefix) == 0)
        {
            const size_t clsEnd = factory.find('>');
            out.classByTypeKey[key] = factory.substr(templatePrefix.size(), clsEnd - templatePrefix.size());
        }
        else if (const auto named = kNamedPanelFactoryClasses.find(factory);
                 named != kNamedPanelFactoryClasses.end())
        {
            out.classByTypeKey[key] = named->second;
        }
        else
        {
            out.unknownFactories.push_back(key + " -> " + factory);
        }
        pos = rowEnd;
    }
    return out;
}

// typeKey string -> concrete panel class the table builds for it.
std::map<std::string, std::string> FactoryTypeKeys()
{
    return ParsePanelTypeRows().classByTypeKey;
}

struct ClassInfo {
    std::string base;      // first base class named after `: public`
    std::string inlineIcon; // icon literal from an inline override, if any
    bool declaresOverride = false;
};

// Scans every panel header once. A class's region runs from its `class X` line to
// the next top-level `class ` in the same header, which is how these headers are
// laid out (one panel per header, plus small derived panels appended).
std::map<std::string, ClassInfo> ScanPanelHeaders()
{
    std::map<std::string, ClassInfo> out;
    const std::filesystem::path dir = EditorSourceRoot() / "Include" / "Panels";
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".h")
            continue;
        const std::string text = ReadFile(entry.path());

        std::vector<std::pair<size_t, std::string>> classes;
        size_t pos = 0;
        while ((pos = text.find("\nclass ", pos)) != std::string::npos)
        {
            const size_t nameStart = pos + std::string("\nclass ").size();
            size_t nameEnd = nameStart;
            while (nameEnd < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[nameEnd])) || text[nameEnd] == '_'))
                ++nameEnd;
            // Skip forward declarations (`class Foo;`).
            const size_t term = text.find_first_not_of(" \t\r\n", nameEnd);
            if (term != std::string::npos && text[term] != ';')
                classes.emplace_back(nameStart, text.substr(nameStart, nameEnd - nameStart));
            pos = nameEnd;
        }

        for (size_t i = 0; i < classes.size(); ++i)
        {
            const size_t begin = classes[i].first;
            const size_t end = (i + 1 < classes.size()) ? classes[i + 1].first : text.size();
            const std::string region = text.substr(begin, end - begin);

            ClassInfo info;
            const size_t basePos = region.find(": public ");
            if (basePos != std::string::npos)
            {
                size_t s = basePos + std::string(": public ").size();
                size_t e = s;
                while (e < region.size() &&
                       (std::isalnum(static_cast<unsigned char>(region[e])) || region[e] == '_' ||
                        region[e] == ':'))
                    ++e;
                info.base = region.substr(s, e - s);
            }
            const size_t ovPos = region.find("DeclaredTabIconClass");
            if (ovPos != std::string::npos)
            {
                info.declaresOverride = true;
                const size_t retPos = region.find("return \"", ovPos);
                const size_t lineEnd = region.find('\n', ovPos);
                if (retPos != std::string::npos && (lineEnd == std::string::npos || retPos < lineEnd))
                {
                    const size_t s = retPos + std::string("return \"").size();
                    const size_t e = region.find('"', s);
                    if (e != std::string::npos)
                        info.inlineIcon = region.substr(s, e - s);
                }
            }
            out[classes[i].second] = info;
        }
    }
    return out;
}

// Walks up the base chain until a class supplies a default.
std::string ResolveIcon(const std::map<std::string, ClassInfo>& classes, const std::string& cls,
                        bool& kindResolved)
{
    std::string current = cls;
    for (int guard = 0; guard < 8 && !current.empty(); ++guard)
    {
        auto it = classes.find(current);
        if (it == classes.end())
            return {};
        if (!it->second.inlineIcon.empty())
            return it->second.inlineIcon;
        if (it->second.declaresOverride)
        {
            kindResolved = true; // declared here, defined in the .cpp
            return {};
        }
        current = it->second.base;
    }
    return {};
}

// <DockablePanel .../> elements as authored in layout.uxml.
std::vector<std::string> LayoutDockablePanelElements()
{
    std::vector<std::string> out;
    const std::string text = ReadFile(EditorSourceRoot() / "Assets" / "UI" / "layout.uxml");
    size_t pos = 0;
    while ((pos = text.find("<DockablePanel", pos)) != std::string::npos)
    {
        const size_t end = text.find("/>", pos);
        if (end == std::string::npos)
            break;
        out.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

GameEngine::StylesheetHandle ParseSheet(const std::string& cssText)
{
    auto sheet = std::make_shared<GameEngine::Stylesheet>();
    if (!GameEngine::UIParsing::CSSParser::ParseStylesFromString(cssText, *sheet))
        return nullptr;
    return sheet;
}

// The editor's global cascade as authored: theme.css is @import statements
// only, flattened in source order the same way UIManager builds the cascade
// the launcher resolves icons against.
std::vector<GameEngine::StylesheetHandle> AuthoredGlobalCascade()
{
    const std::string master = ReadFile(EditorSourceRoot() / "Assets" / "UI" / "theme.css");
    EXPECT_FALSE(master.empty()) << "Assets/UI/theme.css not found in the editor source tree";

    std::vector<GameEngine::StylesheetHandle> cascade;
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
        const std::string css = ReadFile(EditorSourceRoot() / "Assets" / "UI" / relative);
        EXPECT_FALSE(css.empty()) << "imported sheet missing: " << relative;
        if (GameEngine::StylesheetHandle sheet = ParseSheet(css))
            cascade.push_back(std::move(sheet));
    }
    return cascade;
}

} // namespace

// Guards the parsers themselves: if these go silent the checks below would pass
// vacuously.
TEST(PanelDefaultTabIcon, SourceScannersFindTheExpectedInputs)
{
    const PanelTypeRows rows = ParsePanelTypeRows();
    EXPECT_GE(rows.classByTypeKey.size(), 30u);
    EXPECT_TRUE(rows.unknownFactories.empty())
        << "panel type rows naming a factory kNamedPanelFactoryClasses does not map to a class: "
        << ::testing::PrintToString(rows.unknownFactories);
    EXPECT_GE(LayoutDockablePanelElements().size(), 30u);
    EXPECT_GE(ScanPanelHeaders().size(), 30u);
}

// A panel type added to the factory later cannot silently ship without an icon.
TEST(PanelDefaultTabIcon, EveryFactoryPanelTypeHasADefault)
{
    const auto classes = ScanPanelHeaders();
    std::vector<std::string> missing;

    for (const auto& [typeKey, cls] : FactoryTypeKeys())
    {
        if (kIntentionallyIconless.count(typeKey))
            continue;
        bool kindResolved = false;
        const std::string icon = ResolveIcon(classes, cls, kindResolved);
        if (icon.empty() && !kindResolved)
            missing.push_back(typeKey + " (" + cls + ")");
    }

    EXPECT_TRUE(missing.empty())
        << "Panel types with no DeclaredTabIconClass override and not listed as "
           "intentionally icon-less: "
        << ::testing::PrintToString(missing);
}

// The type declaration is the one hand-authored copy of "which class this panel
// wears". layout.uxml naming a class would recreate a second copy that has to be
// kept in character-for-character sync by hand, so the shipped default layout
// must name none. icon="" is exempt and deliberately so: it names no class, it
// suppresses one, which is the only thing markup cannot say any other way.
TEST(PanelDefaultTabIcon, LayoutUxmlRestatesNoIconClasses)
{
    std::vector<std::string> restating;
    for (const std::string& el : LayoutDockablePanelElements())
    {
        const size_t at = el.find("icon=\"");
        if (at == std::string::npos)
            continue;
        if (el.compare(at, std::string("icon=\"\"").size(), "icon=\"\"") == 0)
            continue; // explicit suppression, not a restated class
        restating.push_back(el.substr(0, el.find('\n')));
    }
    EXPECT_TRUE(restating.empty())
        << "layout.uxml names an icon class on panels that already declare one; "
           "the type's DeclaredTabIconClass() is the single source (icon=\"\" to "
           "suppress is fine): "
        << ::testing::PrintToString(restating);
}

// Each default must also resolve to an image: a class no shipped stylesheet
// declares a background-image for would render an iconless tab and an iconless
// (well, generic) launcher row without anything failing.
TEST(PanelDefaultTabIcon, EveryDefaultIconClassResolvesInAuthoredCss)
{
    const std::vector<StylesheetHandle> cascade = AuthoredGlobalCascade();
    ASSERT_GE(cascade.size(), 10u) << "theme.css cascade did not parse; the rest is vacuous";

    const auto classes = ScanPanelHeaders();
    std::vector<std::string> unresolved;

    for (const auto& [typeKey, cls] : FactoryTypeKeys())
    {
        if (kIntentionallyIconless.count(typeKey))
            continue;
        bool kindResolved = false;
        std::string icon = ResolveIcon(classes, cls, kindResolved);
        if (kindResolved)
        {
            auto kr = kKindResolvedIcons.find(typeKey);
            if (kr == kKindResolvedIcons.end())
            {
                unresolved.push_back(typeKey + ": kind-resolved but not listed");
                continue;
            }
            icon = kr->second;
        }
        if (icon.empty())
            continue; // EveryFactoryPanelTypeHasADefault reports these
        if (UIStyleQuery::ResolveClassBackgroundImageUrl(cascade, icon).empty())
            unresolved.push_back(typeKey + ": '." + icon +
                                 "' has no background-image in any shipped stylesheet");
    }

    EXPECT_TRUE(unresolved.empty()) << ::testing::PrintToString(unresolved);
}

// The kind-keyed defaults are defined in AnimationWindowPanel.cpp, so the header
// scan cannot see them; assert the literals really are there.
TEST(PanelDefaultTabIcon, KindResolvedIconsExistInAnimationPanelSource)
{
    const std::string src =
        ReadFile(EditorSourceRoot() / "Source" / "Panels" / "AnimationWindowPanel.cpp");
    ASSERT_FALSE(src.empty());
    for (const auto& [typeKey, icon] : kKindResolvedIcons)
        EXPECT_NE(src.find("\"" + icon + "\""), std::string::npos)
            << typeKey << " expects icon literal " << icon;
}
