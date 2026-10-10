#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

// Declared with the ABI's own GE_API/GE_CDECL so the decoration matches the
// definition in ScriptingABI.cpp on every platform.
extern "C" {
    GE_API int32_t GE_CDECL GE_Debug_ResetScriptingCounters();
    GE_API int32_t GE_CDECL GE_Debug_GetScriptingCounter(int32_t which);
}

static vector<uint8_t> ReadAllFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static fs::path FindDomainRouting()
{
    const fs::path cands[] = {
        GameEngine::TestPaths::ExecutableDirectory() / "DomainRoutingTest.dll",
        GameEngine::TestPaths::StagedRoot() / "DomainRoutingTest.dll",
        GameEngine::Tests::DomainRoutingTestDll(),
        GameEngine::Tests::StagedBinDir() / "DomainRoutingTest.dll"
    };
    for (auto& p : cands) {
        if (fs::exists(p)) return p;
    }
    return {};
}

TEST(ScriptingAbi_TokenEnforcement, NameInvoke_CachesToken_ForSubsequentCalls)
{
    auto dll = FindDomainRouting();
    if (dll.empty()) GTEST_SKIP() << "DomainRoutingTest.dll not found";
    auto bytes = ReadAllFile(dll);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

#if defined(_DEBUG)
    ASSERT_EQ(GE_Debug_ResetScriptingCounters(), 0);
#endif

    int32_t res = -1;
    // Reset to 0
    ASSERT_EQ(GE_Invoke(dom, "GameEngine.Scripts.ScriptsEntryPoint.Reset", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.Reset"), &res), GE_Result_Ok);
    EXPECT_EQ(res, 0);

    // First name-based call should succeed; native will now cache token in background
    ASSERT_EQ(GE_Invoke(dom, "GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet"), &res), GE_Result_Ok);
    EXPECT_EQ(res, 1);

    // Second call should use token fast path via native cache
    ASSERT_EQ(GE_Invoke(dom, "GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.IncrementAndGet"), &res), GE_Result_Ok);
    EXPECT_EQ(res, 2);

#if defined(_DEBUG)
    int32_t byTok = GE_Debug_GetScriptingCounter(1);
    int32_t nameSlow = GE_Debug_GetScriptingCounter(2);
    EXPECT_GE(byTok, 1);     // token path must have been used at least once
    EXPECT_EQ(nameSlow, 0);  // slow name path not used (managed succeeded first time)
#endif
}

TEST(ScriptingAbi_TokenEnforcement, OutParam_And_DomainValidity_Semantics)
{
    auto dll = FindDomainRouting();
    if (dll.empty()) GTEST_SKIP() << "DomainRoutingTest.dll not found";
    auto bytes = ReadAllFile(dll);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    // Obtain a token
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, "GameEngine.Scripts.ScriptsEntryPoint.GetValue", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.GetValue"), &token), GE_Result_Ok);
    ASSERT_NE(token, 0ull);

    // Passing null outResult must be InvalidArg
    EXPECT_EQ(GE_InvokeByToken(dom, token, nullptr), GE_Result_InvalidArg);

    // Unload domain; InvokeByToken must fail when domain not loaded
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);
    int32_t out = -1;
    EXPECT_EQ(GE_InvokeByToken(dom, token, &out), GE_Result_Fail);
}

