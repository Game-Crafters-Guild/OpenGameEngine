// Three of the sky inspector's groups are whole subjects an author works on alone: the star
// field, the falling stars, and the knobs only the 2D backdrop reads. Each is a section of its
// own rather than a text sub-header inside Stylistic, because a sub-header is only a label — it
// cannot be collapsed and the panel keeps no state for it, so reaching the stars means scrolling
// past everything above them every time.
//
// This file locks that routing, and locks the groups that stay sub-headings inside Stylistic.
// Both halves matter: the first is the feature, the second is the reason the remaining
// sub-headers are not a bug.
//
// Source-level, for the same reason TerrainInspectorSectionStyleTests is: the sky inspector is a
// component inspector over a live World and RenderServices, so it cannot be built in this process.
// What is locked is the routing — which container each group's rows are added to — not the
// declarations. Restyle and reword freely.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadEditorFile(const std::string& relativePath)
{
    const std::filesystem::path path = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string SkyInspector()
{
    return ReadEditorFile("Source/Inspectors/SkyEnvironmentInspector.cpp");
}

// The source with every run of whitespace collapsed to one space and the space after an open
// parenthesis dropped, so a call reads the same whether or not it is wrapped across lines. The
// needles below are matched against this, and so pin the call rather than its indentation.
std::string Flattened(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    for (const char c : source)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!out.empty() && out.back() != ' ' && out.back() != '(')
                out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// Everything between two markers. The rows of a group live between its first row and the start of
// the next group, so scoping to that span is what makes "these rows went to that container" a
// falsifiable claim instead of a search that any single occurrence in the file satisfies.
std::string Span(const std::string& source, const std::string& from, const std::string& to)
{
    const size_t start = source.find(from);
    if (start == std::string::npos)
        return {};
    const size_t end = to.empty() ? source.size() : source.find(to, start);
    if (end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}

size_t CountOf(const std::string& haystack, const std::string& needle)
{
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// Every container a sky row is added to. A row names its container as the first argument of the
// call that adds it, so this is the form to search a span for.
constexpr const char* kSkyContainers[] = {"physical", "stylistic", "fallingStars", "stars",
                                          "backdrop2D"};

std::string AddedTo(const std::string& container)
{
    return "(" + container + ", \"";
}

} // namespace

// The three groups are sections, built by the SAME helper the two original sections use. Going
// through AddSkySection is the whole guarantee: it is what routes them to
// InspectorUI::AddComponentSection, and that is what makes them collapsible and remembered.
TEST(SkyInspectorSections, TheThreeGroupsAreSectionsBuiltByTheSharedHelper)
{
    const std::string panel = SkyInspector();
    ASSERT_FALSE(panel.empty()) << "the sky inspector source did not read; this test is vacuous";
    const std::string flat = Flattened(panel);

    for (const char* title : {"\"Falling stars\"", "\"Stars\"", "\"2D backdrop\""})
    {
        const std::string call = std::string("AddSkySection(ctx.Parent, ") + title;
        EXPECT_NE(flat.find(call), std::string::npos)
            << title << " is not built by AddSkySection, so it is not a collapsible section";
    }

    // AddSkySection is the sky's one-line wrapper over the inspector-wide helper. If it stops
    // routing there, all five sky sections quietly lose their remembered state at once.
    const std::string helperBody = Span(panel, "static Foldout* AddSkySection(", "\n}");
    ASSERT_FALSE(helperBody.empty())
        << "AddSkySection is gone or renamed; re-point this test before trusting it";
    EXPECT_NE(helperBody.find("InspectorUI::AddComponentSection"), std::string::npos)
        << "sky sections are built by hand instead of the shared inspector helper";
    // The key is what the remembered state is stored under, so it has to be per-section.
    EXPECT_NE(helperBody.find("\"SkyEnvironment/\" + std::string(title)"), std::string::npos)
        << "sky sections no longer key their remembered state on their own title";
}

// The shared helper is what remembers a section's open/shut state across the panel rebuilds that
// every component edit triggers. Pinned here because a section that reopens on every rebuild is a
// section nobody can close — which is exactly the complaint the split was meant to fix.
TEST(SkyInspectorSections, SectionsRememberTheirExpansionRatherThanHardcodingIt)
{
    const std::string helper = ReadEditorFile("Source/Inspectors/InspectorComponentSection.cpp");
    ASSERT_FALSE(helper.empty()) << "the shared section helper did not read; this test is vacuous";
    EXPECT_NE(helper.find("SetExpanded(expanded)"), std::string::npos)
        << "the shared helper no longer seeds a section from its remembered state";
    EXPECT_NE(helper.find("SetOnExpandedChanged"), std::string::npos)
        << "the shared helper no longer records the user's choice, so nothing is remembered";

    // And nothing in the sky panel pins an expansion to a literal. The panel does call
    // SetExpanded once, for the preset foldout, but it seeds that from a remembered flag too --
    // so the property to hold is "no hardcoded state", not "no call".
    const std::string panel = SkyInspector();
    ASSERT_FALSE(panel.empty());
    EXPECT_EQ(CountOf(panel, "SetExpanded(true)"), 0u)
        << "a sky foldout is pinned open, so it reopens on every panel rebuild";
    EXPECT_EQ(CountOf(panel, "SetExpanded(false)"), 0u)
        << "a sky foldout is pinned shut, so the user's choice to open it is discarded on every "
           "panel rebuild";
}

// Each section took its own rows with it. Scoped to the span the group occupies, so a row left
// behind on `stylistic` is caught rather than hidden by the container name appearing elsewhere.
TEST(SkyInspectorSections, EachSectionContainsTheRowsItTook)
{
    const std::string panel = SkyInspector();
    ASSERT_FALSE(panel.empty());
    const std::string flat = Flattened(panel);

    struct Group
    {
        const char* container;
        const char* firstRow;
        const char* endMarker;
        std::initializer_list<const char*> rows;
    };
    const Group groups[] = {
        {"fallingStars", "addSkyDiskToggle(fallingStars, \"Falling stars\"",
         "\"Star density\"",
         {"\"Falling amount\"", "\"Falling frequency\"", "\"Falling speed\"", "\"Falling length\"",
          "\"Falling thickness\"", "\"Falling dot size\""}},
        // The star rows end where the Sun Dir override rows begin. Those go to `physical`, so a
        // span that runs past them reads three of its rows as stars that landed elsewhere.
        {"stars", "AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, \"Star density\"",
         "if (!sky->AutoSunMoon)",
         {"\"Brightness\"", "\"Size\"", "\"Diamond Shape\"", "\"Core Size\"", "\"Glow Falloff\"",
          "\"Twinkle Speed\"", "\"Twinkle Intensity\""}},
        // Sun size 2D leads the group: it belongs with the other 2D-only knobs rather than beside
        // the sun's real size, because it is a screen fraction and not an angle.
        {"backdrop2D", "(backdrop2D, \"Sun size 2D\"", "",
         {"\"Sun size 2D\"", "\"Sky pan 2D\"", "\"Moon size 2D\"", "\"Falling dot size 2D\""}},
    };

    for (const Group& g : groups)
    {
        const std::string span = Span(flat, g.firstRow, g.endMarker);
        ASSERT_FALSE(span.empty()) << g.container << ": could not locate the group's rows";
        for (const char* row : g.rows)
            EXPECT_NE(span.find(row), std::string::npos)
                << g.container << " lost its " << row << " row";

        // Where each of those rows landed. Naming every container is what makes that falsifiable:
        // a row that kept the old shared parent, or picked up a neighbouring one, renders outside
        // the section its siblings moved into.
        for (const char* container : kSkyContainers)
        {
            if (std::string(container) == g.container)
                continue;
            EXPECT_EQ(CountOf(span, AddedTo(container)), 0u)
                << "a row inside the " << g.container << " group is added to " << container
                << " instead, so it renders outside its section while its siblings moved";
        }
        // The old shared container, in any call shape rather than only the first-argument one.
        EXPECT_EQ(CountOf(span, "stylistic"), 0u)
            << "a row inside the " << g.container
            << " group is still added to the Stylistic container, so it renders outside its "
               "section while its siblings moved";
    }
}

// The three text sub-headers the sections replaced are gone. Without this the split could be half
// done — a section AND the label it was supposed to replace, stacked.
TEST(SkyInspectorSections, TheReplacedSubHeadersAreGone)
{
    const std::string panel = SkyInspector();
    ASSERT_FALSE(panel.empty());

    for (const char* gone : {"AddTextBlock(stylistic, \"Falling stars\"",
                             "AddTextBlock(stylistic, \"Stars\"",
                             "AddTextBlock(stylistic, \"2D backdrop"})
    {
        EXPECT_EQ(panel.find(gone), std::string::npos)
            << "a replaced sub-header is still emitted alongside its section: " << gone;
    }
}

// What stays a sub-heading inside Stylistic, and why. Horizon rim and Below-horizon band are
// built inside a `BelowHorizonMode == StylizedGround` branch, so as sections they would appear
// and disappear as that dropdown changes, which is the churn this split avoids. Ambient gradient
// tint stays because it is a diffuse-only tint trio that belongs with the ambient rows above it.
//
// This is a real guard, not a comment: promoting them later without noticing the branch is the
// mistake, and it fails here with the reason attached.
TEST(SkyInspectorSections, TheModeConditionalGroupsStayAsSubHeadingsInsideStylistic)
{
    const std::string panel = SkyInspector();
    ASSERT_FALSE(panel.empty());

    for (const char* stays : {"AddTextBlock(stylistic, \"Horizon rim",
                              "AddTextBlock(stylistic, \"Below-horizon band",
                              "AddTextBlock(stylistic, \"Ambient gradient tint"})
    {
        EXPECT_NE(panel.find(stays), std::string::npos)
            << stays
            << " was promoted out of Stylistic. The first two are built inside the "
               "BelowHorizonMode == StylizedGround branch, so as sections they would appear and "
               "disappear with a dropdown; if that branch is gone, re-point this test.";
    }

    // The branch that is the reason. If it disappears, the reasoning above no longer holds and
    // this test should be revisited rather than silently kept.
    EXPECT_NE(panel.find("sky->BelowHorizonMode == Rendering::SkyBelowHorizonMode::StylizedGround"),
              std::string::npos)
        << "the mode branch these sub-headers live in is gone; revisit whether they should now be "
           "sections";
}

// The Sun illuminance row sits with the sun it edits, right under the Sun Light picker, and its
// logic is the engine's: which light it may edit, which light the sky reads, and lux to and from
// the light's unit all come from SkySunIlluminance, which EngineRenderServicesTests
// covers. The row file itself only presents. A row that grows its own conversion or its own light
// search is the drift this pins.
TEST(SkyInspectorSections, SunIlluminanceRowIsRoutedThroughTheHelper)
{
    const std::string panel = Flattened(SkyInspector());
    ASSERT_FALSE(panel.empty()) << "the sky inspector source did not read; this test is vacuous";

    const size_t picker = panel.find("AddEntityFieldRow(physical, \"Sun Light\"");
    const size_t row = panel.find("AddSkySunIlluminanceRow(physical, ctx,");
    ASSERT_NE(picker, std::string::npos) << "the Sun Light picker moved; re-point this test";
    ASSERT_NE(row, std::string::npos) << "the Sun illuminance row is not in the Physical section";
    EXPECT_GT(row, picker) << "the Sun illuminance row must follow the Sun Light picker";

    const std::string rowSource = Flattened(ReadEditorFile("Source/Inspectors/SkySunIlluminanceRow.cpp"));
    ASSERT_FALSE(rowSource.empty()) << "the row source did not read; this test is vacuous";
    for (const char* call : {"SunIlluminance::EditableSunLight(", "SunIlluminance::ResolvedSunLight(",
                             "SunIlluminance::LuxFromLightIntensity(", "SunIlluminance::LightIntensityFromLux(",
                             "AddComponentFloatRowWithDrag<Components::Light>(",
                             "\"Change Sun Illuminance\"", "ctx.SimulationRefreshCallbacks"})
        EXPECT_NE(rowSource.find(call), std::string::npos) << "the row no longer routes through " << call;

    for (const char* ownMath : {"kUnitlessPerLux", "kReferenceWhiteNits", "LightIntensityToUnitless(",
                                "Query<"})
        EXPECT_EQ(rowSource.find(ownMath), std::string::npos)
            << "the row does its own " << ownMath << " instead of asking SkySunIlluminance";
}

// Moonlight is the moon's light, so it sits in the Moon rows of the Physical section, under Show Moon,
// which hides it; a Moonlight row anywhere else is one an author looks for under the moon and does not
// find.
TEST(SkyInspectorSections, MoonlightIsRoutedInTheSunAndMoonGroup)
{
    const std::string panel = Flattened(SkyInspector());
    ASSERT_FALSE(panel.empty()) << "the sky inspector source did not read; this test is vacuous";

    const size_t moonHeader = panel.find("AddTextBlock(physical, \"Moon\"");
    const size_t showMoon = panel.find("addSkyDiskToggle(physical, \"Show Moon\"");
    const size_t moonlight = panel.find("AddSkyMoonlightRow(physical, ctx)");
    ASSERT_NE(moonHeader, std::string::npos) << "the Moon sub-header moved; re-point this test";
    ASSERT_NE(showMoon, std::string::npos) << "the Show Moon toggle moved; re-point this test";
    ASSERT_NE(moonlight, std::string::npos) << "the Moonlight row is not in the Physical section";
    EXPECT_GT(moonlight, showMoon) << "Moonlight must follow Show Moon";
    EXPECT_EQ(panel.find("AddTextBlock(", showMoon) > moonlight, true)
        << "Moonlight must sit in the Moon rows, before the next sub-header";
    EXPECT_EQ(CountOf(panel, "AddSkyMoonlightRow("), 1u) << "one Moonlight row";
}

// The ground albedo, brightness and night color feed the ground bounce in every Below Horizon mode
// (sky_capture_cube.frag, GE_GroundBounceRadiance), so the default Continue Horizon mode must show all
// three rows. The span runs from the color defaults the rows are built from to the night row's undo
// label: any mode condition guarding the three rows sits inside it.
TEST(SkyInspectorSections, GroundRowsShowInEveryBelowHorizonMode)
{
    const std::string panel = Flattened(SkyInspector());
    const std::string span =
        Span(panel, "MakeSkyDayDefaultColors(belowHorizonDarkAccess);", "\"Change Sky Ground Night\"");
    ASSERT_FALSE(span.empty()) << "the ground rows' anchors moved; update the span";
    EXPECT_NE(span.find("\"Ground albedo ramp\""), std::string::npos);
    EXPECT_NE(span.find("\"Ground brightness\""), std::string::npos);
    EXPECT_NE(span.find("\"Ground night ramp\""), std::string::npos);
    EXPECT_EQ(span.find("BelowHorizonMode"), std::string::npos)
        << "the ground rows are gated on the Below Horizon mode, but every mode bakes the ground bounce";
}
