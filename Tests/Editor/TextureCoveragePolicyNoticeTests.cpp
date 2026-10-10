#include <gtest/gtest.h>

#include "Inspectors/TextureCoveragePolicyNotice.h"

#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"

#include <string>

using GameEngine::TextureCookCompression;
using GameEngine::TextureCookUsage;
using GameEngine::UIElement;
using GameEngine::Editor::AddTextureCoveragePolicyRows;
using GameEngine::Editor::ReadTextureCoveragePolicy;
using GameEngine::Editor::TextureCoveragePolicyInputs;
using GameEngine::Editor::TextureCoveragePolicyRows;

namespace
{

TextureCoveragePolicyInputs EnabledOn(TextureCookCompression compression, TextureCookUsage usage)
{
    TextureCoveragePolicyInputs inputs;
    inputs.Enabled = "1";
    inputs.Cutoff = "0.5";
    inputs.Compression = compression;
    inputs.Usage = usage;
    return inputs;
}

// Every refusal has to name the action AND the consequence: an author who reads only the first
// half fixes nothing, and one who reads only the second half does not know what to change.
void ExpectActionableRefusal(const std::string& notice)
{
    EXPECT_FALSE(notice.empty());
    EXPECT_NE(notice.find("Preserve Alpha Coverage"), std::string::npos);
    EXPECT_NE(notice.find("this texture will not load"), std::string::npos);
}

TEST(TextureCoveragePolicyNoticeTests, APolicyTheImportSettingsCanCarryShowsNothing)
{
    for (const auto compression : {TextureCookCompression::Auto, TextureCookCompression::BC7,
                                   TextureCookCompression::None})
    {
        const auto state = ReadTextureCoveragePolicy(EnabledOn(compression, TextureCookUsage::Color));
        EXPECT_TRUE(state.Enabled);
        EXPECT_TRUE(state.Satisfied);
        EXPECT_TRUE(state.Notice.empty());
    }
}

TEST(TextureCoveragePolicyNoticeTests, EachRefusalNamesTheActionAndTheConsequence)
{
    for (const auto compression : {TextureCookCompression::BC1, TextureCookCompression::BC4,
                                   TextureCookCompression::BC5, TextureCookCompression::BC6H})
    {
        const auto state = ReadTextureCoveragePolicy(EnabledOn(compression, TextureCookUsage::Color));
        EXPECT_FALSE(state.Satisfied);
        ExpectActionableRefusal(state.Notice);
        EXPECT_NE(state.Notice.find("Auto, BC7 or Uncompressed"), std::string::npos);
    }

    const auto normal = ReadTextureCoveragePolicy(
        EnabledOn(TextureCookCompression::BC7, TextureCookUsage::Normal));
    EXPECT_FALSE(normal.Satisfied);
    ExpectActionableRefusal(normal.Notice);
    EXPECT_NE(normal.Notice.find("Usage"), std::string::npos);

    // Auto alone is fine and Mask alone is fine; only the pair resolves to a format that drops
    // alpha, which is exactly the combination an author reaches by leaving both on Auto.
    const auto maskUnderAuto = ReadTextureCoveragePolicy(
        EnabledOn(TextureCookCompression::Auto, TextureCookUsage::Mask));
    EXPECT_FALSE(maskUnderAuto.Satisfied);
    ExpectActionableRefusal(maskUnderAuto.Notice);
    EXPECT_TRUE(
        ReadTextureCoveragePolicy(EnabledOn(TextureCookCompression::BC7, TextureCookUsage::Mask))
            .Satisfied);

    auto highDynamicRange = EnabledOn(TextureCookCompression::None, TextureCookUsage::Color);
    highDynamicRange.SourceIsHighDynamicRange = true;
    const auto hdrState = ReadTextureCoveragePolicy(highDynamicRange);
    EXPECT_FALSE(hdrState.Satisfied);
    ExpectActionableRefusal(hdrState.Notice);
}

TEST(TextureCoveragePolicyNoticeTests, AnUnreadableStoredValueIsNamedRatherThanReadAsOff)
{
    TextureCoveragePolicyInputs inputs;
    inputs.Enabled = "yes";
    const auto state = ReadTextureCoveragePolicy(inputs);
    EXPECT_FALSE(state.Satisfied);
    ExpectActionableRefusal(state.Notice);
}

TEST(TextureCoveragePolicyNoticeTests, ADisabledPolicyShowsNoNoticeAndStillReadsItsCutoff)
{
    TextureCoveragePolicyInputs inputs;
    inputs.Enabled = "0";
    inputs.Cutoff = "0.538";
    inputs.Compression = TextureCookCompression::BC1;  // would refuse, if the policy were on
    const auto state = ReadTextureCoveragePolicy(inputs);
    EXPECT_FALSE(state.Enabled);
    EXPECT_TRUE(state.Satisfied);
    EXPECT_TRUE(state.Notice.empty());
    EXPECT_FLOAT_EQ(state.Cutoff, 0.538f);
}

// The regression this whole block exists for: the notice used to be built once, and the
// Compression and Usage rows wrote without re-deriving it, so an author could select BC1 with
// the policy on, see nothing, and find out at the next load. The notice must follow the values.
TEST(TextureCoveragePolicyNoticeTests, TheNoticeFollowsAWriteFromAnotherRowWithoutARebuild)
{
    UIElement root;
    TextureCoveragePolicyInputs current = EnabledOn(TextureCookCompression::BC7,
                                                    TextureCookUsage::Color);
    TextureCoveragePolicyRows rows = AddTextureCoveragePolicyRows(
        &root, true, [&current]() { return current; }, [](bool) {}, [](float) {});
    ASSERT_NE(rows.Notice, nullptr);
    ASSERT_TRUE(static_cast<bool>(rows.Refresh));

    EXPECT_TRUE(rows.Notice->HasClass("hidden"));
    EXPECT_TRUE(rows.Notice->GetText().empty());

    // The Compression row wrote BC1 and called back.
    current.Compression = TextureCookCompression::BC1;
    rows.Refresh();
    EXPECT_FALSE(rows.Notice->HasClass("hidden"));
    ExpectActionableRefusal(rows.Notice->GetText());

    // ...and back to a format that can carry it.
    current.Compression = TextureCookCompression::BC7;
    rows.Refresh();
    EXPECT_TRUE(rows.Notice->HasClass("hidden"));
    EXPECT_TRUE(rows.Notice->GetText().empty());

    // The Usage row is the other way in.
    current.Usage = TextureCookUsage::Normal;
    rows.Refresh();
    EXPECT_FALSE(rows.Notice->HasClass("hidden"));
    ExpectActionableRefusal(rows.Notice->GetText());
}

// The cook ignores a stored cutoff while the policy is off, so a write there would spend a
// recook and an upload on a value nothing reads. The row says so by being switched off, and
// the block's own write guard reads the same policy state this asserts.
TEST(TextureCoveragePolicyNoticeTests, TheCutoffRowIsSwitchedOffWhileThePolicyIsOff)
{
    UIElement root;
    TextureCoveragePolicyInputs current;
    current.Enabled = "0";
    current.Cutoff = "0.25";

    TextureCoveragePolicyRows rows = AddTextureCoveragePolicyRows(
        &root, true, [&current]() { return current; }, [](bool) {}, [](float) {});
    ASSERT_NE(rows.Cutoff, nullptr);

    EXPECT_FALSE(rows.Cutoff->IsEnabled());
    EXPECT_FLOAT_EQ(rows.Cutoff->GetValue(), 0.25f);

    current.Enabled = "1";
    rows.Refresh();
    EXPECT_TRUE(rows.Cutoff->IsEnabled());

    current.Enabled = "0";
    rows.Refresh();
    EXPECT_FALSE(rows.Cutoff->IsEnabled());
}

// A source that refuses metadata writes outranks the policy's own gate: turning the policy
// on cannot hand back a live cutoff field on an asset nothing here can save, and the enable
// toggle that would turn it on is off too.
TEST(TextureCoveragePolicyNoticeTests, AReadOnlySourceKeepsBothRowsSwitchedOff)
{
    UIElement root;
    TextureCoveragePolicyInputs current = EnabledOn(TextureCookCompression::BC7,
                                                    TextureCookUsage::Color);

    TextureCoveragePolicyRows rows = AddTextureCoveragePolicyRows(
        &root, false, [&current]() { return current; }, [](bool) {}, [](float) {});
    ASSERT_NE(rows.Cutoff, nullptr);
    ASSERT_NE(rows.Enable, nullptr);

    EXPECT_FALSE(rows.Enable->IsEnabled());
    EXPECT_FALSE(rows.Cutoff->IsEnabled());

    rows.Refresh();
    EXPECT_FALSE(rows.Enable->IsEnabled());
    EXPECT_FALSE(rows.Cutoff->IsEnabled());
}

TEST(TextureCoveragePolicyNoticeTests, ANullParentIsIgnored)
{
    const TextureCoveragePolicyRows rows = AddTextureCoveragePolicyRows(
        nullptr, true, []() { return TextureCoveragePolicyInputs{}; }, [](bool) {}, [](float) {});
    EXPECT_EQ(rows.Notice, nullptr);
    EXPECT_FALSE(static_cast<bool>(rows.Refresh));
}

} // namespace
