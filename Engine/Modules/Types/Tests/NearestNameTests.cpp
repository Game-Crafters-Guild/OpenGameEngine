#include <gtest/gtest.h>

#include "Types/NearestName.h"

#include <string>
#include <vector>

using namespace GameEngine;

TEST(NearestNameTests, IdenticalStringsHaveZeroDistance)
{
    EXPECT_EQ(LevenshteinCapped("PULSE", "PULSE", 2), 0u);
}

TEST(NearestNameTests, SingleEditsCostOne)
{
    EXPECT_EQ(LevenshteinCapped("PULSE", "PULS", 2), 1u);   // deletion
    EXPECT_EQ(LevenshteinCapped("PULS", "PULSE", 2), 1u);   // insertion
    EXPECT_EQ(LevenshteinCapped("PULSE", "PULSA", 2), 1u);  // substitution
}

// The cap is the contract: anything further away reports cap + 1 rather than the
// true distance, so callers must compare against the cap, never for equality.
TEST(NearestNameTests, DistanceBeyondTheCapReportsCapPlusOne)
{
    EXPECT_EQ(LevenshteinCapped("PULSE", "TRIPLANAR", 2), 3u);
    EXPECT_EQ(LevenshteinCapped("abc", "", 2), 3u);
    EXPECT_EQ(LevenshteinCapped("", "abc", 2), 3u);
}

TEST(NearestNameTests, EmptyStringsMatch)
{
    EXPECT_EQ(LevenshteinCapped("", "", 2), 0u);
}

TEST(NearestNameTests, NearestPicksTheClosestCandidate)
{
    const std::vector<std::string> candidates = {"TRIPLANAR", "PULSE", "GLOW"};
    EXPECT_EQ(NearestName("PULS", candidates), "PULSE");
    EXPECT_EQ(NearestName("GLW", candidates), "GLOW");
}

TEST(NearestNameTests, NothingCloseEnoughYieldsNoSuggestion)
{
    const std::vector<std::string> candidates = {"TRIPLANAR", "PULSE"};
    EXPECT_TRUE(NearestName("COMPLETELYDIFFERENT", candidates).empty());
    EXPECT_TRUE(NearestNameSuffix("COMPLETELYDIFFERENT", candidates).empty());
}

TEST(NearestNameTests, EmptyCandidateListYieldsNoSuggestion)
{
    const std::vector<std::string> none;
    EXPECT_TRUE(NearestName("PULSE", none).empty());
    EXPECT_TRUE(NearestNameSuffix("PULSE", none).empty());
}

TEST(NearestNameTests, SuffixIsReadyToAppendToASentence)
{
    const std::vector<std::string> candidates = {"PULSE"};
    EXPECT_EQ(NearestNameSuffix("PULS", candidates), " (did you mean 'PULSE'?)");
}

// Mutation guard: a strictly-closer test (`d < bestDist`) makes ties resolve to
// the FIRST candidate in range order, which is what makes the suggestion stable
// for a caller that controls its ordering.
TEST(NearestNameTests, TiesResolveToTheFirstCandidate)
{
    const std::vector<std::string> candidates = {"CAT", "COT"};
    EXPECT_EQ(NearestName("CUT", candidates), "CAT");
}
