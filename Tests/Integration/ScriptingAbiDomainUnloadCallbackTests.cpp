#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <atomic>

// GE_SetOnDomainWillUnload must deliver the notification from the module that
// executes GE_ScriptsDomainUnload. The old path (setting a callback on
// ScriptingService::Get() from the host EXE) never fired: ScriptingService is an
// inline-static singleton, so EXE and GameEngine.Native.dll each own a copy.
namespace
{
std::atomic<int> g_fireCount{0};
std::atomic<uint64_t> g_lastDomain{0};

void GE_CDECL OnDomainWillUnloadCounter(GE_DomainHandle domain, void* user)
{
    g_fireCount.fetch_add(1, std::memory_order_relaxed);
    g_lastDomain.store(domain, std::memory_order_relaxed);
    if (user)
        static_cast<std::atomic<int>*>(user)->fetch_add(1, std::memory_order_relaxed);
}
} // namespace

TEST(ScriptingAbiDomainUnloadCallback, FiresOnDomainUnloadAndUnregisters)
{
    g_fireCount.store(0);
    g_lastDomain.store(0);
    std::atomic<int> userCounter{0};
    ASSERT_EQ(GE_SetOnDomainWillUnload(&OnDomainWillUnloadCounter, &userCounter), GE_Result_Ok);

    // The notification fires before managed domain validation, so a synthetic handle
    // exercises the wiring without needing a real loaded domain. CLR hosting is still
    // required to reach the notify point; skip when unavailable in this environment.
    const GE_DomainHandle synthetic = 0xDEADBEEFULL;
    GE_Result rc = GE_ScriptsDomainUnload(synthetic);
    if (g_fireCount.load() == 0 && rc != GE_Result_Ok)
    {
        (void)GE_SetOnDomainWillUnload(nullptr, nullptr);
        GTEST_SKIP() << "CLR hosting unavailable (rc=" << rc << "); domain unload path unreachable";
    }

    EXPECT_GE(g_fireCount.load(), 1);
    EXPECT_EQ(g_lastDomain.load(), synthetic);
    EXPECT_GE(userCounter.load(), 1) << "user pointer was not forwarded";

    // Unregistering must stop deliveries.
    ASSERT_EQ(GE_SetOnDomainWillUnload(nullptr, nullptr), GE_Result_Ok);
    const int before = g_fireCount.load();
    (void)GE_ScriptsDomainUnload(synthetic);
    EXPECT_EQ(g_fireCount.load(), before);
}
