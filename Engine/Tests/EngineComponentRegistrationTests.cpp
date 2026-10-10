// Engine components have one ComponentRegistry registrar: AutoComponentRegistrar<T>,
// at static init or on the type's first World use, keyed by the canonical type
// name — the same string the type id is the hash of. EngineCore::Initialize
// registers no engine component itself — only plugin components arrive through a
// hook — so one Initialize must re-register nothing, and every registry entry
// must be spelled by its canonical type name.
//
// A second registrar fails in one of two ways, and the two tests pin one each. If
// it reaches a type AFTER the canonical registrar, the registry skips it and the
// only trace is its "already registered, skipping" line (the first test reads the
// log for exactly that). If it reaches a type FIRST under another spelling — the
// short "Transform" rather than "GameEngine::Components::Transform" — the entry's
// name no longer hashes to its type id and every name-keyed lookup for that type
// misses (the second test sweeps the registry for that).

#include "EngineLogCapture.h"

#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ComponentTypeName.h"
#include "ECS/ECS.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr std::string_view kDuplicateRegistrationNotice = "already registered, skipping";

// One headless EngineCore::Initialize per test, with every engine log line down
// to Debug captured across it. The workspace is a throwaway directory so the
// asset scan is empty and nothing is written next to the staged binaries.
class EngineComponentRegistrationTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Capture.emplace(&m_Lines, Logger::LogLevel::Debug);

        // Positive control on the instrument: a zero read out of a sink that
        // receives nothing would pass a broken registration.
        Logger::Log::Debug("EngineComponentRegistration log-capture self test");
        Logger::Log::Flush();
        ASSERT_EQ(TestLog::CountLinesContaining(m_Lines, "log-capture self test"), 1u)
            << "the log capture is not receiving Debug-level engine lines";
        m_Lines.clear();

        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("EngineComponentRegistrationTests");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
        Logger::Log::Flush();
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        std::filesystem::remove_all(m_Workspace, ec);
    }

    // Declared before the capture so the sink is torn down first.
    std::vector<std::string> m_Lines;
    std::optional<TestLog::ScopedEngineLogCapture> m_Capture;
    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
};

TEST_F(EngineComponentRegistrationTests, SingleInitializeRegistersNoComponentTwice)
{
    std::string duplicates;
    for (const std::string& line : m_Lines)
        if (line.find(kDuplicateRegistrationNotice) != std::string::npos)
            duplicates += "\n  " + line;
    EXPECT_TRUE(duplicates.empty())
        << "a second registrar re-registered engine components during Initialize:" << duplicates;
}

TEST_F(EngineComponentRegistrationTests, EveryRegisteredComponentIsKeyedByItsCanonicalTypeName)
{
    // The sweep must cover the engine's own components, not an empty registry.
    ASSERT_NE(ECS::ComponentRegistry::GetComponentInfo(ECS::GetComponentTypeId<Components::Transform>()),
              nullptr);

    for (const std::string& name : ECS::ComponentRegistry::GetAllComponentNames())
    {
        const ECS::ComponentRegistry::ComponentInfo* info = ECS::ComponentRegistry::GetComponentInfo(name);
        ASSERT_NE(info, nullptr) << name;
        EXPECT_EQ(info->TypeId, ECS::Hash64(name))
            << "'" << name << "' is registered under a spelling other than its canonical type name";
    }
}

} // namespace
} // namespace GameEngine
