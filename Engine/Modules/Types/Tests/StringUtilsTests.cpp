#include <gtest/gtest.h>

#include "Types/StringUtils.h"

using namespace GameEngine;

TEST(StringUtilsTests, FormatCountTakesTheSingularOnlyForOne)
{
    EXPECT_EQ(FormatCount(1, "entity", "entities"), "1 entity");
    EXPECT_EQ(FormatCount(0, "entity", "entities"), "0 entities");
    EXPECT_EQ(FormatCount(3, "entity", "entities"), "3 entities");
}

TEST(StringUtilsTests, FormatGroupedIntegerPutsACommaBetweenEachThreeDigits)
{
    EXPECT_EQ(FormatGroupedInteger(0), "0");
    EXPECT_EQ(FormatGroupedInteger(999), "999");
    EXPECT_EQ(FormatGroupedInteger(1000), "1,000");
    EXPECT_EQ(FormatGroupedInteger(4194304), "4,194,304");
    EXPECT_EQ(FormatGroupedInteger(18446744073709551615ull), "18,446,744,073,709,551,615");
}

TEST(StringUtilsTests, FormatMebibytesKeepsADecimalOnlyBelowTen)
{
    EXPECT_EQ(FormatMebibytes(0), "0.0 MiB");
    EXPECT_EQ(FormatMebibytes(4718592), "4.5 MiB");
    EXPECT_EQ(FormatMebibytes(16777216), "16 MiB");
    EXPECT_EQ(FormatMebibytes(810549248), "773 MiB");
}

TEST(StringUtilsTests, EqualsIgnoreCaseMatchesTheSameTextInAnyCase)
{
    EXPECT_TRUE(EqualsIgnoreCase("off", "OFF"));
    EXPECT_TRUE(EqualsIgnoreCase("False", "fALSE"));
    EXPECT_TRUE(EqualsIgnoreCase("", ""));
}

TEST(StringUtilsTests, EqualsIgnoreCaseRejectsPrefixesAndOtherText)
{
    EXPECT_FALSE(EqualsIgnoreCase("of", "off")) << "a prefix is not equal";
    EXPECT_FALSE(EqualsIgnoreCase("offset", "off")) << "neither is a longer text";
    EXPECT_FALSE(EqualsIgnoreCase("no", "on"));
    EXPECT_FALSE(EqualsIgnoreCase("", "0"));
}

TEST(StringUtilsTests, ContainsControlCharacterFindsBytesBelowSpaceAndDelete)
{
    EXPECT_TRUE(ContainsControlCharacter(std::string("a\0b", 3)));
    EXPECT_TRUE(ContainsControlCharacter("tab\there"));
    EXPECT_TRUE(ContainsControlCharacter("del\x7F"));
    EXPECT_FALSE(ContainsControlCharacter("Textures/Bloom/lens Dirt1.png"));
    EXPECT_FALSE(ContainsControlCharacter("caf\xC3\xA9.png")) << "UTF-8 bytes are not control characters";
    EXPECT_FALSE(ContainsControlCharacter(""));
}

// Reflected inspector rows and options are labelled from their identifiers.
TEST(StringUtilsTests, IdentifierToWordsReadsIdentifiersAsWords)
{
    struct Case
    {
        const char* Identifier;
        const char* Words;
    };
    const Case cases[] = {
        // The owner's cases (#2231), verbatim.
        {"PrewarmSeconds", "Prewarm Seconds"},
        {"castsShadows", "Casts Shadows"},
        {"m_Value", "Value"},
        {"value_", "Value"},
        {"variable_name", "Variable Name"},
        {"GPUSkin", "GPU Skin"},
        {"URLPath", "URL Path"},
        {"CastsShadows", "Casts Shadows"},
        {"initial_linear_velocity", "Initial Linear Velocity"},
        {"m_SheetColumns", "Sheet Columns"},
        {"_private_", "Private"},
        {"trailing_", "Trailing"},
        {"HDRColor", "HDR Color"},
        // An acronym keeps its number, a dimension stays whole, a two-letter acronym before a
        // capital word splits from it once.
        {"MSM4", "MSM4"},
        {"HDR10", "HDR10"},
        {"Grid5x5", "Grid 5x5"},
        {"Grid3x3", "Grid 3x3"},
        {"FidelityFXFast", "Fidelity FX Fast"},
        {"SSAO", "SSAO"},
        {"World3D", "World 3D"},
        {"Cascaded2", "Cascaded 2"},
        {"Mip0Bias", "Mip 0 Bias"},
        {"Cascade16Count", "Cascade 16 Count"},
        {"FaceCameraYAlongVelocity", "Face Camera Y Along Velocity"},
        {"x", "X"},
        {"Already Words", "Already Words"},
        {"", ""},
    };
    for (const Case& c : cases)
        EXPECT_EQ(IdentifierToWords(c.Identifier), c.Words) << c.Identifier;
}
