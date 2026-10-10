#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Scripting/ScriptingABI.h"
#include <filesystem>
#include <fstream>
#include <vector>

static std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST(ScriptingAbi_Additional, TokenInvalidAfterUnload)
{
    // Prepare assembly bytes for DomainRoutingTest
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &token), GE_Result_Ok);

    ASSERT_EQ(GE_ScriptsDomainUnload(dom), GE_Result_Ok);

    int32_t result = 1234;
    EXPECT_EQ(GE_InvokeByToken(dom, token, &result), GE_Result_Fail);
    EXPECT_EQ(result, 1234);
}

TEST(ScriptingAbi_Additional, TokenDomainIsolation)
{
    // Prepare assembly bytes for DomainRoutingTest
    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle domA = 0ULL, domB = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &domB), GE_Result_Ok);
    ASSERT_NE(domA, 0ull); ASSERT_NE(domB, 0ull); ASSERT_NE(domA, domB);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.Reset";

    uint64_t tokA = 0ull, tokB = 0ull;
    ASSERT_EQ(GE_QueryExport(domA, fq, (uint32_t)strlen(fq), &tokA), GE_Result_Ok);
    ASSERT_EQ(GE_QueryExport(domB, fq, (uint32_t)strlen(fq), &tokB), GE_Result_Ok);
    ASSERT_NE(tokA, 0ull); ASSERT_NE(tokB, 0ull);

    int32_t resA = -1, resB = -1;
    ASSERT_EQ(GE_ScriptsDomainSwap(domA), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(domA, tokA, &resA), GE_Result_Ok);
    ASSERT_EQ(GE_ScriptsDomainSwap(domB), GE_Result_Ok);
    ASSERT_EQ(GE_InvokeByToken(domB, tokB, &resB), GE_Result_Ok);
    EXPECT_EQ(resA, 0); EXPECT_EQ(resB, 0);
}

