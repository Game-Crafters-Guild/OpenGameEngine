#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <vector>

// Debug counters exposed only in debug builds. Declared with the ABI's own
// GE_API/GE_CDECL so the decoration matches the definition in ScriptingABI.cpp
// on every platform.
extern "C" {
    GE_API int32_t GE_CDECL GE_Debug_ResetScriptingCounters();
    GE_API int32_t GE_CDECL GE_Debug_GetScriptingCounter(int32_t which);
}

static std::vector<uint8_t> ReadAllFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(ScriptingAbi_TokenPath, InvokeLoop_UsesOnlyTokenFastPath)
{
    // Load test assembly
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllFile(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &token), GE_Result_Ok);
    ASSERT_NE(token, 0ull);

#if defined(_DEBUG)
    ASSERT_EQ(GE_Debug_ResetScriptingCounters(), 0);
#endif

    // Invoke by token repeatedly; should not hit name-based slow path
    const int kN = 200;
    for (int i = 0; i < kN; ++i) {
        int32_t res = -1;
        ASSERT_EQ(GE_InvokeByToken(dom, token, &res), GE_Result_Ok);
        ASSERT_EQ(res, 0);
    }

#if defined(_DEBUG)
    int32_t byTok = GE_Debug_GetScriptingCounter(1);
    int32_t nameSlow = GE_Debug_GetScriptingCounter(2);
    EXPECT_EQ(byTok, kN);
    EXPECT_EQ(nameSlow, 0);
#endif
}

