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

TEST(ScriptingAbi_TokenCache, PurgedOnDomainUnload)
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

    // Query a token and invoke it once
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, "GameEngine.Scripts.ScriptsEntryPoint.GetValue", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.GetValue"), &token), GE_Result_Ok);
    ASSERT_NE(token, 0ull);
    int32_t res = -1;
    ASSERT_EQ(GE_InvokeByToken(dom, token, &res), GE_Result_Ok);

#if defined(_DEBUG)
    EXPECT_EQ(GE_Debug_GetScriptingCounter(1), 1); // by token once
#endif

    // Unload domain
    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);

    // Token must be invalid now
    EXPECT_EQ(GE_InvokeByToken(dom, token, &res), GE_Result_Fail);

#if defined(_DEBUG)
    ASSERT_EQ(GE_Debug_ResetScriptingCounters(), 0);
#endif

    // Recreate domain and invoke by name; first call should go through name path and cache anew
    dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    ASSERT_EQ(GE_Invoke(dom, "GameEngine.Scripts.ScriptsEntryPoint.GetValue", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.GetValue"), &res), GE_Result_Ok);
    ASSERT_EQ(GE_Invoke(dom, "GameEngine.Scripts.ScriptsEntryPoint.GetValue", (uint32_t)strlen("GameEngine.Scripts.ScriptsEntryPoint.GetValue"), &res), GE_Result_Ok);

#if defined(_DEBUG)
    // After two name-based invokes, at least one should have used token (second call) and no slow-name was necessary if managed succeeded first call
    EXPECT_GE(GE_Debug_GetScriptingCounter(1), 1);
    EXPECT_EQ(GE_Debug_GetScriptingCounter(2), 0);
#endif
}

