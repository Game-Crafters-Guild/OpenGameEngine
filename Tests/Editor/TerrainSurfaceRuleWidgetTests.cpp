// The Surface Rules condition widget's copy, its per-row state, and the arm that
// holds the terrain preview-cadence throttle open for a drag.
//
// Three things are pinned here, none of them decoration:
//
//   * The UNIT copy. Two of the five condition kinds exist only so a migrated
//     normalized domain is what a scene may already carry, and they measure something the other two do
//     not — picking the wrong one is silent and only shows up as a wrong-looking
//     terrain. Height in metres in particular is measured from the TERRAIN'S
//     BASE; calling it world Y would be wrong in exactly the way an author
//     discovers after moving a terrain and watching the band stay put.
//   * The per-row IGNORED state. The Shape::Global review's finding was that a
//     row which does nothing must say so AT THE ROW, because at a real Inspector
//     width a summary at the top of the effect is off screen.
//   * The arm's COUNT. A missed release leaves the throttle armed for every
//     later edit, so the terrain never settles again — the failure is silent,
//     permanent, and nowhere near the widget that caused it.
//
// Copy is asserted by phrase, not whole string, so rewording stays cheap while
// the CLAIMS stay pinned. Layout is not asserted at all: EditorTests runs no
// layout pass, which is why every string here is reachable without one.

#include <gtest/gtest.h>

#include <cctype>
#include <memory>
#include <string>

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Inspectors/TerrainRuleNotices.h"
#include "Terrain/TerrainInteractiveModifierEdit.h"
#include "Terrain/TerrainRuleConditionVocabulary.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::Editor::TerrainInteractiveModifierEditArm;
using GameEngine::Editor::TerrainInteractiveModifierEditArmCount;

namespace Components = GameEngine::Components;
namespace Vocab = GameEngine::Editor::TerrainRuleVocabulary;

using Kind = Components::TerrainRuleConditionKind;
using Curve = Components::TerrainRuleFalloffCurve;

namespace
{

constexpr Kind kAllKinds[] = {Kind::SlopeDegrees, Kind::SlopeNormalized, Kind::HeightMetres,
                              Kind::HeightNormalized, Kind::Noise};

std::string Lower(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The whole subtree, not the host's own row: an info card is a control that owns
// the elements its copy lives on, so what a notice added is not always a direct
// child of the host. Both readings below walk it through here, so neither can
// miss a set of elements the other sees.
template <typename Fn>
void ForEachDescendant(const UIElement& parent, Fn& fn)
{
    for (const auto& child : parent.GetChildren())
    {
        if (!child)
            continue;
        fn(*child);
        ForEachDescendant(*child, fn);
    }
}

// Every label the notice added, lowercased so a phrase assertion does not also
// pin capitalization. An empty label says nothing and is skipped: a notice with
// no action still carries its hidden action button, whose label is empty.
std::string AllTextIn(const UIElement& parent)
{
    std::string all;
    auto collect = [&all](const UIElement& element)
    {
        const auto* label = dynamic_cast<const Label*>(&element);
        if (label && !label->GetText().empty())
        {
            all += label->GetText();
            all += "\n";
        }
    };
    ForEachDescendant(parent, collect);
    return Lower(std::move(all));
}

std::size_t WarningCountIn(const UIElement& parent)
{
    std::size_t n = 0;
    auto count = [&n](const UIElement& element)
    {
        if (dynamic_cast<const GameEngine::EditorUI::InspectorNotice*>(&element))
            ++n;
    };
    ForEachDescendant(parent, count);
    return n;
}

bool Has(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

Components::TerrainRuleCondition MakeCondition(Kind kind, float min, float max, float feather)
{
    Components::TerrainRuleCondition condition{};
    condition.Kind = kind;
    condition.Min = min;
    condition.Max = max;
    condition.Feather = feather;
    return condition;
}

// A row that writes: full strength, one ordinary band.
Components::TerrainSurfaceRule MakeLiveRule()
{
    Components::TerrainSurfaceRule rule{};
    rule.Strength = 1.0f;
    rule.ConditionCount = 1;
    rule.Conditions[0] = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    return rule;
}

// The rule a caption is read against. Spelled out at every call site rather than
// defaulted, because strength AND mode both change what the closing sentence
// claims — a band-wording test that silently inherited one of them would be
// asserting on copy it never chose.
Components::TerrainSurfaceRule MakeOwningRule(float strength = 1.0f, bool replace = false)
{
    Components::TerrainSurfaceRule rule{};
    rule.Strength = strength;
    rule.Replace = replace;
    return rule;
}

} // namespace

// ---- Unit copy -------------------------------------------------------------

// The recorded engine finding, pinned as copy: HeightMetres is measured from the
// terrain's base, and the terrain entity's own Y translation is not part of it.
//
// The assertion is on the CLAIM, not on the substring "world y". The copy is
// allowed to contain that phrase — and does — because DISCLAIMING it is the
// strongest way to carry the finding, which is what the shipped component
// header's own comment does ("METRES ABOVE THE TERRAIN'S BASE - not world-space
// Y"). What must never appear is an affirmative reading.
TEST(TerrainRuleVocabulary, HeightMetresIsAboveTheTerrainBaseAndDisclaimsWorldY)
{
    const std::string help = Lower(std::string(Vocab::ConditionKindHelp(Kind::HeightMetres)));
    EXPECT_TRUE(Has(help, "metres above the terrain's base")) << help;
    EXPECT_TRUE(Has(help, "not world y")) << help;
    for (const char* affirmative : {"is world y", "world y height", "world-space y height"})
        EXPECT_FALSE(Has(help, affirmative)) << affirmative << " in: " << help;
}

// The class version: no kind may claim to measure world Y, not only the one that
// would be most wrong. Noise legitimately names world XZ — it samples a field in
// world space — so the ban is on the vertical claim specifically.
TEST(TerrainRuleVocabulary, NoConditionKindClaimsToMeasureWorldY)
{
    for (const Kind kind : kAllKinds)
    {
        const std::string help = Lower(std::string(Vocab::ConditionKindHelp(kind)));
        for (const char* affirmative : {"is world y", "world y height", "world-space y height",
                                        "above world y", "in world y"})
            EXPECT_FALSE(Has(help, affirmative))
                << static_cast<int>(kind) << " claims " << affirmative << ": " << help;
    }
}

TEST(TerrainRuleVocabulary, EveryKindHasALabelAndAnExplanation)
{
    for (const Kind kind : kAllKinds)
    {
        EXPECT_FALSE(Vocab::ConditionKindLabel(kind).empty()) << static_cast<int>(kind);
        EXPECT_FALSE(Vocab::ConditionKindHelp(kind).empty()) << static_cast<int>(kind);
    }
}

// The pairs differ only by unit, and picking the wrong one is silent. Their
// labels must therefore be distinct AND must each name a unit — a dropdown
// showing "Slope" twice is the whole failure.
TEST(TerrainRuleVocabulary, UnitPairsAreDistinguishableInAPicker)
{
    const std::string slopeDeg = Lower(std::string(Vocab::ConditionKindLabel(Kind::SlopeDegrees)));
    const std::string slopeRaw =
        Lower(std::string(Vocab::ConditionKindLabel(Kind::SlopeNormalized)));
    const std::string heightM = Lower(std::string(Vocab::ConditionKindLabel(Kind::HeightMetres)));
    const std::string heightN =
        Lower(std::string(Vocab::ConditionKindLabel(Kind::HeightNormalized)));

    EXPECT_NE(slopeDeg, slopeRaw);
    EXPECT_NE(heightM, heightN);
    EXPECT_TRUE(Has(slopeDeg, "degrees")) << slopeDeg;
    EXPECT_TRUE(Has(heightM, "metres")) << heightM;
    EXPECT_TRUE(Has(slopeRaw, "0-1")) << slopeRaw;
    EXPECT_TRUE(Has(heightN, "0-1")) << heightN;
}

// The migration-exact kinds have to say why they are on the menu at all, or an
// author picks one for its shorter name and silently gets the normalized
// squashed domain instead of an angle.
TEST(TerrainRuleVocabulary, RawSlopeHelpSaysItIsNotAnAngle)
{
    const std::string help = Lower(std::string(Vocab::ConditionKindHelp(Kind::SlopeNormalized)));
    EXPECT_TRUE(Has(help, "not an angle")) << help;
    EXPECT_TRUE(Has(help, "degrees")) << help; // names where to go instead
}

TEST(TerrainRuleVocabulary, NormalizedHeightHelpSaysTheBandChasesTheHeightfield)
{
    const std::string help = Lower(std::string(Vocab::ConditionKindHelp(Kind::HeightNormalized)));
    EXPECT_TRUE(Has(help, "chases")) << help;
}

// ---- Domains ---------------------------------------------------------------

TEST(TerrainRuleVocabulary, AuthoringDomainsMatchTheKindsUnit)
{
    constexpr float kHeightScale = 512.0f;

    const auto degrees = Vocab::ConditionAuthoringDomain(Kind::SlopeDegrees, kHeightScale);
    EXPECT_FLOAT_EQ(degrees.Min, 0.0f);
    EXPECT_FLOAT_EQ(degrees.Max, 90.0f);

    const auto metres = Vocab::ConditionAuthoringDomain(Kind::HeightMetres, kHeightScale);
    EXPECT_FLOAT_EQ(metres.Min, 0.0f);
    EXPECT_FLOAT_EQ(metres.Max, kHeightScale);

    for (const Kind kind : {Kind::SlopeNormalized, Kind::HeightNormalized, Kind::Noise})
    {
        const auto unit = Vocab::ConditionAuthoringDomain(kind, kHeightScale);
        EXPECT_FLOAT_EQ(unit.Min, 0.0f) << static_cast<int>(kind);
        EXPECT_FLOAT_EQ(unit.Max, 1.0f) << static_cast<int>(kind);
    }
}

// A rules effect can be authored before any terrain exists, and a zero-width
// domain is a slider whose handles cannot move — a dead control, not an empty one.
TEST(TerrainRuleVocabulary, MetresDomainStaysUsableWithNoTerrainToReadAHeightFrom)
{
    for (const float heightScale : {0.0f, -1.0f})
    {
        const auto domain = Vocab::ConditionAuthoringDomain(Kind::HeightMetres, heightScale);
        EXPECT_GT(domain.Max, domain.Min) << "height scale " << heightScale;
    }
}

// ---- Band summary ----------------------------------------------------------

TEST(TerrainRuleVocabulary, OrdinaryBandSummaryNamesBothEdgesAndTheFeather)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule()));
    EXPECT_TRUE(Has(summary, "34")) << summary;
    EXPECT_TRUE(Has(summary, "90")) << summary;
    EXPECT_TRUE(Has(summary, "8")) << summary;
    EXPECT_TRUE(Has(summary, "feather")) << summary;
}

// The feather's reach is stated as a WIDTH. Stating it as the two absolute
// values it implies puts numbers outside the domain on screen for any band
// touching an end — a slope band to 90 degrees with an 8 degree feather would
// "reach" 98 — and a value no handle can travel to reads as a bug.
TEST(TerrainRuleVocabulary, FeatherReachIsAWidthNotAnOutOfDomainValue)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule()));
    EXPECT_FALSE(Has(summary, "98")) << summary;
}

// The curve default is load-bearing — linear is the only shape that reproduces
// the normalized domain — so the summary has to say which one is in force rather than
// leaving it to a dropdown several rows away.
TEST(TerrainRuleVocabulary, BandSummaryNamesTheFeatherShape)
{
    auto linear = MakeCondition(Kind::HeightNormalized, 0.2f, 0.8f, 0.1f);
    EXPECT_TRUE(Has(Lower(Vocab::ConditionBandSummary(linear, MakeOwningRule())), "linear"));

    auto smooth = linear;
    smooth.FalloffCurve = Curve::Smoothstep;
    EXPECT_TRUE(Has(Lower(Vocab::ConditionBandSummary(smooth, MakeOwningRule())), "smoothstep"));
}

TEST(TerrainRuleVocabulary, HardEdgedBandSaysSoRatherThanNamingAZeroFeather)
{
    const auto condition = MakeCondition(Kind::HeightMetres, 100.0f, 400.0f, 0.0f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule()));
    EXPECT_TRUE(Has(summary, "hard edge")) << summary;
}

// A degenerate band fires hardest at one exact value and fades both ways, and it is a
// peak rather than a range — describing it as "from 0.3 to 0.3" reads as broken.
TEST(TerrainRuleVocabulary, DegenerateBandReadsAsAPeak)
{
    const auto condition = MakeCondition(Kind::SlopeNormalized, 0.3f, 0.3f, 0.2f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule()));
    EXPECT_TRUE(Has(summary, "peak")) << summary;
}

// An inverted band with no feather can never be satisfied, so its summary is
// where the fix gets named — both ways out, not just the diagnosis.
TEST(TerrainRuleVocabulary, EmptyBandSummaryNamesBothWaysOut)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 70.0f, 30.0f, 0.0f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule()));
    EXPECT_TRUE(Has(summary, "empty band")) << summary;
    EXPECT_TRUE(Has(summary, "handle")) << summary;
    EXPECT_TRUE(Has(summary, "feather")) << summary;
}

// ---- The per-row ignored state --------------------------------------------

TEST(TerrainRuleState, ARowThatWritesHasNothingToReport)
{
    EXPECT_TRUE(Vocab::RuleInertReason(MakeLiveRule()).empty());
}

TEST(TerrainRuleState, ZeroStrengthIsReportedAndNamesTheFix)
{
    auto rule = MakeLiveRule();
    rule.Strength = 0.0f;

    const std::string reason = Lower(Vocab::RuleInertReason(rule));
    ASSERT_FALSE(reason.empty());
    EXPECT_TRUE(Has(reason, "ignored")) << reason;
    EXPECT_TRUE(Has(reason, "strength")) << reason;
    EXPECT_TRUE(Has(reason, "raise")) << reason; // the fix, not only the diagnosis
}

// An inverted band with no feather returns 0 for every possible value, so the
// whole row multiplies to nothing. The message numbers the condition the way the
// Inspector labels it — 1-based — because "condition 0" names no visible row.
TEST(TerrainRuleState, AnEmptyBandWithNoFeatherKillsTheRowAndIsNumberedFromOne)
{
    auto rule = MakeLiveRule();
    rule.ConditionCount = 2;
    rule.Conditions[1] = MakeCondition(Kind::HeightMetres, 400.0f, 100.0f, 0.0f);

    const std::string reason = Lower(Vocab::RuleInertReason(rule));
    ASSERT_FALSE(reason.empty());
    EXPECT_TRUE(Has(reason, "ignored")) << reason;
    EXPECT_TRUE(Has(reason, "condition 2")) << reason;
    EXPECT_FALSE(Has(reason, "condition 1")) << reason;
    EXPECT_TRUE(Has(reason, "feather")) << reason;
}

// The complement, so the test above cannot pass by the check firing on any
// inverted band: WITH a feather the two ramps still carry weight, so the row is
// live and flagging it would name a cause that is not one.
TEST(TerrainRuleState, AnEmptyBandWithAFeatherIsStillLive)
{
    auto rule = MakeLiveRule();
    rule.Conditions[0] = MakeCondition(Kind::HeightMetres, 400.0f, 100.0f, 25.0f);
    EXPECT_TRUE(Vocab::RuleInertReason(rule).empty());
}

// Red arm for the count: a dead condition parked BEYOND ConditionCount is not
// evaluated by the bake, so reporting it would flag a row that writes fine.
TEST(TerrainRuleState, ConditionsPastTheCountAreNotInspected)
{
    auto rule = MakeLiveRule();
    rule.ConditionCount = 1;
    rule.Conditions[1] = MakeCondition(Kind::HeightMetres, 400.0f, 100.0f, 0.0f);
    EXPECT_TRUE(Vocab::RuleInertReason(rule).empty());

    // ...and it IS reported the moment the count reaches it, so the test above
    // cannot pass by the scan never running.
    rule.ConditionCount = 2;
    EXPECT_FALSE(Vocab::RuleInertReason(rule).empty());
}

// ---- Notices, and where they render ---------------------------------------

TEST(TerrainRuleNotice, AnEffectWithNoRulesSaysSoAndNamesTheButton)
{
    UIElement host;
    GameEngine::Editor::AddSurfaceRulesEmptyNotice(&host, 0);

    const std::string text = AllTextIn(host);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(WarningCountIn(host), 1u);
    EXPECT_TRUE(Has(text, "add rule")) << text;
}

TEST(TerrainRuleNotice, AnEffectWithRulesIsQuiet)
{
    UIElement host;
    GameEngine::Editor::AddSurfaceRulesEmptyNotice(&host, 1);
    EXPECT_TRUE(AllTextIn(host).empty());
}

// The state-at-site rule: the reason renders at the ROW, as a warning.
TEST(TerrainRuleNotice, AnIgnoredRowCarriesItsOwnWarning)
{
    auto rule = MakeLiveRule();
    rule.Strength = 0.0f;

    UIElement host;
    GameEngine::Editor::AddRuleRowStateNotice(&host, rule, 0);

    EXPECT_EQ(WarningCountIn(host), 1u);
    EXPECT_TRUE(Has(AllTextIn(host), "ignored"));
}

TEST(TerrainRuleNotice, ARowThatWritesUnderConditionsIsQuiet)
{
    UIElement host;
    GameEngine::Editor::AddRuleRowStateNotice(&host, MakeLiveRule(), 0);
    EXPECT_TRUE(AllTextIn(host).empty());
}

// An unconditional row is a legitimate authoring choice — a base material under
// everything else is exactly this shape — so it gets a plain line, not a
// warning. A warning here would train the author to ignore the amber ones.
TEST(TerrainRuleNotice, AnUnconditionalRowIsExplainedButNotWarnedAbout)
{
    Components::TerrainSurfaceRule rule{};
    rule.Strength = 1.0f;
    rule.ConditionCount = 0;

    UIElement host;
    GameEngine::Editor::AddRuleRowStateNotice(&host, rule, 0);

    const std::string text = AllTextIn(host);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(WarningCountIn(host), 0u) << "an unconditional row is not an error";
    EXPECT_TRUE(Has(text, "whole volume")) << text;
}

// ---- Caps ------------------------------------------------------------------

TEST(TerrainRuleNotice, TheRuleCapNamesTheCapAndTheWayPastIt)
{
    UIElement host;
    GameEngine::Editor::AddRuleCapNotice(&host, Components::kMaxTerrainSurfaceRules);

    const std::string text = AllTextIn(host);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(WarningCountIn(host), 1u);
    EXPECT_TRUE(Has(text, std::to_string(Components::kMaxTerrainSurfaceRules).c_str())) << text;
    EXPECT_TRUE(Has(text, "second surface rules effect")) << text;
}

TEST(TerrainRuleNotice, TheRuleCapIsSilentBelowTheCap)
{
    UIElement host;
    GameEngine::Editor::AddRuleCapNotice(&host, Components::kMaxTerrainSurfaceRules - 1);
    EXPECT_TRUE(AllTextIn(host).empty());
}

// The condition cap is PER RULE, so its message has to name which rule is full —
// otherwise one full row reads as though the whole effect is out of room.
TEST(TerrainRuleNotice, TheConditionCapNamesItsRuleAndIsNumberedFromOne)
{
    UIElement host;
    GameEngine::Editor::AddConditionCapNotice(&host, /*ruleIndex=*/2,
                                              Components::kMaxTerrainRuleConditions);

    const std::string text = AllTextIn(host);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(WarningCountIn(host), 1u);
    EXPECT_TRUE(Has(text, "rule 3")) << text;
    EXPECT_TRUE(Has(text, std::to_string(Components::kMaxTerrainRuleConditions).c_str())) << text;
}

TEST(TerrainRuleNotice, TheConditionCapIsSilentBelowTheCap)
{
    UIElement host;
    GameEngine::Editor::AddConditionCapNotice(&host, 0,
                                              Components::kMaxTerrainRuleConditions - 1);
    EXPECT_TRUE(AllTextIn(host).empty());
}

// ---- The throttle arm ------------------------------------------------------

TEST(TerrainInteractiveEditArm, ArmingIsIdempotentAcrossADragsManyPreviews)
{
    ASSERT_EQ(TerrainInteractiveModifierEditArmCount(), 0);

    TerrainInteractiveModifierEditArm arm;
    arm.Arm();
    arm.Arm();
    arm.Arm();
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 1)
        << "each mouse-move's preview arms again; only the first may count";

    arm.Release();
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 0);
    arm.Release();
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 0);
}

// The failure this type exists for. An Inspector rebuild destroys a widget's
// callbacks mid-drag, so the mouse-up that would have released never arrives;
// without release-on-destruction the throttle stays armed for every later edit
// and the terrain never settles again.
TEST(TerrainInteractiveEditArm, DestructionReleasesADragThatNeverCommitted)
{
    ASSERT_EQ(TerrainInteractiveModifierEditArmCount(), 0);
    {
        TerrainInteractiveModifierEditArm arm;
        arm.Arm();
        ASSERT_EQ(TerrainInteractiveModifierEditArmCount(), 1);
    }
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 0)
        << "a widget destroyed mid-drag leaked the throttle arm";
}

// Two live arms share one source bit on the terrain service, so the count — not
// either arm — is what decides when that bit clears.
TEST(TerrainInteractiveEditArm, ReleasingOneArmDoesNotDropAnotherStillHolding)
{
    ASSERT_EQ(TerrainInteractiveModifierEditArmCount(), 0);

    TerrainInteractiveModifierEditArm first;
    TerrainInteractiveModifierEditArm second;
    first.Arm();
    second.Arm();
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 2);

    first.Release();
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 1);
    EXPECT_TRUE(second.IsArmed());

    second.Release();
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 0);
}

TEST(TerrainInteractiveEditArm, AnArmThatNeverArmedReleasesNothing)
{
    ASSERT_EQ(TerrainInteractiveModifierEditArmCount(), 0);
    {
        TerrainInteractiveModifierEditArm arm;
        EXPECT_FALSE(arm.IsArmed());
    }
    EXPECT_EQ(TerrainInteractiveModifierEditArmCount(), 0);
}

// ---- Fresh-condition default (visual-critic finding 9) ---------------------

// The component's zero-initialized band is [0, 1]. For a DEGREES condition that
// is [0 deg, 1 deg] — very nearly the empty band, and the one almost-inert state
// with no notice attached, because such a row is neither rule-less nor
// condition-less. Following the notices therefore collapsed a rule to about one
// degree silently. A fresh condition spans its kind's whole domain instead, so
// adding one is a no-op until the author narrows it.
TEST(TerrainRuleVocabulary, AFreshConditionSpansItsKindsWholeDomain)
{
    constexpr float kHeightScale = 512.0f;
    for (const Kind kind : kAllKinds)
    {
        const auto condition = Vocab::MakeDefaultCondition(kind, kHeightScale);
        const auto domain = Vocab::ConditionAuthoringDomain(kind, kHeightScale);
        EXPECT_EQ(condition.Kind, kind);
        EXPECT_FLOAT_EQ(condition.Min, domain.Min) << static_cast<int>(kind);
        EXPECT_FLOAT_EQ(condition.Max, domain.Max) << static_cast<int>(kind);
        EXPECT_GT(condition.Max, condition.Min) << static_cast<int>(kind);
    }
}

// The specific regression: a degrees condition must not start life one degree
// wide. Named separately from the sweep so a failure says which case broke.
TEST(TerrainRuleVocabulary, AFreshDegreesConditionIsNotAOneDegreeSliver)
{
    const auto condition = Vocab::MakeDefaultCondition(Kind::SlopeDegrees, 256.0f);
    EXPECT_FLOAT_EQ(condition.Max, 90.0f);
    EXPECT_GT(condition.Max - condition.Min, 1.0f)
        << "a fresh slope condition is a sliver, so adding one silently kills the rule";
}

// A fresh condition must also be INERT-FREE: adding one to a live rule cannot
// make that rule stop writing.
TEST(TerrainRuleState, AddingAFreshConditionDoesNotMakeARuleIgnored)
{
    auto rule = MakeLiveRule();
    rule.ConditionCount = 2;
    rule.Conditions[1] = Vocab::MakeDefaultCondition(Kind::HeightMetres, 300.0f);
    EXPECT_TRUE(Vocab::RuleInertReason(rule).empty());
}

// ---- Caption rounding (finding 10) ----------------------------------------

// Whole-number rendering rounded 4.8 up to "5" and 3.2 down to "3", so two
// captions disagreed about which way the same widget rounds. Under 10, metric
// kinds carry a decimal.
TEST(TerrainRuleVocabulary, SmallMetricValuesKeepADecimalRatherThanRoundingBothWays)
{
    EXPECT_EQ(Vocab::FormatConditionValue(Kind::SlopeDegrees, 4.8f), "4.8\xC2\xB0");
    EXPECT_EQ(Vocab::FormatConditionValue(Kind::HeightMetres, 3.2f), "3.2 m");
}

// Large values stay whole: a decimal on every slope in a rule set is noise.
TEST(TerrainRuleVocabulary, LargeMetricValuesStayWholeNumbers)
{
    EXPECT_EQ(Vocab::FormatConditionValue(Kind::SlopeDegrees, 35.0f), "35\xC2\xB0");
    EXPECT_EQ(Vocab::FormatConditionValue(Kind::HeightMetres, 400.0f), "400 m");
}

// A non-zero value must never print as zero, whatever its kind's precision — a
// 0.4 m feather reading "0 m" tells the reader "hard edge" while the sentence
// around it says the band is feathered.
TEST(TerrainRuleVocabulary, ASmallNonZeroValueNeverRendersAsZero)
{
    for (const float value : {0.4f, 0.04f, 0.004f})
    {
        const std::string text = Vocab::FormatConditionValue(Kind::HeightMetres, value);
        EXPECT_NE(text.find_first_of("123456789"), std::string::npos)
            << value << " rendered as " << text;
    }
    // Genuine zero renders "0 m": it takes the one-decimal form like the 0.4
    // beside it, and the trailing zero then comes off with every other one.
    EXPECT_EQ(Vocab::FormatConditionValue(Kind::HeightMetres, 0.0f), "0 m");
}

// ---- Band range text (finding 11) -----------------------------------------

// The band control's tooltip repeated the row's own name over the strip it
// covered. It carries the live band instead, so it says something nothing else
// on the row does.
TEST(TerrainRuleVocabulary, BandRangeTextCarriesBothEdgesWithUnits)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 0.0f, 35.0f, 0.0f);
    const std::string text = Vocab::ConditionBandRangeText(condition);
    EXPECT_TRUE(Has(text, "0")) << text;
    EXPECT_TRUE(Has(text, "35")) << text;
    EXPECT_TRUE(Has(text, "\xC2\xB0")) << text;
    EXPECT_FALSE(Has(Lower(text), "band")) << "the label already says Band: " << text;
    EXPECT_FALSE(Has(Lower(text), "condition")) << text;
}

// ---- Rule strength in the caption (finding 8) -----------------------------

// The DECISION this pins: the strip and its sentence describe the CONDITION,
// because rule strength multiplies the product of every condition in the row and
// scaling each condition's own plot by it would show one multiplication N times.
// The sentence therefore stops claiming full weight when the rule will scale it.
TEST(TerrainRuleVocabulary, FullStrengthSaysNothingAboutTheRulesScaling)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f)));
    EXPECT_FALSE(Has(summary, "scales")) << summary;
    EXPECT_TRUE(Has(summary, "full weight")) << summary;
}

TEST(TerrainRuleVocabulary, AReducedRuleStrengthIsNamedInTheCaption)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    const std::string summary = Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(0.5f)));
    EXPECT_TRUE(Has(summary, "scales")) << summary;
    EXPECT_TRUE(Has(summary, "50%")) << summary;
}

// ---- The caption's closing sentence is MODE-aware -------------------------
//
// "Full weight" described the CONDITION honestly and the RESULT dishonestly. What
// a full-weight row actually leaves on the ground is decided by Mode, and the two
// answers are far apart: CompositeSplatTexel's blend arm adds the weight and
// renormalizes, so a full-weight row against ground holding none of its material
// ends up level with it — the ground keeps half. The replace arm lerps every
// channel toward the pure material, so at full weight the ground is gone. A
// caption that says the same thing for both promises the author an erasure they
// will not get, or hides one they will.

TEST(TerrainRuleVocabulary, AFullStrengthBlendRuleSaysItSharesRatherThanTakesOver)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    const std::string summary =
        Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, /*replace=*/false)));
    EXPECT_TRUE(Has(summary, "equal share with the ground beneath")) << summary;
    EXPECT_FALSE(Has(summary, "replaces")) << summary;
}

TEST(TerrainRuleVocabulary, AFullStrengthReplaceRuleSaysItTakesTheGround)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    const std::string summary =
        Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, /*replace=*/true)));
    EXPECT_TRUE(Has(summary, "fully replaces the ground")) << summary;
    EXPECT_FALSE(Has(summary, "equal share")) << summary;
}

// The two modes must not read alike. Asserted as a whole-string inequality rather
// than as two phrase checks, so a future rewording that collapses them back into
// one sentence fails here even if it keeps both phrases somewhere.
TEST(TerrainRuleVocabulary, TheTwoModesDoNotProduceTheSameCaption)
{
    const auto condition = MakeCondition(Kind::HeightMetres, 100.0f, 400.0f, 20.0f);
    EXPECT_NE(Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, false)),
              Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, true)));
}

// Below full strength the percentage is the whole story and the mode claim is
// withdrawn — in BOTH modes. "Fully replaces" is false for a row that lerps
// halfway, and it is the more dangerous of the two to leave standing.
TEST(TerrainRuleVocabulary, AScaledRuleMakesNoModeClaimInEitherMode)
{
    const auto condition = MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f);
    for (const bool replace : {false, true})
    {
        const std::string summary =
            Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(0.5f, replace)));
        EXPECT_TRUE(Has(summary, "scales this to 50%")) << summary;
        EXPECT_FALSE(Has(summary, "fully replaces")) << replace << ": " << summary;
        EXPECT_FALSE(Has(summary, "equal share")) << replace << ": " << summary;
    }
}

// Every band shape carries the closing sentence, not only the ordinary one. An
// empty or degenerate band still writes through the row's mode wherever its
// feather reaches, so the shape that leads the caption cannot decide whether the
// outcome gets stated.
TEST(TerrainRuleVocabulary, EveryBandShapeCarriesTheOutcomeSentence)
{
    const Components::TerrainRuleCondition shapes[] = {
        MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 8.0f), // ordinary, feathered
        MakeCondition(Kind::SlopeDegrees, 34.0f, 90.0f, 0.0f), // ordinary, hard
        MakeCondition(Kind::SlopeDegrees, 30.0f, 30.0f, 4.0f), // peak, feathered
        MakeCondition(Kind::SlopeDegrees, 30.0f, 30.0f, 0.0f), // peak, hard
        MakeCondition(Kind::SlopeDegrees, 70.0f, 30.0f, 4.0f), // empty, feathered
        MakeCondition(Kind::SlopeDegrees, 70.0f, 30.0f, 0.0f), // empty, hard
    };
    for (const auto& condition : shapes)
    {
        EXPECT_TRUE(Has(Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, false))),
                        "equal share"))
            << Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, false));
        EXPECT_TRUE(Has(Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, true))),
                        "fully replaces"))
            << Vocab::ConditionBandSummary(condition, MakeOwningRule(1.0f, true));
        EXPECT_TRUE(Has(Lower(Vocab::ConditionBandSummary(condition, MakeOwningRule(0.25f, false))),
                        "25%"))
            << Vocab::ConditionBandSummary(condition, MakeOwningRule(0.25f, false));
    }
}

// ---- The section's standing help ------------------------------------------
//
// Her report: "my new tool erased the layer I painted first and nothing told me."
// A rules effect composites into the same splat the volume's other effects wrote,
// so a rule DOES take back ground a paint layer earlier in the stack put down —
// and the section help was the only surface that could say so before the first
// bake rather than after it.
TEST(TerrainRuleVocabulary, TheSectionHelpWarnsThatARuleTakesGroundFromTheStackBeneathIt)
{
    const std::string help = Lower(std::string(Vocab::SurfaceRulesSectionHelp()));
    EXPECT_TRUE(Has(help, "paint layer")) << help;
    EXPECT_TRUE(Has(help, "stack")) << help;
    // Mode-accurate, like the caption: the two outcomes are named, so the line
    // cannot be read as "a rule always erases" or as "a rule never does".
    EXPECT_TRUE(Has(help, "replace")) << help;
    EXPECT_TRUE(Has(help, "blend")) << help;
}

// The two rules the section already carried survive the addition — a help line
// that gained a warning and lost how rows combine is a worse line.
TEST(TerrainRuleVocabulary, TheSectionHelpStillSaysHowRowsAndConditionsCombine)
{
    const std::string help = Lower(std::string(Vocab::SurfaceRulesSectionHelp()));
    EXPECT_TRUE(Has(help, "top to bottom")) << help;
    EXPECT_TRUE(Has(help, "and together")) << help;
    EXPECT_TRUE(Has(help, "feathers multiply")) << help;
}

// ---- Cap reasons, and that both sites say the same thing (finding 12) ------

TEST(TerrainRuleNotice, CapReasonsAreEmptyBelowTheCapAndPopulatedAtIt)
{
    EXPECT_TRUE(GameEngine::Editor::RuleCapReason(
                    Components::kMaxTerrainSurfaceRules - 1).empty());
    EXPECT_FALSE(GameEngine::Editor::RuleCapReason(
                     Components::kMaxTerrainSurfaceRules).empty());

    EXPECT_TRUE(GameEngine::Editor::ConditionCapReason(
                    0, Components::kMaxTerrainRuleConditions - 1).empty());
    EXPECT_FALSE(GameEngine::Editor::ConditionCapReason(
                     0, Components::kMaxTerrainRuleConditions).empty());
}

// The disabled button's tooltip and the notice beside it must be ONE sentence.
// A cap explained one way on the control and another way underneath it is two
// answers to the same question.
TEST(TerrainRuleNotice, TheDisabledButtonsReasonIsTheNoticesOwnText)
{
    UIElement ruleHost;
    GameEngine::Editor::AddRuleCapNotice(&ruleHost, Components::kMaxTerrainSurfaceRules);
    EXPECT_EQ(AllTextIn(ruleHost),
              Lower(GameEngine::Editor::RuleCapReason(Components::kMaxTerrainSurfaceRules)) + "\n");

    UIElement conditionHost;
    GameEngine::Editor::AddConditionCapNotice(&conditionHost, 2,
                                              Components::kMaxTerrainRuleConditions);
    EXPECT_EQ(AllTextIn(conditionHost),
              Lower(GameEngine::Editor::ConditionCapReason(
                        2, Components::kMaxTerrainRuleConditions))
                  + "\n");
}

// ---- Escaping-closure capture guard ---------------------------------------
//
// The regression arm for the SIGSEGV in the feather label-scrub.
//
// TerrainRuleRows.cpp builds a row and returns; every closure it installs is
// owned by a control that outlives that call by the whole life of the Inspector
// panel. A closure there may therefore capture only things it OWNS. The crash
// was two local pointers captured by REFERENCE and assigned after the row was
// built — it compiles, it reads correctly, and it dereferences a dead stack slot
// on the first scrub after the builder returns, by which time the slot held
// something non-null enough to pass the guard in front of it.
//
// This is a SOURCE-STRUCTURE test, following DebugServerReadPurityTests: the
// bug is a capture lifetime, so there is nothing to call that would reproduce
// it, and EditorTests compiles no widget TU to call into anyway. What CAN be
// mechanised is the rule the file has to keep. It dies red on the pre-fix
// source, which carried exactly one such capture.

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{

std::string ReadRuleRowsSource()
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" / "Terrain" / "TerrainRuleRows.cpp";
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "could not read " << path.string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Byte offsets of every lambda-introducer that captures by reference.
//
// Scans bracket groups and keeps those that both contain '&' AND are followed by
// lambda syntax, so an ordinary `f(a, &b)` or a subscript cannot be mistaken for
// a capture list.
std::vector<std::size_t> ReferenceCaptureOffsets(const std::string& source)
{
    std::vector<std::size_t> hits;
    for (std::size_t open = source.find('['); open != std::string::npos;
         open = source.find('[', open + 1))
    {
        const std::size_t close = source.find(']', open);
        if (close == std::string::npos)
            break;

        const std::string capture = source.substr(open + 1, close - open - 1);
        if (capture.find('&') == std::string::npos)
            continue;

        std::size_t next = source.find_first_not_of(" \t\r\n", close + 1);
        if (next == std::string::npos)
            continue;
        // A lambda introducer is followed by its parameter list, `mutable`, a
        // trailing return type, or the body directly.
        const bool looksLikeLambda = source[next] == '(' || source[next] == '{'
                                  || source.compare(next, 8, "mutable ") == 0
                                  || source.compare(next, 2, "->") == 0;
        if (looksLikeLambda)
            hits.push_back(open);
    }
    return hits;
}

std::string LineContaining(const std::string& source, std::size_t offset)
{
    const std::size_t begin = source.rfind('\n', offset);
    const std::size_t end = source.find('\n', offset);
    const std::size_t from = (begin == std::string::npos) ? 0 : begin + 1;
    return source.substr(from, (end == std::string::npos ? source.size() : end) - from);
}

// The source between a row's edit name and the next construct built after it, or
// EMPTY when either delimiter is missing.
//
// Both ends are required, and that is the whole point. Taking whatever follows
// the opening anchor when the closing one has moved lets the block run to
// end-of-file, where it matches the very call it is searching for from some
// unrelated row — a green that means "the delimiter moved", which is exactly the
// vacuous pass the capture guard's controls exist to rule out. An empty return
// makes the caller's ASSERT fire instead.
std::string HandlerBlock(const std::string& source, const std::string& editName,
                         const std::string& endsBefore)
{
    const std::size_t begin = source.find(editName);
    if (begin == std::string::npos)
        return {};
    const std::size_t end = source.find(endsBefore, begin + editName.size());
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end - begin);
}

} // namespace

TEST(TerrainRuleRowsSource, NoClosureCapturesALocalByReference)
{
    const std::string source = ReadRuleRowsSource();
    ASSERT_FALSE(source.empty());

    const std::vector<std::size_t> hits = ReferenceCaptureOffsets(source);

    std::string report;
    for (const std::size_t offset : hits)
        report += "\n    " + LineContaining(source, offset);

    EXPECT_TRUE(hits.empty())
        << "a closure in TerrainRuleRows.cpp captures by reference. Every closure in that file is "
           "owned by a control that outlives the function building it, so a reference capture "
           "points into a dead stack frame the moment that function returns - which is the "
           "SIGSEGV this test exists for. Capture what the closure owns (a shared_ptr holder for "
           "widgets filled in later)." << report;
}

// The guard is only worth having if it can SEE a reference capture, so this
// feeds it the exact shape the crash had and requires a hit. Without this, a
// scanner that silently matched nothing would read as a permanent pass.
TEST(TerrainRuleRowsSource, TheCaptureGuardDetectsTheShapeThatCrashed)
{
    const std::string crashed =
        "    Slider* featherSlider = nullptr;\n"
        "    auto refreshFeather = [refreshVisuals, &featherSlider, target]() mutable {\n"
        "        if (featherSlider) featherSlider->SetValueWithoutNotify(0.0f);\n"
        "    };\n";
    EXPECT_EQ(ReferenceCaptureOffsets(crashed).size(), 1u);

    // ...and does not fire on an ordinary address-of argument or a subscript.
    const std::string benign =
        "    Register(handler, &target);\n"
        "    const float v = values[index];\n"
        "    auto byValue = [refreshVisuals, target]() { refreshVisuals(); };\n";
    EXPECT_TRUE(ReferenceCaptureOffsets(benign).empty());
}

// ---- Caption precision matches the fields beside it (finding E) -----------

// A caption reading "34" beside a field reading "34.4" is the same disagreement
// the whole-number rounding produced, one step smaller.
TEST(TerrainRuleVocabulary, CaptionsCarryTheSameDigitsTheFieldShows)
{
    const std::string text = Vocab::FormatConditionValue(Kind::SlopeDegrees, 34.4f);
    EXPECT_TRUE(Has(text, "34.4")) << text;
    EXPECT_EQ(Vocab::ConditionDecimals(Kind::SlopeDegrees), 1);
}

// ...but a value that IS whole shows no decimal point, and no trailing zeros
// survive anywhere.
TEST(TerrainRuleVocabulary, NoCaptionCarriesATrailingZero)
{
    for (const auto pair : {std::pair<Kind, float>{Kind::SlopeDegrees, 34.0f},
                            std::pair<Kind, float>{Kind::SlopeDegrees, 4.0f},
                            std::pair<Kind, float>{Kind::HeightMetres, 400.0f},
                            std::pair<Kind, float>{Kind::HeightNormalized, 0.5f},
                            std::pair<Kind, float>{Kind::Noise, 1.0f}})
    {
        const std::string text = Vocab::FormatConditionValue(pair.first, pair.second);
        const std::size_t dot = text.find('.');
        if (dot == std::string::npos)
            continue;
        const std::size_t lastDigit = text.find_last_of("0123456789");
        EXPECT_NE(text[lastDigit], '0') << pair.second << " rendered as " << text;
    }
}

// ---- The Strength -> caption channel (finding B) --------------------------
//
// The caption is a pure function of (condition, rule strength), which is what
// the vocabulary tests pin. What BROKE was the widget's channel: Strength is
// built in the rule's scope and the captions in each condition's, so a Strength
// commit had nothing to call and the sentence kept its build-time text. There is
// no runtime seam to test — EditorTests compiles no widget TU — so the arm is
// structural, like the capture guard above.
TEST(TerrainRuleRowsSource, StrengthEditsRefreshEveryConditionCaption)
{
    const std::string source = ReadRuleRowsSource();
    ASSERT_FALSE(source.empty());

    // The handler block that follows it must hand the rule's conditions their
    // refresh, or a strength change cannot reach the sentences that report it.
    const std::string block = HandlerBlock(source, "\"Rule Strength\"", "// ---- conditions ----");
    ASSERT_FALSE(block.empty())
        << "the Strength row's edit name or its closing delimiter moved, so this guard scanned "
           "nothing - fix the anchors rather than deleting the test";
    EXPECT_NE(block.find("RefreshAllConditions"), std::string::npos)
        << "Strength commits without refreshing its conditions' captions, so a caption that "
           "says \"full weight\" keeps saying it after the rule is scaled to 50%.";
}

// Mode joined Strength as an input to the caption: the closing sentence says what
// a full-weight row leaves on the ground, and Blend and Replace leave different
// ground. Same structural channel, same failure if it is not wired — the Mode row
// is built in the rule's scope and the captions in each condition's.
TEST(TerrainRuleRowsSource, ModeEditsRefreshEveryConditionCaption)
{
    const std::string source = ReadRuleRowsSource();
    ASSERT_FALSE(source.empty());

    const std::string block = HandlerBlock(source, "\"Rule Mode\"", "\"Rule Strength\"");
    ASSERT_FALSE(block.empty())
        << "the Mode row's edit name or its closing delimiter moved, so this guard scanned "
           "nothing - fix the anchors rather than deleting the test";
    EXPECT_NE(block.find("RefreshAllConditions"), std::string::npos)
        << "Mode commits without refreshing its conditions' captions, so a caption that says the "
           "rule blends to an equal share keeps saying it after the row is switched to Replace.";
}

// The controls the two caption guards above were missing, in the shape the
// capture guard ships: feed the scanner a block that fails the rule and require a
// miss, feed it one that passes and require a hit, and — the case that made the
// old spelling vacuous — feed it a source whose CLOSING delimiter is gone and
// require an empty block rather than a run to end-of-file.
TEST(TerrainRuleRowsSource, TheCaptionGuardSeesAWiredRowAnUnwiredOneAndAMovedDelimiter)
{
    const std::string wired =
        "    auto handlers = MakeRuleDragHandlers<float>(ctx, \"Rule Strength\",\n"
        "        [](RulesEffect& u, float v) { u.Rules[0].Strength = v; },\n"
        "        [conditionRefreshers]() { RefreshAllConditions(conditionRefreshers); });\n"
        "    AddActionButton(body, \"Remove Rule\", nullptr, {});\n";
    EXPECT_NE(HandlerBlock(wired, "\"Rule Strength\"", "AddActionButton").find(
                  "RefreshAllConditions"),
              std::string::npos);

    // The pre-fix shape: the row commits and refreshes only its own two widgets.
    // RefreshAllConditions appears LATER in the file, which is what a block run to
    // end-of-file would have swallowed.
    const std::string unwired =
        "    auto handlers = MakeRuleDragHandlers<float>(ctx, \"Rule Strength\",\n"
        "        [](RulesEffect& u, float v) { u.Rules[0].Strength = v; },\n"
        "        [slider, field]() { slider->SetValueWithoutNotify(0.0f); });\n"
        "    AddActionButton(body, \"Remove Rule\", nullptr, {});\n"
        "    void Elsewhere() { RefreshAllConditions(refreshers); }\n";
    EXPECT_EQ(HandlerBlock(unwired, "\"Rule Strength\"", "AddActionButton").find(
                  "RefreshAllConditions"),
              std::string::npos)
        << "the block reached past its closing delimiter and matched an unrelated call";

    // Closing delimiter gone: EMPTY, so the caller's ASSERT fires. The old
    // spelling returned everything to end-of-file here and read green.
    const std::string movedDelimiter =
        "    auto handlers = MakeRuleDragHandlers<float>(ctx, \"Rule Strength\", apply, after);\n"
        "    AddRemoveRuleButton(body);\n"
        "    void Elsewhere() { RefreshAllConditions(refreshers); }\n";
    EXPECT_TRUE(HandlerBlock(movedDelimiter, "\"Rule Strength\"", "AddActionButton").empty());

    // Opening anchor gone: likewise empty rather than a block starting at zero.
    EXPECT_TRUE(HandlerBlock(wired, "\"Rule Loudness\"", "AddActionButton").empty());
}
