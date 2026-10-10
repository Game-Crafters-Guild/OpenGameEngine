#include "Panels/LogView.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using GameEngine::LogView;

namespace
{

Logger::LogMessage MakeMsg(const char* text, const char* file, const char* function)
{
    Logger::LogMessage msg;
    msg.Level = Logger::LogLevel::Info;
    msg.Message = text ? text : "";
    msg.Timestamp = std::chrono::system_clock::now();
    msg.SetSourceLocation(file, 42, function);
    return msg;
}

const LogView::StoredMessage* FlushOne(LogView& view)
{
    view.FlushPendingMessages();
    EXPECT_EQ(view.GetMessageCount(), 1u);
    return view.GetMessage(0);
}

} // namespace

TEST(LogViewTests, CopiesValidSourceAndFunction)
{
    LogView view;
    view.AddMessage(MakeMsg("hello", "Foo.cpp", "Bar"));
    const auto* stored = FlushOne(view);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->Text, "hello");
    EXPECT_EQ(stored->SourceFile, "Foo.cpp");
    EXPECT_EQ(stored->Function, "Bar");
    EXPECT_EQ(stored->SourceLine, 42);
}

TEST(LogViewTests, NullSourceAndFunctionBecomeEmpty)
{
    LogView view;
    view.AddMessage(MakeMsg("hello", nullptr, nullptr));
    const auto* stored = FlushOne(view);
    ASSERT_NE(stored, nullptr);
    EXPECT_TRUE(stored->SourceFile.empty());
    EXPECT_TRUE(stored->Function.empty());
}
