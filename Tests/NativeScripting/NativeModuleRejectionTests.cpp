#include "Core/EngineLoggerBridge.h"
#include "Logger/CallbackSink.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptManager.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
namespace ns = GameEngine::NativeScripting;
namespace fs = std::filesystem;

struct RejectionCase
{
    const char* Name;
    const char* Dll;
    int Mode;
    const char* Error;
    int RegisterCalls;
};

class NativeModuleRejection : public testing::TestWithParam<RejectionCase>
{
};

TEST_P(NativeModuleRejection, ReportsBeforeCleanupAndNotifiesAfterDetach)
{
    const auto& testCase = GetParam();
    wchar_t exePath[MAX_PATH]{};
    ASSERT_NE(::GetModuleFileNameW(nullptr, exePath, MAX_PATH), 0u);
    const auto dll = fs::path(exePath).parent_path() / testCase.Dll;
    ASSERT_TRUE(fs::exists(dll));
    const auto root = fs::temp_directory_path() /
        ("ge_rejection_" + std::to_string(::GetCurrentProcessId()) + "_" + testCase.Name);
    ASSERT_TRUE(ns::WriteBuildCacheRecord(root / "NativeScripts" / "build",
        ns::BuildCacheRecord{"test", dll.generic_string(), "", ns::EngineBuildIdentity()}));

    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    auto messages = std::make_shared<std::vector<std::string>>();
    auto sink = std::make_unique<Logger::CallbackSink>();
    sink->RegisterCallback([messages](const Logger::LogMessage& msg) {
        messages->push_back(msg.Message);
    });
    Logger::Log::AddSink(std::move(sink));

    int detachCount = 0;
    int purgeCount = 0;
    int unmappingCount = 0;
    int unmappedCount = 0;
    HMODULE mapped = nullptr;
    ns::NativeScriptManager manager;
    ns::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapEnd = [&](std::uint64_t base, std::uint64_t) {
        mapped = reinterpret_cast<HMODULE>(base);
        ASSERT_NE(mapped, nullptr);
        const auto configure = reinterpret_cast<void (*)(int, int*)>(
            ::GetProcAddress(mapped, "GE_RejectionProbe_Configure"));
        ASSERT_NE(configure, nullptr);
        configure(testCase.Mode, &detachCount);
    };
    manager.SetModuleRegistrationPurgeHandler([&](std::string_view moduleId, std::uint64_t generation) {
        ++purgeCount;
        EXPECT_EQ(moduleId, "RejectionProbe");
        EXPECT_EQ(generation, 1u);
        EXPECT_EQ(detachCount, 0);
        // Drain while still mapped. This proves the rejection was enqueued
        // before the first externally supplied cleanup callback was invoked.
        Logger::Log::Flush();
        EXPECT_TRUE(std::any_of(messages->begin(), messages->end(), [&](const auto& message) {
            return message.find(testCase.Error) != std::string::npos;
        }));
        const auto calls = reinterpret_cast<int (*)()>(
            ::GetProcAddress(mapped, "GE_RejectionProbe_RegisterCalls"));
        ASSERT_NE(calls, nullptr);
        EXPECT_EQ(calls(), testCase.RegisterCalls);
    });
    observer.ImageUnmapping = [&](std::uint64_t base, std::uint64_t) {
        ++unmappingCount;
        EXPECT_EQ(reinterpret_cast<HMODULE>(base), mapped);
        EXPECT_EQ(purgeCount, 1);
        EXPECT_EQ(detachCount, 0);
    };
    observer.ImageUnmapped = [&](std::uint64_t base) {
        ++unmappedCount;
        EXPECT_EQ(unmappingCount, 1);
        EXPECT_EQ(detachCount, 1) << "the notification preceded DLL static destruction";
        HMODULE owner = nullptr;
        EXPECT_FALSE(::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCWSTR>(base), &owner));
    };
    manager.SetModuleImageObserver(std::move(observer));

    EXPECT_FALSE(manager.LoadPrebuiltUserModule(root, "RejectionProbe"));
    EXPECT_EQ(manager.LoadedModuleImageCount(), 0u);
    EXPECT_EQ(purgeCount, 1);
    EXPECT_EQ(unmappingCount, 1);
    EXPECT_EQ(unmappedCount, 1);
    EXPECT_EQ(detachCount, 1);
    Logger::Log::Flush();
    Logger::Log::ClearSinks();
    std::error_code ec;
    fs::remove_all(root, ec);
}

INSTANTIATE_TEST_SUITE_P(HandshakeGates, NativeModuleRejection, testing::Values(
    RejectionCase{"MissingAbi", "RejectedModuleMissingAbi.dll", 0,
                  "missing GE_UserModule_AbiVersion_v1 export", 0},
    RejectionCase{"AbiMismatch", "RejectedModuleProbe.dll", 1, "ABI version mismatch", 0},
    RejectionCase{"OldUserSystemAbi", "RejectedModuleProbe.dll", 4,
                  "ABI version mismatch: module=1 host=2", 0},
    RejectionCase{"MissingFingerprint", "RejectedModuleMissingFingerprint.dll", 0,
                  "missing GE_UserModule_ToolchainFingerprint_v1 export", 0},
    RejectionCase{"FingerprintMismatch", "RejectedModuleProbe.dll", 2,
                  "CompilerVersionMinor mismatch", 0},
    RejectionCase{"RegisterFailure", "RejectedModuleProbe.dll", 3,
                  "GE_UserModule_Register_v1 returned 17", 1}),
    [](const testing::TestParamInfo<RejectionCase>& info) { return info.param.Name; });
}
