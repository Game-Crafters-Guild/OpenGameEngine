#include <gtest/gtest.h>
#include "Logger/Logger.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

namespace LoggerOwnershipTest
{
struct BorrowedValue
{
    const char* Text;
    int* Formatted;
};
struct ThrowingValue
{
};
} // namespace LoggerOwnershipTest

template <>
struct std::formatter<LoggerOwnershipTest::BorrowedValue> : std::formatter<std::string_view>
{
    auto format(const LoggerOwnershipTest::BorrowedValue& value, std::format_context& context) const
    {
        ++*value.Formatted;
        return std::formatter<std::string_view>::format(value.Text, context);
    }
};

template <>
struct std::formatter<LoggerOwnershipTest::ThrowingValue> : std::formatter<std::string_view>
{
    std::format_context::iterator format(const LoggerOwnershipTest::ThrowingValue&,
                                         std::format_context&) const
    {
        throw std::format_error("deliberate formatter failure");
    }
};

namespace
{
using namespace Logger;

class CaptureSink : public LogSink
{
  public:
    void Write(const LogMessage& message) override { Messages.push_back(message); }
    void Flush() override {}
    bool ShouldLog(LogLevel) const override { return true; }
    String GetName() const override { return "Capture"; }
    // Tests read only after Flush has acknowledged all writes.
    const std::vector<LogMessage>& GetMessages() const { return Messages; }
    size_t GetCount() const { return Messages.size(); }

  private:
    std::vector<LogMessage> Messages;
};

class LoggerOwnershipTestFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Initialize({});
        Log::ClearSinks();
        auto sink = std::make_unique<CaptureSink>();
        Spy = sink.get();
        Log::AddSink(std::move(sink));
        Log::Flush();
    }
    void TearDown() override { Log::Shutdown(); }
    CaptureSink* Spy = nullptr;
};

TEST_F(LoggerOwnershipTestFixture, FormatterAndBorrowedDataAreConsumedBeforeReturn)
{
    char text[] = "producer text";
    char file[] = "producer.cpp";
    char function[] = "ProducerFunction";
    int formatted = 0;
    {
        std::unique_lock sinkLock(Log::GetStateForSharing()->Mutex);
        Log::LogWithSource(LogLevel::Info, file, 17, function, "value: {}",
                           LoggerOwnershipTest::BorrowedValue{text, &formatted});
        EXPECT_EQ(formatted, 1);
        std::memset(text, 'x', sizeof(text) - 1);
        std::memset(file, 'x', sizeof(file) - 1);
        std::memset(function, 'x', sizeof(function) - 1);
    }
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "value: producer text");
    EXPECT_EQ(messages[0].SourceFile, "producer.cpp");
    EXPECT_EQ(messages[0].Function, "ProducerFunction");
    EXPECT_EQ(messages[0].SourceLine, 17);
    EXPECT_EQ(formatted, 1);
}

TEST_F(LoggerOwnershipTestFixture, FilteredMessagesDoNotInvokeUserFormatters)
{
    Log::SetLogLevel(LogLevel::Warning);
    int formatted = 0;
    Log::Info("{}", LoggerOwnershipTest::BorrowedValue{"ignored", &formatted});
    Log::Flush();
    EXPECT_EQ(formatted, 0);
    EXPECT_EQ(Spy->GetCount(), 0u);
}

TEST_F(LoggerOwnershipTestFixture, OwnedSourceConsumesFormatterBeforeProducerReturns)
{
    char text[] = "producer text";
    int formatted = 0;
    {
        std::unique_lock sinkLock(Log::GetStateForSharing()->Mutex);
        const OwnedSourceLocation source("owned.cpp", "OwnedProducer");
        Log::LogWithSource(LogLevel::Info, source, 23, "value: {}",
                           LoggerOwnershipTest::BorrowedValue{text, &formatted});
        EXPECT_EQ(formatted, 1);
        std::memset(text, 'x', sizeof(text) - 1);
    }
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "value: producer text");
    EXPECT_EQ(messages[0].SourceFile, "owned.cpp");
    EXPECT_EQ(messages[0].Function, "OwnedProducer");
    EXPECT_EQ(messages[0].SourceLine, 23);
    EXPECT_EQ(formatted, 1);
}

TEST_F(LoggerOwnershipTestFixture, OwnedSourcePreservesFormattingFailureRecord)
{
    const OwnedSourceLocation source("owned.cpp", "OwnedProducer");
    EXPECT_NO_THROW(Log::LogWithSource(LogLevel::Info, source, 29, "{}",
                                       LoggerOwnershipTest::ThrowingValue{}));
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "[FORMAT ERROR: deliberate formatter failure]");
    EXPECT_EQ(messages[0].SourceFile, "owned.cpp");
    EXPECT_EQ(messages[0].SourceLine, 29);
}

TEST_F(LoggerOwnershipTestFixture, FormattingFailureStillProducesAnErrorRecord)
{
    EXPECT_NO_THROW(Log::Info("{}", LoggerOwnershipTest::ThrowingValue{}));
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "[FORMAT ERROR: deliberate formatter failure]");
}

TEST_F(LoggerOwnershipTestFixture, LargeArgumentsHaveNoDeferredCaptureLimit)
{
    const std::string value(100, 'a');
    Log::Info("{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}", value, value, value, value, value, value, value,
              value, value, value, value, value, value, value, value, value);
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, std::string(1600, 'a'));
}

TEST_F(LoggerOwnershipTestFixture, ConcurrentProducersKeepTheirOwnCallOrder)
{
    std::array<std::thread, 4> producers;
    for (unsigned producer = 0; producer < producers.size(); ++producer)
        producers[producer] = std::thread(
            [producer]
            {
                for (unsigned sequence = 0; sequence < 64; ++sequence)
                    Log::Info("{} {}", producer, sequence);
            });
    for (auto& producer : producers)
        producer.join();
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 256u);
    std::array<unsigned, 4> next{};
    for (const auto& message : messages)
    {
        unsigned producer = 99, sequence = 99;
        ASSERT_EQ(std::sscanf(message.Message.c_str(), "%u %u", &producer, &sequence), 2);
        ASSERT_LT(producer, next.size());
        EXPECT_EQ(sequence, next[producer]++);
    }
    for (const auto count : next)
        EXPECT_EQ(count, 64u);
}
} // namespace
