// Keyword cross-check: ShaderComposer::DiagnoseUnreadKeywords compares the
// keywords a material enables against the GE_USER_* names its surface actually
// reads. A misspelled keyword is otherwise completely silent — the preamble
// emits GE_USER_<TYPO>, no #ifdef matches, and the material renders as if the
// keyword were absent. These are warnings by construction: an unread keyword is
// legal, so nothing here may fail a compile.

#include <gtest/gtest.h>

#include "Rendering/Materials/ShaderComposer.h"

#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

// The starter template's shape: one keyword, read through an #ifdef.
constexpr const char* kPulseSurface = R"(
// @texture albedoMap srgb
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
#ifdef GE_USER_PULSE
    float pulse = 0.5 + 0.5 * sin(Light.uTimeParams.x);
#else
    float pulse = 1.0;
#endif
    o.emissive = vec3(pulse);
    return o;
}
)";

bool AnyMentions(const std::vector<std::string>& lines, const std::string& needle)
{
    for (const std::string& line : lines)
    {
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST(ShaderComposerKeywordDiagnostics, KeywordTheSurfaceReadsIsSilent)
{
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {"PULSE"}, "my.glsl");
    EXPECT_TRUE(lines.empty()) << "a keyword the surface reads must not warn";
}

TEST(ShaderComposerKeywordDiagnostics, MisspelledKeywordWarnsWithTheNearestMatch)
{
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {"PULS"}, "my.glsl");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("'PULS'"), std::string::npos);
    EXPECT_NE(lines[0].find("did you mean 'PULSE'?"), std::string::npos);
    EXPECT_NE(lines[0].find("my.glsl"), std::string::npos) << "the warning must name the surface";
}

// Mutation guard: the raw authored spelling is what the author has to fix, so it
// must appear verbatim even though the comparison runs on the sanitized form.
TEST(ShaderComposerKeywordDiagnostics, WarningQuotesTheAuthoredSpelling)
{
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {"puls"}, "my.glsl");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("'puls'"), std::string::npos);
    EXPECT_NE(lines[0].find("did you mean 'PULSE'?"), std::string::npos);
}

// Keywords are matched case-insensitively via the sanitizer, exactly as the
// preamble emits them — a lowercase authored keyword is NOT a typo.
TEST(ShaderComposerKeywordDiagnostics, LowercaseAuthoredKeywordMatches)
{
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {"pulse"}, "my.glsl");
    EXPECT_TRUE(lines.empty());
}

TEST(ShaderComposerKeywordDiagnostics, UnrelatedUnreadKeywordWarnsWithoutASuggestion)
{
    const auto lines =
        ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {"TRIPLANAR"}, "my.glsl");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("'TRIPLANAR'"), std::string::npos);
    EXPECT_EQ(lines[0].find("did you mean"), std::string::npos);
}

// A surface that reads NO keyword at all cannot distinguish a typo from a
// keyword consumed inside an #include, so the warning says so rather than
// implying the keyword is wrong.
TEST(ShaderComposerKeywordDiagnostics, SurfaceReadingNoKeywordSaysIncludesAreNotScanned)
{
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(
        "SurfaceOutput EvaluateSurface(SurfaceInput s) { return DefaultSurfaceOutput(); }",
        {"PULSE"}, "plain.glsl");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("#include"), std::string::npos);
}

// GE_USER_TEXTURE is the named-slot accessor macro, not a keyword. Treating it
// as one would make every slot-sampling surface look like it reads a keyword
// called TEXTURE.
TEST(ShaderComposerKeywordDiagnostics, TextureAccessorMacroIsNotAKeywordConsumer)
{
    constexpr const char* kSurface = R"(
// @texture accentMask linear
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    float mask = texture(GE_USER_TEXTURE(accentMask), sIn.uv0).r;
    SurfaceOutput o = DefaultSurfaceOutput();
    o.emissive = vec3(mask);
    return o;
}
)";
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(kSurface, {"TEXTURE"}, "s.glsl");
    // TEXTURE is a reserved engine define, so it is never emitted as a keyword
    // and is therefore not reported as unread either.
    EXPECT_TRUE(lines.empty());

    // And a real keyword still reports "no keyword read at all" for this surface.
    const auto other = ShaderComposer::DiagnoseUnreadKeywords(kSurface, {"PULSE"}, "s.glsl");
    ASSERT_EQ(other.size(), 1u);
    EXPECT_EQ(other[0].find("did you mean"), std::string::npos)
        << "GE_USER_TEXTURE must not become the suggestion";
}

// A keyword named only in a comment is prose, not a consumer.
TEST(ShaderComposerKeywordDiagnostics, KeywordMentionedOnlyInACommentIsNotRead)
{
    constexpr const char* kSurface = R"(
// Set "keywords": ["PULSE"] to enable GE_USER_PULSE.
SurfaceOutput EvaluateSurface(SurfaceInput s) { return DefaultSurfaceOutput(); }
)";
    const auto lines = ShaderComposer::DiagnoseUnreadKeywords(kSurface, {"PULSE"}, "s.glsl");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("'PULSE'"), std::string::npos);
}

TEST(ShaderComposerKeywordDiagnostics, NoKeywordsMeansNoDiagnostics)
{
    EXPECT_TRUE(ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {}, "my.glsl").empty());
}

// Several keywords report independently; one good keyword does not suppress the
// warning for a bad sibling.
TEST(ShaderComposerKeywordDiagnostics, EachUnreadKeywordGetsItsOwnLine)
{
    const auto lines =
        ShaderComposer::DiagnoseUnreadKeywords(kPulseSurface, {"PULSE", "PULS", "GLOW"}, "my.glsl");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_TRUE(AnyMentions(lines, "'PULS'"));
    EXPECT_TRUE(AnyMentions(lines, "'GLOW'"));
}
