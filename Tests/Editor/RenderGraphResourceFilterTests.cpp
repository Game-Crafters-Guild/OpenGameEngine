// The defect this locks: get_render_graph_resources advertised lifetime/type/aliveOnly
// and its handler discarded the request context, so every filtered query returned the
// FULL resource list. Measured live before the fix — no filter, `lifetime=Transient`,
// `type=Buffer`, `aliveOnly` and even `lifetime=Nonsense` all answered with the same 35
// resources. A silently-ignored filter is worse than a broken one: the full list is
// indistinguishable from a correct narrow answer, so nothing tells the caller to look.
//
// The two halves that matters are that a filter NARROWS, and that a value naming nothing
// is an error rather than a widening back to everything.

#include <gtest/gtest.h>

#include "DebugServer/RenderGraphResourceFilter.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
using GameEngine::Editor::RenderGraphResourceFilter;
using json = nlohmann::json;

// One row of the listing the handler emits, in its canonical spellings.
struct Resource
{
    const char* Kind;
    const char* Lifetime;
    bool Used;
};

// Shaped like a real frame: transient textures dominate, a couple are dead, and the
// backbuffer is the one Swapchain row.
const Resource kFrame[] = {
    {"Texture", "Transient", true},  {"Texture", "Transient", false}, {"Buffer", "Transient", true},
    {"Buffer", "Persistent", true},  {"Texture", "Imported", true},   {"Texture", "Swapchain", true},
    {"Buffer", "Persistent", false},
};

int CountAccepted(const json& params)
{
    const RenderGraphResourceFilter filter(params);
    int n = 0;
    for (const Resource& r : kFrame)
        if (filter.Accepts(r.Kind, r.Lifetime, r.Used))
            ++n;
    return n;
}

TEST(RenderGraphResourceFilterTests, NoFilterKeepsEveryResource)
{
    EXPECT_EQ(CountAccepted(json::object()), 7);
}

TEST(RenderGraphResourceFilterTests, EachFilterNarrowsBelowTheUnfilteredCount)
{
    // The regression itself: these three all read 7 while the handler ignored them.
    EXPECT_EQ(CountAccepted({{"lifetime", "Transient"}}), 3);
    EXPECT_EQ(CountAccepted({{"type", "Buffer"}}), 3);
    EXPECT_EQ(CountAccepted({{"aliveOnly", true}}), 5);
}

TEST(RenderGraphResourceFilterTests, FiltersCombineAsAConjunction)
{
    EXPECT_EQ(CountAccepted({{"type", "Buffer"}, {"lifetime", "Persistent"}}), 2);
    EXPECT_EQ(CountAccepted({{"type", "Buffer"}, {"lifetime", "Persistent"}, {"aliveOnly", true}}), 1);
}

TEST(RenderGraphResourceFilterTests, SwapchainNamesTheBackbuffer)
{
    // Advertised from the start and never emitted: the backbuffer reported "Imported",
    // so a documented lifetime=Swapchain query could only ever answer empty.
    EXPECT_EQ(CountAccepted({{"lifetime", "Swapchain"}}), 1);
}

TEST(RenderGraphResourceFilterTests, MatchingIgnoresCase)
{
    EXPECT_EQ(CountAccepted({{"lifetime", "transient"}}), 3);
    EXPECT_EQ(CountAccepted({{"type", "BUFFER"}}), 3);
}

TEST(RenderGraphResourceFilterTests, AbsentAndEmptyBothMeanNoFilter)
{
    // The MCP layer drops empty strings before sending; the handler must not depend on it.
    EXPECT_EQ(CountAccepted({{"lifetime", ""}, {"type", ""}}), 7);
    EXPECT_EQ(CountAccepted({{"aliveOnly", false}}), 7);
}

TEST(RenderGraphResourceFilterTests, AValueNamingNothingIsAnErrorNotEverything)
{
    const RenderGraphResourceFilter lifetime(json{{"lifetime", "Nonsense"}});
    ASSERT_FALSE(lifetime.Error().empty()) << "an unrecognised lifetime must not read as unfiltered";
    EXPECT_NE(lifetime.Error().find("Nonsense"), std::string::npos) << "name the value that matched nothing";
    EXPECT_NE(lifetime.Error().find("Transient"), std::string::npos) << "state the values that would match";
    EXPECT_NE(lifetime.Error().find("Swapchain"), std::string::npos);

    const RenderGraphResourceFilter kind(json{{"type", "Sampler"}});
    ASSERT_FALSE(kind.Error().empty());
    EXPECT_NE(kind.Error().find("Texture"), std::string::npos);
    EXPECT_NE(kind.Error().find("Buffer"), std::string::npos);
}

TEST(RenderGraphResourceFilterTests, AWrongTypedValueIsAnErrorNotEverything)
{
    // Same trap one level down: skipping a value of the wrong type widens the answer
    // back to the full list, which is indistinguishable from a correct filtered one.
    const RenderGraphResourceFilter lifetime(json{{"lifetime", 123}});
    ASSERT_FALSE(lifetime.Error().empty());
    EXPECT_NE(lifetime.Error().find("Transient"), std::string::npos) << "state the values that would match";

    const RenderGraphResourceFilter alive(json{{"aliveOnly", "yes"}});
    ASSERT_FALSE(alive.Error().empty());
    EXPECT_NE(alive.Error().find("boolean"), std::string::npos);
}

TEST(RenderGraphResourceFilterTests, ANullValueMeansNoFilter)
{
    EXPECT_EQ(CountAccepted({{"lifetime", nullptr}, {"type", nullptr}, {"aliveOnly", nullptr}}), 7);
}

TEST(RenderGraphResourceFilterTests, AWellFormedRequestReportsNoError)
{
    EXPECT_TRUE(RenderGraphResourceFilter(json::object()).Error().empty());
    EXPECT_TRUE(RenderGraphResourceFilter(json{{"lifetime", "Imported"}, {"type", "Texture"}}).Error().empty());
}

} // namespace
