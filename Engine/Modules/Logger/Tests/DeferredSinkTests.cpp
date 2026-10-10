// What a DeferredSink hands to the sink it attaches: every record it held, in
// the order it was logged, ahead of anything logged after the attach.

#include "Logger/DeferredSink.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <cstdlib>
#endif
#include <memory>
#include <string>
#include <vector>

using namespace Logger;

namespace
{

struct ReceivedRecord
{
    LogLevel Level;
    std::string Text;
};

class RecordingSink final : public LogSink
{
  public:
    RecordingSink(std::vector<ReceivedRecord>* received, LogLevel minLevel)
        : m_Received(received), m_MinLevel(minLevel)
    {
    }

    void Write(const LogMessage& message) override { m_Received->push_back({message.Level, message.Message}); }
    void Flush() override {}
    bool ShouldLog(LogLevel level) const override { return level >= m_MinLevel; }
    String GetName() const override { return "Recording"; }

  private:
    std::vector<ReceivedRecord>* m_Received;
    LogLevel m_MinLevel;
};

// The child of a death test dies on a CRT assert; on Windows that assert would
// otherwise raise a modal dialog and hang the child instead of ending it.
void SuppressCrtDialogsInDeathTestChild()
{
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}

std::vector<std::string> Texts(const std::vector<ReceivedRecord>& records)
{
    std::vector<std::string> texts;
    for (const ReceivedRecord& record : records)
        texts.push_back(record.Text);
    return texts;
}

} // namespace

TEST(DeferredSink, HeldRecordsReachTheAttachedSinkBeforeLaterOnes)
{
    std::vector<ReceivedRecord> received;
    DeferredSink deferred;
    deferred.Write(LogMessage(LogLevel::Error, "held first"));
    deferred.Write(LogMessage(LogLevel::Warning, "held second"));
    EXPECT_TRUE(received.empty());

    deferred.Attach(std::make_unique<RecordingSink>(&received, LogLevel::Trace));
    deferred.Write(LogMessage(LogLevel::Info, "after attach"));

    EXPECT_EQ(Texts(received), (std::vector<std::string>{"held first", "held second", "after attach"}));
}

TEST(DeferredSink, TheAttachedSinkLevelFiltersHeldAndLaterRecords)
{
    std::vector<ReceivedRecord> received;
    DeferredSink deferred;
    deferred.Write(LogMessage(LogLevel::Debug, "held debug"));
    deferred.Write(LogMessage(LogLevel::Error, "held error"));

    deferred.Attach(std::make_unique<RecordingSink>(&received, LogLevel::Info));
    deferred.Write(LogMessage(LogLevel::Debug, "later debug"));
    deferred.Write(LogMessage(LogLevel::Warning, "later warning"));

    EXPECT_EQ(Texts(received), (std::vector<std::string>{"held error", "later warning"}));
}

TEST(DeferredSink, OverflowKeepsTheEarliestRecordsAndReportsTheDropCount)
{
    std::vector<ReceivedRecord> received;
    DeferredSink deferred(2);
    deferred.Write(LogMessage(LogLevel::Error, "cause"));
    deferred.Write(LogMessage(LogLevel::Info, "second"));
    deferred.Write(LogMessage(LogLevel::Info, "dropped one"));
    deferred.Write(LogMessage(LogLevel::Info, "dropped two"));

    deferred.Attach(std::make_unique<RecordingSink>(&received, LogLevel::Trace));

    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0].Text, "cause");
    EXPECT_EQ(received[1].Text, "second");
    EXPECT_EQ(received[2].Level, LogLevel::Error);
    EXPECT_NE(received[2].Text.find("2 records"), std::string::npos) << received[2].Text;
    // The attached sink may filter some held records by level, so the notice
    // counts what was held, not what reached it.
    EXPECT_NE(received[2].Text.find("first 2 were held"), std::string::npos) << received[2].Text;
}

// A lost diagnostic is an error: a sink that records only errors must still be
// told that records were dropped before it attached.
TEST(DeferredSink, AnErrorOnlySinkStillReceivesTheDropNotice)
{
    std::vector<ReceivedRecord> received;
    DeferredSink deferred(1);
    deferred.Write(LogMessage(LogLevel::Info, "held"));
    deferred.Write(LogMessage(LogLevel::Error, "dropped error"));

    deferred.Attach(std::make_unique<RecordingSink>(&received, LogLevel::Error));

    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].Level, LogLevel::Error);
    EXPECT_NE(received[0].Text.find("1 records"), std::string::npos) << received[0].Text;
}

#if !defined(NDEBUG)
// A second Attach would leave the first sink with every held record and throw the
// second away, so the caller learns nothing about where its log went.
TEST(DeferredSink, ASecondAttachAssertsInDevelopmentBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            std::vector<ReceivedRecord> received;
            DeferredSink deferred;
            deferred.Attach(std::make_unique<RecordingSink>(&received, LogLevel::Trace));
            deferred.Attach(std::make_unique<RecordingSink>(&received, LogLevel::Trace));
        },
        "Attach is called once");
}

TEST(DeferredSink, AttachingNoSinkAssertsInDevelopmentBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SuppressCrtDialogsInDeathTestChild();
            DeferredSink deferred;
            deferred.Attach(nullptr);
        },
        "needs the sink");
}
#endif
