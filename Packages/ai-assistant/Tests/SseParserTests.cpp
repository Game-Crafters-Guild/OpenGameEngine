#include "Providers/SseParser.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{
// Every line ending the grammar allows, a comment, a field the parser ignores,
// a two-line data field and an event with no type.
constexpr std::string_view kRecordedStream = ": keep-alive comment\r\n"
                                             "event: message_start\r\n"
                                             "data: {\"type\":\"message_start\"}\r\n"
                                             "\r\n"
                                             "id: 7\n"
                                             "event: content_block_delta\n"
                                             "data: first line\n"
                                             "data: second line\n"
                                             "\n"
                                             "data:no space after the colon\r"
                                             "\r"
                                             "event: ping\n"
                                             "data: {}\n"
                                             "\n";

std::vector<std::string> Parse(const std::vector<std::string_view>& chunks)
{
    SseParser parser;
    std::vector<std::string> events;
    for (const std::string_view chunk : chunks)
        parser.Feed(chunk, [&](const SseEvent& event) { events.push_back(event.Type + "|" + event.Data); });
    return events;
}
} // namespace

TEST(SseParserTests, FramesEventsAcrossLineEndingsCommentsAndMultiLineData)
{
    const std::vector<std::string> expected = {
        "message_start|{\"type\":\"message_start\"}",
        "content_block_delta|first line\nsecond line",
        "message|no space after the colon",
        "ping|{}",
    };
    EXPECT_EQ(Parse({kRecordedStream}), expected);
}

TEST(SseParserTests, SplittingTheStreamAtEveryByteBoundaryYieldsTheSameEvents)
{
    const std::vector<std::string> whole = Parse({kRecordedStream});
    ASSERT_EQ(whole.size(), 4u);
    for (size_t split = 0; split <= kRecordedStream.size(); ++split)
    {
        EXPECT_EQ(Parse({kRecordedStream.substr(0, split), kRecordedStream.substr(split)}), whole)
            << "split at byte " << split;
    }

    std::vector<std::string_view> bytes;
    for (size_t i = 0; i < kRecordedStream.size(); ++i)
        bytes.push_back(kRecordedStream.substr(i, 1));
    EXPECT_EQ(Parse(bytes), whole);
}
} // namespace GameEngine
