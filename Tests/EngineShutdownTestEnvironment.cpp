// Shuts EngineCore down after the last test of a test executable that can initialize it.
// Every such executable lists this file among its sources.
//
// EngineCore::GetInstance() leaks its instance on purpose, and suites initialize the engine
// in SetUpTestSuite and leave it running for the suites after them. A running engine keeps
// its job pool working, the startup asset scan among that work, while exit() destroys the
// function-local statics the work reads: an executable that returns from main with the
// engine still initialized can crash in a job worker after its last test passed.
// EngineCore::Shutdown() joins the asset scans and the job pool before RUN_ALL_TESTS
// returns, and does nothing when no test left the engine initialized.
#include "Core/Engine.h"

#include <gtest/gtest.h>

namespace
{

class EngineShutdownTestEnvironment : public ::testing::Environment
{
  public:
    void TearDown() override { GameEngine::EngineCore::GetInstance().Shutdown(); }
};

// GTest takes ownership and runs TearDown() after the last test.
[[maybe_unused]] const ::testing::Environment* const g_EngineShutdownTestEnvironment =
    ::testing::AddGlobalTestEnvironment(new EngineShutdownTestEnvironment());

} // namespace
