#include "Core/EngineLoggerBridge.h"
#include "Logger/Logger.h"
#include "LoggerModuleProbe.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptManager.h"
#include <gtest/gtest.h>
#include <windows.h>
#include <cstdio>
#include <filesystem>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 17);
    }
}

class CaptureSink : public Logger::LogSink
{
  public:
    std::vector<Logger::LogMessage> Messages;
    void Write(const Logger::LogMessage& message) override { Messages.push_back(message); }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "NativeModuleCapture"; }
};

void RejectLoggingModule(bool drainFirst, bool logOnDetach)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    namespace ns = GameEngine::NativeScripting;
    namespace fs = std::filesystem;
    wchar_t executable[32768]{};
    Check(::GetModuleFileNameW(nullptr, executable, 32768) != 0, "missing executable path");
    const auto dll = fs::path(executable).parent_path() / "NativeLoggerModuleProbe.dll";
    const auto root =
        fs::temp_directory_path() / ("ge_logger_module_" + std::to_string(::GetCurrentProcessId()));
    Check(ns::WriteBuildCacheRecord(root / "NativeScripts" / "build",
                                    ns::BuildCacheRecord{"logger-test", dll.generic_string(), "",
                                                         ns::EngineBuildIdentity()}),
          "could not create isolated build record");

    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::ClearSinks();
    auto sink = std::make_unique<CaptureSink>();
    auto* spy = sink.get();
    Logger::Log::AddSink(std::move(sink));
    Logger::Log::Flush();
    std::unique_lock sinkLock(Logger::Log::GetStateForSharing()->Mutex);
    LoggerModuleProbeStats stats;
    std::uint64_t mappedBase = 0;
    ns::NativeScriptManager manager;
    ns::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapEnd = [&](std::uint64_t base, std::uint64_t)
    {
        mappedBase = base;
        const auto module = reinterpret_cast<HMODULE>(base);
        Check(module != nullptr, "module was not mapped");
        auto configure = reinterpret_cast<ConfigureLoggerModuleProbe>(
            ::GetProcAddress(module, "ConfigureProbe"));
        auto emit = reinterpret_cast<EmitLoggerModuleProbe>(::GetProcAddress(module, "EmitProbe"));
        Check(configure && emit, "missing logger probe exports");
        configure(&stats, logOnDetach);
        emit(29, true);
        if (drainFirst)
        {
            sinkLock.unlock();
            Logger::Log::Flush();
            sinkLock.lock();
        }
    };
    manager.SetModuleImageObserver(std::move(observer));
    Check(!manager.LoadPrebuiltUserModule(root, "LoggerProbe"), "missing-ABI module was accepted");
    Check(manager.LoadedModuleImageCount() == 0, "rejected module was retained");
    HMODULE owner = nullptr;
    Check(mappedBase && !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                              reinterpret_cast<LPCWSTR>(mappedBase), &owner),
          "rejected image is still mapped");
    sinkLock.unlock();
    Logger::Log::Flush();

    unsigned probeMessages = 0, detachMessages = 0;
    bool rejectionSeen = false;
    for (const auto& message : spy->Messages)
    {
        if (message.Message == "probe value 29")
            ++probeMessages;
        if (message.Message == "detach log 71")
            ++detachMessages;
        rejectionSeen |=
            message.Message.find("missing GE_UserModule_AbiVersion_v1 export") != std::string::npos;
    }
    Check(probeMessages == 1 && stats.Formatted == 1 && stats.LiveArguments == 0,
          "module log was lost or retained module-defined arguments");
    Check(detachMessages == (logOnDetach ? 1u : 0u), "module detach log was lost");
    Check(rejectionSeen, "rejection diagnostic was lost");
    Logger::Log::Shutdown();
    std::error_code error;
    fs::remove_all(root, error);
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

TEST(NativeModuleLogger, RejectedModuleCanDrainAfterActualUnload)
{
    EXPECT_EXIT(RejectLoggingModule(false, false), ::testing::ExitedWithCode(0), "");
}
TEST(NativeModuleLogger, RejectedModuleDetachCanLog)
{
    EXPECT_EXIT(RejectLoggingModule(false, true), ::testing::ExitedWithCode(0), "");
}
TEST(NativeModuleLogger, DrainBeforeRejectionControl)
{
    EXPECT_EXIT(RejectLoggingModule(true, false), ::testing::ExitedWithCode(0), "");
}
} // namespace
