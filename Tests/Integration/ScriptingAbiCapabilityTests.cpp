#include "ManagedFixturePaths.h"
#include "gtest/gtest.h"
#include "Core/Engine.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/ScriptingABI.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

static std::vector<uint8_t> ReadAll2(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void SetHostSwitches(uint32_t switches)
{
    GameEngine::EngineCore::GetInstance().GetScriptManager().GetCLRHost().SetHostSwitches(switches);
}

// GE_DebugGetPathMask bits: the typed delegates bound for QueryExport and InvokeByToken,
// and the reflection wrappers bound for the same pair.
static constexpr int kTypedDelegatesBound = 0x01 | 0x02;
static constexpr int kReflectionWrappersBound = 0x04 | 0x08;

TEST(ScriptingAbi_Capability, HrmMissing_ReturnsNotFound)
{
    SetHostSwitches(GE_HostSwitch_ForceHrmCapabilityFailure);

    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll2(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    // The managed loader reports the missing capability as -3, which maps to NotFound with no fallback.
    GE_DomainHandle dom = 0ULL;
    GE_Result rc = GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom);
    EXPECT_EQ(rc, GE_Result_NotFound);

    SetHostSwitches(0);
}

TEST(ScriptingAbi_Capability, DisableDelegates_FallbackToReflection)
{
    SetHostSwitches(GE_HostSwitch_DisableHrmDelegates);

    std::filesystem::path dllPath = GameEngine::Tests::DomainRoutingTestDll();
    ASSERT_TRUE(std::filesystem::exists(dllPath));
    auto bytes = ReadAll2(dllPath);
    GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();

    GE_DomainHandle dom = 0ULL;
    ASSERT_EQ(GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom), GE_Result_Ok);
    ASSERT_NE(dom, 0ull);

    const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
    uint64_t token = 0ull;
    ASSERT_EQ(GE_QueryExport(dom, fq, (uint32_t)strlen(fq), &token), GE_Result_Ok);
    ASSERT_NE(token, 0ull);

    int32_t result = -777;
    ASSERT_EQ(GE_InvokeByToken(dom, token, &result), GE_Result_Ok);
    EXPECT_EQ(result, 0);

    // The switch, not a delegate that failed to bind, is what put both calls on the reflection wrappers.
    int mask = GE_DebugGetPathMask();
    ASSERT_GE(mask, 0);
    EXPECT_EQ(mask & kTypedDelegatesBound, 0) << "typed delegate bound while delegates are disabled, mask=" << mask;
    EXPECT_EQ(mask & kReflectionWrappersBound, kReflectionWrappersBound) << "reflection wrappers not bound, mask=" << mask;

    // Clearing the switch rebinds the typed delegates on the next use.
    SetHostSwitches(0);
    mask = GE_DebugGetPathMask();
    EXPECT_EQ(mask & kTypedDelegatesBound, kTypedDelegatesBound) << "typed delegates not rebound, mask=" << mask;
    EXPECT_EQ(mask & kReflectionWrappersBound, 0) << "reflection wrappers still bound, mask=" << mask;
}
