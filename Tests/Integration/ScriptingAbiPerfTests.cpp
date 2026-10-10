#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>

static std::vector<uint8_t> ReadAllPerf(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Optional micro-benchmark: token invoke throughput
TEST(ScriptingAbi_Perf, TokenInvokeThroughput_DebugOnly)
{
#ifndef NDEBUG
    // Prepare assembly bytes
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAllPerf(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.GetValue";
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &token), GE_Result_Ok);

    const int kIterations = 50000; // keep modest to not slow CI
    auto start = std::chrono::high_resolution_clock::now();
    int32_t last = 0;
    for (int i = 0; i < kIterations; ++i) {
        ASSERT_EQ(GE_InvokeByToken(dom, token, &last), GE_Result_Ok);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    // Not asserting on time; just ensure it runs
    SUCCEED() << "TokenInvokeThroughput ran " << kIterations << " calls in " << ms << " ms";
#else
    GTEST_SKIP() << "Perf test is Debug-only";
#endif
}

