#include "Editor/Hierarchy/HierarchySearch.h"

#include <gtest/gtest.h>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace GameEngine::Editor {
namespace {

HierarchySearchQuery::Candidate Candidate(std::string_view name,
                                          std::string_view id,
                                          std::string_view path,
                                          std::unordered_set<std::string> types = {})
{
    return {name, id, path, [types = std::move(types)](std::string_view type, bool exact)
            {
                for (const std::string& candidateType : types)
                {
                    if ((exact && candidateType == type) || (!exact && candidateType.find(type) != std::string::npos))
                        return true;
                }
                return false;
            }};
}

TEST(HierarchySearchTests, CombinesTypeAndQuotedSubtreePath)
{
    const auto query = HierarchySearchQuery::Parse(R"(t:light path:"/World/Vehicles New")");

    EXPECT_TRUE(query.Matches(Candidate("Headlamp", "42", "/World/Vehicles New/Car/Headlamp", {"light", "transform"})));
    EXPECT_FALSE(query.Matches(Candidate("Headlamp", "42", "/World/Props/Headlamp", {"light", "transform"})));
    EXPECT_FALSE(query.Matches(Candidate("Wheel", "43", "/World/Vehicles New/Car/Wheel", {"meshrenderer"})));
}

TEST(HierarchySearchTests, SupportsAliasesExactValuesAndExclusions)
{
    const auto query = HierarchySearchQuery::Parse("type:camera -name:preview id=17");

    EXPECT_TRUE(query.Matches(Candidate("Main Camera", "17", "/World/Main Camera", {"camera"})));
    EXPECT_FALSE(query.Matches(Candidate("Preview Camera", "17", "/World/Preview Camera", {"camera"})));
    EXPECT_FALSE(query.Matches(Candidate("Main Camera", "170", "/World/Main Camera", {"camera"})));
}

TEST(HierarchySearchTests, BareTermsHonorSearchBarScope)
{
    const auto query = HierarchySearchQuery::Parse("camera");
    const auto candidate = Candidate("Player", "314", "/World/Player Camera", {"camera"});

    EXPECT_FALSE(query.Matches(candidate, "all"));
    EXPECT_FALSE(query.Matches(candidate, "name"));
    EXPECT_TRUE(HierarchySearchQuery::Parse("314").Matches(candidate, "id"));
    EXPECT_TRUE(query.Matches(candidate, "type"));
    EXPECT_TRUE(HierarchySearchQuery::Parse("player camera").Matches(candidate, "path"));
}

TEST(HierarchySearchTests, PlusForcesAnExactBareTerm)
{
    const auto query = HierarchySearchQuery::Parse("+Player");

    EXPECT_TRUE(query.Matches(Candidate("Player", "1", "/World/Player")));
    EXPECT_FALSE(query.Matches(Candidate("Player Camera", "2", "/World/Player Camera")));
}

} // namespace
} // namespace GameEngine::Editor
