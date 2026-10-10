// EngineCore::s_Instance is the one record of which engine is current:
// GetInstance() returns it, and the constructor reads it to report a second
// live engine. A host- or test-owned EngineCore binds it for its lifetime and
// clears it on destruction; GetInstance() then binds the process's default
// instance again. The pointer is private, so the constructor's duplicate report
// is how the suite observes what is bound: it fires exactly when an engine is.
//
// Its own binary: the test needs to control when the process's default instance
// is created, which any earlier GetInstance() call in a shared binary would do.

#include "EngineLogCapture.h"

#include "Core/Engine.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr std::string_view kDuplicateInstanceNotice = "Engine instance already exists";

TEST(EngineCoreInstance, DestroyingAnOwnedEngineRebindsTheDefaultInstance)
{
    EngineCore& defaultEngine = EngineCore::GetInstance();

    auto owned = std::make_unique<EngineCore>();
    ASSERT_EQ(&EngineCore::GetInstance(), owned.get());
    owned.reset();
    ASSERT_EQ(&EngineCore::GetInstance(), &defaultEngine);

    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Debug);
    // Positive control on the instrument: a zero read out of a sink that
    // receives nothing would look like an unbound default instance.
    Logger::Log::Debug("EngineCoreInstance log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "log-capture self test"), 1u)
        << "the log capture is not receiving Debug-level engine lines";

    auto second = std::make_unique<EngineCore>();
    second.reset();
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, kDuplicateInstanceNotice), 1u)
        << "GetInstance() returned the default instance without binding it, so a new "
           "EngineCore did not see the default as a live engine";
}

} // namespace
} // namespace GameEngine
