// The finite-coordinate gate every debug-server pointer request passes through, and
// the reply it refuses with.
//
// The predicate itself is covered by InjectedInputRoutingTests; what is pinned here is
// the request-shaped half nothing else reaches: which request field a rejection names,
// and that the reply echoes the RAW number the caller sent rather than the infinity it
// narrowed to. A rejection that said only "coordinates must be finite" would leave a
// caller of drag_element — four coordinate fields in one request — with no way to tell
// which one it got wrong.
//
// Requests are parsed from JSON text, the form they actually arrive in: 1e300 is an
// ordinary JSON number, and only the narrowing cast to float makes it infinite.
#include <gtest/gtest.h>

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/PointerCoordsRequest.h"

#include <cmath>
#include <string>

using namespace GameEngine;
using nlohmann::json;

namespace
{
std::string RejectionMessage(const json& params, const char* xKey, const char* yKey)
{
    float x = 0.0f;
    float y = 0.0f;
    json error;
    EXPECT_FALSE(Editor::ReadPointerCoords(params, xKey, yKey, x, y, error));
    EXPECT_TRUE(Editor::IsRefusal(error)) << error.dump();
    return Editor::HandlerResponse("0", error).value("error", std::string());
}
} // namespace

TEST(PointerCoordsRequestTests, FinitePairIsAcceptedAndLeavesTheErrorUntouched)
{
    const json params = json::parse(R"({"x": 120.5, "y": -60.25})");

    float x = 0.0f;
    float y = 0.0f;
    json error = json();

    EXPECT_TRUE(Editor::ReadPointerCoords(params, "x", "y", x, y, error));
    EXPECT_FLOAT_EQ(x, 120.5f);
    EXPECT_FLOAT_EQ(y, -60.25f);
    // The handlers return `error` verbatim on a false result and ignore it otherwise;
    // an acceptance that wrote to it would still be answered with, so it stays null.
    EXPECT_TRUE(error.is_null()) << error.dump();
}

// The float range's own edge stays acceptable: the gate rejects what cannot be
// represented, not what is merely large.
TEST(PointerCoordsRequestTests, LargeButRepresentableCoordinatesAreAccepted)
{
    const json params = json::parse(R"({"x": 3.0e38, "y": -3.0e38})");

    float x = 0.0f;
    float y = 0.0f;
    json error;

    EXPECT_TRUE(Editor::ReadPointerCoords(params, "x", "y", x, y, error));
    EXPECT_TRUE(std::isfinite(x));
    EXPECT_TRUE(std::isfinite(y));
}

TEST(PointerCoordsRequestTests, OutOfRangeXIsRejectedNamingTheFieldsAndEchoingTheRawValue)
{
    const json params = json::parse(R"({"x": 1e300, "y": 60})");
    const std::string message = RejectionMessage(params, "x", "y");

    EXPECT_NE(message.find("x/y must be finite numbers in float range"), std::string::npos) << message;
    // The raw request value, not the infinity it became — a reply reading "got x=inf"
    // describes the narrowing rather than the number the caller has to fix.
    EXPECT_NE(message.find("x=" + params["x"].dump()), std::string::npos) << message;
    EXPECT_NE(message.find("y=" + params["y"].dump()), std::string::npos) << message;
    EXPECT_EQ(message.find("inf"), std::string::npos) << message;
    EXPECT_NE(message.find("Pass UI-logical pixel coordinates."), std::string::npos) << message;
}

TEST(PointerCoordsRequestTests, OutOfRangeYIsRejectedNamingTheFieldsAndEchoingTheRawValue)
{
    const json params = json::parse(R"({"x": 120, "y": -1e300})");
    const std::string message = RejectionMessage(params, "x", "y");

    EXPECT_NE(message.find("x/y must be finite numbers in float range"), std::string::npos) << message;
    EXPECT_NE(message.find("x=" + params["x"].dump()), std::string::npos) << message;
    EXPECT_NE(message.find("y=" + params["y"].dump()), std::string::npos) << message;
    EXPECT_EQ(message.find("inf"), std::string::npos) << message;
}

TEST(PointerCoordsRequestTests, BothCoordinatesOutOfRangeAreBothEchoed)
{
    const json params = json::parse(R"({"x": 1e300, "y": -1e300})");
    const std::string message = RejectionMessage(params, "x", "y");

    EXPECT_NE(message.find("x=" + params["x"].dump()), std::string::npos) << message;
    EXPECT_NE(message.find("y=" + params["y"].dump()), std::string::npos) << message;
    EXPECT_EQ(message.find("inf"), std::string::npos) << message;
}

// drag_element reads two pairs out of one request. The reply has to name the pair it
// rejected, or the caller cannot tell a bad start from a bad end.
TEST(PointerCoordsRequestTests, RejectionNamesTheDragFieldsItWasAskedFor)
{
    const json params = json::parse(R"({"startX": 1e300, "startY": 10, "endX": 20, "endY": 1e300})");

    const std::string startMessage = RejectionMessage(params, "startX", "startY");
    EXPECT_NE(startMessage.find("startX/startY must be finite numbers in float range"), std::string::npos)
        << startMessage;
    EXPECT_NE(startMessage.find("startX=" + params["startX"].dump()), std::string::npos) << startMessage;
    EXPECT_EQ(startMessage.find("endX"), std::string::npos) << startMessage;

    const std::string endMessage = RejectionMessage(params, "endX", "endY");
    EXPECT_NE(endMessage.find("endX/endY must be finite numbers in float range"), std::string::npos) << endMessage;
    EXPECT_NE(endMessage.find("endY=" + params["endY"].dump()), std::string::npos) << endMessage;
    EXPECT_EQ(endMessage.find("startX"), std::string::npos) << endMessage;
}
