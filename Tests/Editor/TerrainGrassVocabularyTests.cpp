// The grass Render Mode row's copy: the dropdown tooltip and the inactive note.
//
// Both are derived from ONE gating answer — TerrainGrassNeedsAlpha for this grass — whose
// own semantics are pinned in RenderPipelineDeclareTests. What is pinned HERE is the half
// that lives in the Inspector: that the row admits it resolves nothing when no alpha is
// live, that it names the fix rather than only the diagnosis, and that it scopes its claim
// to every active terrain rather than to this one.
//
// Copy is asserted by phrase rather than whole string, matching TerrainSurfaceRuleWidgetTests,
// so rewording stays cheap while the CLAIMS stay pinned. The one exception is the
// multi-terrain sentence, which is asserted byte-exact and says below why.
//
// Layout is not asserted at all: EditorTests runs no layout pass, which is why the copy is
// reachable as a plain string in the first place.

#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "Terrain/TerrainGrassVocabulary.h"

using namespace GameEngine;
namespace GrassVocab = GameEngine::Editor::TerrainGrassVocabulary;

namespace
{

std::string GrassLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool GrassHas(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// ---- The inactive note ----------------------------------------------------

TEST(TerrainGrassVocabulary, ALiveRowHasNothingToReport)
{
    EXPECT_TRUE(GrassVocab::RenderModeInactiveNote(/*needsAlpha=*/true).empty());
}

TEST(TerrainGrassVocabulary, NoLiveAlphaIsReportedAndNamesTheFix)
{
    const std::string note = GrassLower(GrassVocab::RenderModeInactiveNote(/*needsAlpha=*/false));
    ASSERT_FALSE(note.empty());
    EXPECT_TRUE(GrassHas(note, "no live alpha")) << note;      // the state
    EXPECT_TRUE(GrassHas(note, "opaque")) << note;             // what draws instead
    EXPECT_TRUE(GrassHas(note, "texture grass")) << note;      // the fix, not only the diagnosis
    EXPECT_TRUE(GrassHas(note, "alpha map")) << note;
}

// Byte-exact, not by phrase. This sentence is the note's SCOPE claim: the alpha reduction
// runs once per params upload over every active terrain rather than per view, so the row
// still counts while any other terrain needs the alpha path. That scoping is what justifies
// leaving the control enabled instead of disabling it, and it was measured and written down
// as committed evidence — a reword here would falsify that record silently.
TEST(TerrainGrassVocabulary, TheInactiveNoteScopesItselfToEveryActiveTerrain)
{
    const std::string note = GrassVocab::RenderModeInactiveNote(/*needsAlpha=*/false);
    EXPECT_TRUE(GrassHas(note,
        "One draw serves every active terrain, so this row still counts while any other "
        "terrain needs the alpha path."))
        << note;
}

// ---- The dropdown tooltip -------------------------------------------------

TEST(TerrainGrassVocabulary, TheTooltipDescribesBothModesInEitherState)
{
    for (const bool needsAlpha : {true, false})
    {
        const std::string tooltip = GrassLower(GrassVocab::RenderModeTooltip(needsAlpha));
        EXPECT_TRUE(GrassHas(tooltip, "dither")) << tooltip;
        EXPECT_TRUE(GrassHas(tooltip, "blend")) << tooltip;
        // The two modes differ by HOW coverage resolves, which is the whole reason to pick one.
        EXPECT_TRUE(GrassHas(tooltip, "alpha-to-coverage")) << tooltip;
        EXPECT_TRUE(GrassHas(tooltip, "placement order")) << tooltip;
    }
}

// The tooltip carries the inactive state too: a reader who hovers the control before
// noticing the note below it still learns the choice currently resolves nothing.
TEST(TerrainGrassVocabulary, TheTooltipAdmitsInactivityOnlyWhenNoAlphaIsLive)
{
    EXPECT_FALSE(GrassHas(GrassLower(GrassVocab::RenderModeTooltip(/*needsAlpha=*/true)),
                          "inactive"));

    const std::string inactive = GrassLower(GrassVocab::RenderModeTooltip(/*needsAlpha=*/false));
    EXPECT_TRUE(GrassHas(inactive, "inactive")) << inactive;
    EXPECT_TRUE(GrassHas(inactive, "opaque")) << inactive;
}

// The tooltip's live text is a PREFIX of its inactive text: the inactive state appends a
// sentence rather than rewriting the description, so the two can never drift apart.
TEST(TerrainGrassVocabulary, TheInactiveTooltipExtendsTheLiveOne)
{
    const std::string live = GrassVocab::RenderModeTooltip(/*needsAlpha=*/true);
    const std::string inactive = GrassVocab::RenderModeTooltip(/*needsAlpha=*/false);
    ASSERT_GT(inactive.size(), live.size());
    EXPECT_EQ(inactive.compare(0, live.size(), live), 0);
}

// ---- The Range row --------------------------------------------------------

// Range is a CEILING. The per-view instance budget shortens it when the authored density cannot be
// carried that far, so the tooltip must not promise it as hard.
TEST(TerrainGrassVocabulary, TheRangeTooltipDoesNotPromiseTheAuthoredDistance)
{
    const std::string tip = GrassLower(GrassVocab::RangeTooltip());
    EXPECT_FALSE(GrassHas(tip, "hard visibility range")) << tip;
    EXPECT_TRUE(GrassHas(tip, "ceiling")) << tip;
    EXPECT_TRUE(GrassHas(tip, "budget")) << tip;
}

// A range the budget did not touch has nothing to report: the authored number is the whole truth,
// and a note there would train the reader to ignore the one that matters.
TEST(TerrainGrassVocabulary, AnUnshortenedRangeSaysNothing)
{
    EXPECT_TRUE(GrassVocab::RangeFitNote(500.0f, 500.0f, /*textureCards=*/false).empty());
    EXPECT_TRUE(GrassVocab::RangeFitNote(150.0f, 150.0f, /*textureCards=*/true).empty());
    // A fit that reported nothing at all is not evidence that the range survived.
    EXPECT_TRUE(GrassVocab::RangeFitNote(500.0f, 0.0f, /*textureCards=*/false).empty());
}

// The note carries BOTH numbers, so the row does not merely say "shorter than you asked".
TEST(TerrainGrassVocabulary, AShortenedRangeNamesWhatIsDeliveredAndWhatWasAsked)
{
    const std::string note = GrassVocab::RangeFitNote(500.0f, 369.1f, /*textureCards=*/false);
    ASSERT_FALSE(note.empty());
    EXPECT_TRUE(GrassHas(note, "369 m")) << note;
    EXPECT_TRUE(GrassHas(note, "500 m")) << note;
    EXPECT_TRUE(GrassHas(GrassLower(note), "budget")) << note;
}

// The note names the density the fit was actually run on. The placement compute fits a texture-card
// terrain on cards/m2 and a geometric one on blades/m2, so a note that always said "blades" would
// point a card terrain's author at a dial that did not shorten anything. This is the half of that
// mismatch that lives in the copy; the fit input itself is chosen in TerrainInspector.
TEST(TerrainGrassVocabulary, TheShortenedNoteNamesTheModesOwnDensity)
{
    const std::string blades = GrassLower(GrassVocab::RangeFitNote(500.0f, 369.1f,
                                                                   /*textureCards=*/false));
    const std::string cards = GrassLower(GrassVocab::RangeFitNote(500.0f, 369.1f,
                                                                  /*textureCards=*/true));
    ASSERT_FALSE(blades.empty());
    ASSERT_FALSE(cards.empty());

    EXPECT_TRUE(GrassHas(blades, "blades per")) << blades;
    EXPECT_FALSE(GrassHas(blades, "cards per")) << blades;

    EXPECT_TRUE(GrassHas(cards, "cards per")) << cards;
    EXPECT_FALSE(GrassHas(cards, "blades per")) << cards;
}

// The unit is the typographic one the rest of the grass rows use, not the ASCII fallback: the
// inspector's own density row is labelled "blades / m²", and a note beside it reading "m2" reads
// as a different quantity.
TEST(TerrainGrassVocabulary, TheNoteUsesTheSquaredUnitGlyph)
{
    const std::string note = GrassVocab::RangeFitNote(500.0f, 369.1f, /*textureCards=*/false);
    ASSERT_FALSE(note.empty());
    // Explicit UTF-8 bytes: a u8"" literal is char8_t in C++20 and will not convert to
    // std::string, and a raw glyph here would depend on the source encoding surviving.
    EXPECT_TRUE(GrassHas(note, "m\xc2\xb2")) << note;
    EXPECT_FALSE(GrassHas(note, "m2")) << note;
}
