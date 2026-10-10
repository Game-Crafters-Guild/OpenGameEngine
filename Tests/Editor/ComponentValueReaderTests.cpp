#include <gtest/gtest.h>

#include "DebugServer/ComponentValueReader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using GameEngine::Editor::ComponentValueReader;
using json = nlohmann::json;

// The defect this locks: `set_component` used to answer `ok: true` for a values object
// whose keys named no field, because a branch built on `values.contains("Density")`
// cannot tell a misspelling from an omission. Measured live before the fix — a
// TerrainGrass write of {"density": 0.25} (wrong case) reported success and moved
// nothing. Reporting the leftovers is what turns that into an error the caller can act on.

TEST(ComponentValueReaderTests, ReportsKeysNobodyAskedAbout)
{
    const json values = {{"Density", 0.5}, {"density", 0.25}, {"NoSuchField", 1}};
    ComponentValueReader reader(values);

    EXPECT_TRUE(reader.Has("Density"));
    EXPECT_FALSE(reader.Has("BladeHeight")); // a field the request never supplied

    const std::vector<std::string> left = reader.UnconsumedKeys();
    ASSERT_EQ(left.size(), 2u) << "the wrong-case and unknown keys must both be reported";
    EXPECT_NE(std::find(left.begin(), left.end(), "density"), left.end());
    EXPECT_NE(std::find(left.begin(), left.end(), "NoSuchField"), left.end());
}

TEST(ComponentValueReaderTests, EveryConsumedKeyLeavesNothingToReport)
{
    const json values = {{"Density", 0.5}, {"BladeHeight", 2.0}};
    ComponentValueReader reader(values);
    EXPECT_TRUE(reader.Has("Density"));
    EXPECT_TRUE(reader.Has("BladeHeight"));
    EXPECT_TRUE(reader.UnconsumedKeys().empty());
}

// An empty values object is the documented "add this component with defaults" request,
// so it must not be reported as an unapplied write.
TEST(ComponentValueReaderTests, EmptyValuesReportNothing)
{
    ComponentValueReader reader(json::object());
    EXPECT_FALSE(reader.Has("Density"));
    EXPECT_TRUE(reader.UnconsumedKeys().empty());
}

// A branch owns the outcome of a field it recognised, including a value it rejects on its
// own terms — otherwise a rejected-but-understood key would be double-reported as unknown.
TEST(ComponentValueReaderTests, AskingConsumesRegardlessOfTheValue)
{
    const json values = {{"Density", "not-a-number"}};
    ComponentValueReader reader(values);
    EXPECT_TRUE(reader.Has("Density"));
    EXPECT_TRUE(reader.UnconsumedKeys().empty());
}

TEST(ComponentValueReaderTests, ValueReadsThroughAndConsumes)
{
    const json values = {{"closed", true}, {"defaultRadius", 7.5}, {"stray", 1}};
    ComponentValueReader reader(values);
    EXPECT_TRUE(reader.Value("closed", false));
    EXPECT_FLOAT_EQ(reader.Value("defaultRadius", 5.0f), 7.5f);
    EXPECT_FLOAT_EQ(reader.Value("absent", 5.0f), 5.0f); // fallback, and no phantom key

    const std::vector<std::string> left = reader.UnconsumedKeys();
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0], "stray");
}

// A JSON null means "leave this alone"; it must still count as consumed so it is not
// reported, and Value must fall back rather than throw on the get<T>().
TEST(ComponentValueReaderTests, NullValueConsumesAndFallsBack)
{
    const json values = {{"mass", nullptr}};
    ComponentValueReader reader(values);
    EXPECT_FLOAT_EQ(reader.Value("mass", 3.0f), 3.0f);
    EXPECT_TRUE(reader.UnconsumedKeys().empty());
}

TEST(ComponentValueReaderTests, IndexingReturnsTheValueAndNullForAbsent)
{
    const json values = {{"radius", 4.0}};
    ComponentValueReader reader(values);
    ASSERT_TRUE(reader.Has("radius"));
    EXPECT_FLOAT_EQ(reader["radius"].get<float>(), 4.0f);
    EXPECT_TRUE(reader["missing"].is_null());
}

// Non-object payloads (a stray array or scalar) must not be mistaken for a key set.
TEST(ComponentValueReaderTests, NonObjectValuesHaveNoKeys)
{
    const json values = json::array({1, 2, 3});
    ComponentValueReader reader(values);
    EXPECT_FALSE(reader.Has("anything"));
    EXPECT_TRUE(reader.UnconsumedKeys().empty());
}

// Reported order follows the request so the error message reads back in the order the
// caller wrote their keys.
TEST(ComponentValueReaderTests, UnconsumedKeysKeepRequestOrder)
{
    const json values = json::parse(R"({"zzz":1,"aaa":2,"mmm":3})");
    ComponentValueReader reader(values);
    const std::vector<std::string> left = reader.UnconsumedKeys();
    ASSERT_EQ(left.size(), 3u);
    // nlohmann's default object is ordered by key; the reader preserves whatever order it
    // iterates, so the contract is "same order as iteration", not "insertion".
    EXPECT_EQ(left[0], "aaa");
    EXPECT_EQ(left[1], "mmm");
    EXPECT_EQ(left[2], "zzz");
}

// ---------------------------------------------------------------------------
// The unit tests above prove the reader reports leftovers. They cannot prove the
// component-write branches USE it, which is where the defect actually lived — so this
// scan is that half, in the DebugServerReadPurityTests style: a branch that goes back to
// raw `values.contains(...)` / `values[...]` silently drops unmatched keys again.
// ---------------------------------------------------------------------------

std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST(ComponentValueKeyContract, ComponentWritesReadKeysThroughTheTrackingReader)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" / "DebugServer" / "DebugHandlers.cpp";
    const std::string src = ReadSource(path);
    ASSERT_FALSE(src.empty()) << "could not read " << path.string();

    // Instrument check first: a scan whose patterns match nothing anywhere would pass on
    // any file at all. The reader's own name must be present, or the scan is vacuous.
    ASSERT_NE(src.find("ComponentValueReader"), std::string::npos)
        << "DebugHandlers.cpp no longer mentions ComponentValueReader — this scan would pass "
           "vacuously; re-point it at whatever replaced the reader.";

    for (const char* banned : {"values.contains(", "values[\"", "values.value("})
    {
        const std::size_t at = src.find(banned);
        EXPECT_EQ(at, std::string::npos)
            << "DebugHandlers.cpp reads a component-values key through raw `" << banned
            << "` — an unmatched key is then indistinguishable from an absent one and the "
               "write reports ok while changing nothing. Read it through "
               "ComponentValueReader::Has/Value so the tail can report the leftovers.";
    }
}
} // namespace
