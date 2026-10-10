#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>

static std::vector<uint8_t> ReadAllNamePerf(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(ScriptingAbi_Perf, InvokeByNameThroughput_DebugOnly)
{
#ifndef NDEBUG
    // Prepare assembly bytes
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllNamePerf(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.GetValue";
    const int kIterations = 10000; // lower than token path to keep CI fast
    auto start = std::chrono::high_resolution_clock::now();
    int32_t last = 0;
    for (int i = 0; i < kIterations; ++i) {
        ASSERT_EQ(GE_Invoke(dom, fq, (uint32_t)strlen(fq), &last), GE_Result_Ok);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    SUCCEED() << "InvokeByNameThroughput ran " << kIterations << " calls in " << ms << " ms";
#else
    GTEST_SKIP() << "Perf test is Debug-only";
#endif
}

