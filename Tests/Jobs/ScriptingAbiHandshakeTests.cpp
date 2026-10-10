#include <gtest/gtest.h>
#include "Scripting/ScriptingTestHooks.h"
#include "Scripting/CoreCLRHost.h"
#include <string>

using namespace GameEngine;

TEST(ScriptingAbiHandshake, HostStoresLastInitResult_NotFound) {
#ifdef _DEBUG
    SetCoreBridgeInitResultOverrideForTests(-3);
    CoreCLRHost host;
    // Should early-out due to test override, without needing runtime initialization
    bool ok = host.InitializeCoreBridge(L"Dummy.CoreBridge.dll");
    EXPECT_FALSE(ok);
    EXPECT_EQ(host.GetLastCoreBridgeInitResult(), -3);
    // Clear override
    SetCoreBridgeInitResultOverrideForTests((std::numeric_limits<int32_t>::max)());
#else
    GTEST_SKIP() << "Test-only hook available in Debug builds only";
#endif
}

TEST(ScriptingAbiHandshake, HostStoresLastInitResult_OtherError) {
#ifdef _DEBUG
    SetCoreBridgeInitResultOverrideForTests(-42);
    CoreCLRHost host;
    bool ok = host.InitializeCoreBridge(L"Dummy.CoreBridge.dll");
    EXPECT_FALSE(ok);
    EXPECT_EQ(host.GetLastCoreBridgeInitResult(), -42);
    SetCoreBridgeInitResultOverrideForTests((std::numeric_limits<int32_t>::max)());
#else
    GTEST_SKIP() << "Test-only hook available in Debug builds only";
#endif
}

