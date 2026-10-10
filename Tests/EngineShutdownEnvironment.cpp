#include <gtest/gtest.h>

#include "Core/Engine.h"

// Fixtures in a test executable initialize the process-wide EngineCore and
// leave it running. Returning from main with the engine up ends the process
// while job workers still execute (a registry scan, for example) as static
// destruction tears down the tables they read. Shutting the engine down first
// joins the workers while the process is still in a well-defined state.
//
// Linking this file into a test executable registers the environment. Global
// environments tear down after every suite, so no fixture still needs the engine.
namespace
{

class EngineShutdownEnvironment : public ::testing::Environment
{
  public:
    void TearDown() override
    {
        GameEngine::EngineCore& engine = GameEngine::EngineCore::GetInstance();
        engine.Shutdown();
        EXPECT_FALSE(engine.IsInitialized());
    }
};

const bool g_EngineShutdownEnvironmentRegistered = []
{
    ::testing::AddGlobalTestEnvironment(new EngineShutdownEnvironment());
    return true;
}();

} // namespace
