#pragma once

// A NativeScriptManager with one package module build dispatched and still in flight,
// for tests of what waits on the editor's module builds (the packaged export).
//
// The build is real: RequestRebuild + Tick dispatch it to a worker. Its generated-project
// directory sits under a regular file, so the worker fails fast without a compiler. The
// build stays in flight until a Tick on the constructing thread applies its completion
// task; `onCompletion` runs inside that task, where a successful build writes its record.
// The completion is counted only after `onCompletion` returns.

#include "JobSystem/WorkStealingThreadPool.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace GameEngine::NativeScripting::Testing
{

// Shipped: the module ships in a packaged game (a runtime package module).
// EditorOnly: an Editor-kind package module, marked the way the editor marks it
// (a non-empty NativeBuildConfig::EditorImportLib); a packaged game never ships it.
enum class ModuleKind
{
    Shipped,
    EditorOnly,
};

class InFlightModuleBuild
{
public:
    // `sourceDir` gets one source file; `cacheDir` holds NativeScripts/{build,active};
    // `scratchDir` holds the file that blocks the generated-project directory.
    InFlightModuleBuild(const std::filesystem::path& sourceDir, const std::filesystem::path& cacheDir,
                        const std::filesystem::path& scratchDir, const std::string& moduleName,
                        ModuleKind kind, std::function<void()> onCompletion)
        : m_Pool(2), m_OnCompletion(std::move(onCompletion))
    {
        std::error_code ec;
        std::filesystem::create_directories(sourceDir, ec);
        std::filesystem::create_directories(scratchDir, ec);
        std::ofstream(sourceDir / (moduleName + ".cpp")) << "int GeInFlightModuleBuildProbe() { return 1; }\n";
        std::ofstream(scratchDir / "not-a-directory") << "blocks the generated project directory";

        NativeBuildConfig config;
        config.SourceDir = sourceDir;
        config.ProjectDir = scratchDir / "not-a-directory" / "project";
        config.BuildDir = cacheDir / "NativeScripts" / "build";
        config.ActiveDir = cacheDir / "NativeScripts" / "active";
        config.ModuleName = moduleName;
        if (kind == ModuleKind::EditorOnly)
            config.EditorImportLib = scratchDir / "EditorSDK.lib"; // absent file: only the kind matters

        EXPECT_TRUE(m_Manager.Initialize(&m_Pool));
        m_Manager.SetPackageModuleConfigs({config});
        m_Manager.SetBuildCompletedCallback([this](const NativeBuildResult&) {
            if (m_OnCompletion)
                m_OnCompletion();
            ++m_Completions;
        });
        m_Manager.RequestRebuild();
        m_Manager.Tick(); // dispatches the build; in flight until a later Tick applies it
    }

    ~InFlightModuleBuild() { m_Manager.Shutdown(); }

    InFlightModuleBuild(const InFlightModuleBuild&) = delete;
    InFlightModuleBuild& operator=(const InFlightModuleBuild&) = delete;

    NativeScriptManager& Manager() { return m_Manager; }
    int Completions() const { return m_Completions.load(); }

    // Ticks (as the editor's frame loop does) until the completion task has run or
    // the deadline passes.
    void TickUntilCompleted(std::chrono::steady_clock::time_point deadline)
    {
        while (m_Completions.load() == 0 && std::chrono::steady_clock::now() < deadline)
        {
            m_Manager.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

private:
    JobSystem::WorkStealingThreadPool m_Pool;
    NativeScriptManager m_Manager;
    std::function<void()> m_OnCompletion;
    std::atomic<int> m_Completions{0};
};

} // namespace GameEngine::NativeScripting::Testing
