#include "PlatformServices/PlatformProviderV1.h"
#include "PlatformServices/PlatformServices.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace PS = GameEngine::PlatformServices;

namespace
{

// Keyed by process id so parallel test runs on one machine don't share a
// save root.
std::filesystem::path TestSaveRoot()
{
#ifdef _WIN32
    const int processId = _getpid();
#else
    const int processId = getpid();
#endif
    return std::filesystem::temp_directory_path() / "GEPlatformServicesTests" /
           std::to_string(processId);
}

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> values)
{
    return std::vector<uint8_t>(values);
}

// The facade is a process-wide singleton, so every test goes through this
// fixture to guarantee a clean Initialize/Shutdown bracket and an empty
// save root.
class PlatformServicesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        PS::Get().Shutdown();
        std::filesystem::remove_all(TestSaveRoot());

        PS::Config config;
        config.AppName = "PlatformServicesTests";
        config.SaveRootOverride = TestSaveRoot();
        config.MockEntitlements = {"base-game", "dlc-1"};
        ASSERT_EQ(PS::Get().Initialize(config), PS::Result::Ok);
    }

    void TearDown() override
    {
        PS::Get().Shutdown();
        std::filesystem::remove_all(TestSaveRoot());
    }
};

TEST(PlatformServicesLifecycle, CallsBeforeInitializeReturnNotInitialized)
{
    PS::Get().Shutdown();
    std::vector<std::string> slots;
    EXPECT_EQ(PS::Get().ListSlots(slots), PS::Result::NotInitialized);
    PS::Account account;
    EXPECT_EQ(PS::Get().GetPrimaryAccount(account), PS::Result::NotInitialized);
    EXPECT_EQ(PS::Get().GetCapabilities(), 0u);
    EXPECT_FALSE(PS::Get().IsInitialized());
}

TEST(PlatformServicesLifecycle, InitializeRequiresAppNameOrSaveRoot)
{
    PS::Get().Shutdown();
    EXPECT_EQ(PS::Get().Initialize(PS::Config{}), PS::Result::InvalidArgument);
    EXPECT_FALSE(PS::Get().IsInitialized());
}

TEST_F(PlatformServicesTest, DoubleInitializeReturnsAlreadyInitialized)
{
    PS::Config config;
    config.AppName = "PlatformServicesTests";
    EXPECT_EQ(PS::Get().Initialize(config), PS::Result::AlreadyInitialized);
}

TEST_F(PlatformServicesTest, ReportsLocalProvider)
{
    const PS::ProviderInfo info = PS::Get().GetProviderInfo();
    EXPECT_EQ(info.Name, "Local");
    EXPECT_FALSE(info.Version.empty());
}

TEST_F(PlatformServicesTest, CapabilitiesMatchLocalProvider)
{
    EXPECT_TRUE(PS::Get().Supports(PS::Capability::Accounts));
    EXPECT_TRUE(PS::Get().Supports(PS::Capability::LocalSaves));
    EXPECT_TRUE(PS::Get().Supports(PS::Capability::Achievements));
    EXPECT_TRUE(PS::Get().Supports(PS::Capability::Entitlements));
    EXPECT_TRUE(PS::Get().Supports(PS::Capability::OfflineMode));
    EXPECT_FALSE(PS::Get().Supports(PS::Capability::CloudSaves));
    EXPECT_FALSE(PS::Get().Supports(PS::Capability::PlatformUI));
}

// Game code must branch from capabilities, not provider identity: this is
// the capability-driven path a CloudSaves-less build takes.
TEST_F(PlatformServicesTest, CapabilityBranchSkipsCloudSync)
{
    ASSERT_EQ(PS::Get().WriteFile("slot0", "state.bin", Bytes({1})), PS::Result::Ok);
    if (PS::Get().Supports(PS::Capability::CloudSaves))
    {
        FAIL() << "Local provider must not report CloudSaves";
    }
    // Commit is still valid without cloud saves; the provider decides it is
    // a local flush / no-op.
    EXPECT_EQ(PS::Get().Commit("slot0"), PS::Result::Ok);
}

TEST_F(PlatformServicesTest, PrimaryAccountIsOfflineGuest)
{
    PS::Account account;
    ASSERT_EQ(PS::Get().GetPrimaryAccount(account), PS::Result::Ok);
    EXPECT_FALSE(account.Id.empty());
    EXPECT_FALSE(account.DisplayName.empty());
    EXPECT_TRUE(account.SignedIn);
    EXPECT_TRUE(account.GuestOrOffline);
}

TEST_F(PlatformServicesTest, SignOutAndSignInToggleAccountState)
{
    ASSERT_EQ(PS::Get().RequestSignOut(), PS::Result::Ok);
    PS::Account account;
    ASSERT_EQ(PS::Get().GetPrimaryAccount(account), PS::Result::Ok);
    EXPECT_FALSE(account.SignedIn);

    ASSERT_EQ(PS::Get().RequestSignIn(), PS::Result::Ok);
    ASSERT_EQ(PS::Get().GetPrimaryAccount(account), PS::Result::Ok);
    EXPECT_TRUE(account.SignedIn);
}

TEST_F(PlatformServicesTest, WriteReadRoundTripPreservesBytes)
{
    const std::vector<uint8_t> payload = Bytes({0x00, 0xFF, 0x42, 0x00, 0x7F});
    ASSERT_EQ(PS::Get().WriteFile("slot1", "save.bin", payload), PS::Result::Ok);

    std::vector<uint8_t> readBack;
    ASSERT_EQ(PS::Get().ReadFile("slot1", "save.bin", readBack), PS::Result::Ok);
    EXPECT_EQ(readBack, payload);
}

TEST_F(PlatformServicesTest, OverwriteReplacesContent)
{
    ASSERT_EQ(PS::Get().WriteFile("slot1", "save.bin", Bytes({1, 2, 3})), PS::Result::Ok);
    ASSERT_EQ(PS::Get().WriteFile("slot1", "save.bin", Bytes({9})), PS::Result::Ok);

    std::vector<uint8_t> readBack;
    ASSERT_EQ(PS::Get().ReadFile("slot1", "save.bin", readBack), PS::Result::Ok);
    EXPECT_EQ(readBack, Bytes({9}));
}

TEST_F(PlatformServicesTest, EmptyFileRoundTrips)
{
    ASSERT_EQ(PS::Get().WriteFile("slot1", "empty.bin", {}), PS::Result::Ok);
    std::vector<uint8_t> readBack = Bytes({1});
    ASSERT_EQ(PS::Get().ReadFile("slot1", "empty.bin", readBack), PS::Result::Ok);
    EXPECT_TRUE(readBack.empty());
}

TEST_F(PlatformServicesTest, SlotsAreCreatedImplicitlyAndListed)
{
    std::vector<std::string> slots;
    ASSERT_EQ(PS::Get().ListSlots(slots), PS::Result::Ok);
    EXPECT_TRUE(slots.empty());

    ASSERT_EQ(PS::Get().WriteFile("alpha", "a.bin", Bytes({1})), PS::Result::Ok);
    ASSERT_EQ(PS::Get().WriteFile("beta", "b.bin", Bytes({2})), PS::Result::Ok);

    ASSERT_EQ(PS::Get().ListSlots(slots), PS::Result::Ok);
    ASSERT_EQ(slots.size(), 2u);
    EXPECT_NE(std::find(slots.begin(), slots.end(), "alpha"), slots.end());
    EXPECT_NE(std::find(slots.begin(), slots.end(), "beta"), slots.end());
}

TEST_F(PlatformServicesTest, ListFilesReturnsSlotContents)
{
    ASSERT_EQ(PS::Get().WriteFile("slot1", "a.bin", Bytes({1})), PS::Result::Ok);
    ASSERT_EQ(PS::Get().WriteFile("slot1", "b.bin", Bytes({2})), PS::Result::Ok);

    std::vector<std::string> files;
    ASSERT_EQ(PS::Get().ListFiles("slot1", files), PS::Result::Ok);
    ASSERT_EQ(files.size(), 2u);

    EXPECT_EQ(PS::Get().ListFiles("missing-slot", files), PS::Result::NotFound);
}

TEST_F(PlatformServicesTest, DeleteRemovesFile)
{
    ASSERT_EQ(PS::Get().WriteFile("slot1", "save.bin", Bytes({1})), PS::Result::Ok);
    ASSERT_EQ(PS::Get().DeleteFile("slot1", "save.bin"), PS::Result::Ok);

    std::vector<uint8_t> readBack;
    EXPECT_EQ(PS::Get().ReadFile("slot1", "save.bin", readBack), PS::Result::NotFound);
    EXPECT_EQ(PS::Get().DeleteFile("slot1", "save.bin"), PS::Result::NotFound);
}

TEST_F(PlatformServicesTest, SavesPersistAcrossReinitialize)
{
    const std::vector<uint8_t> payload = Bytes({0xDE, 0xAD, 0xBE, 0xEF});
    ASSERT_EQ(PS::Get().WriteFile("slot1", "persist.bin", payload), PS::Result::Ok);
    ASSERT_EQ(PS::Get().Commit("slot1"), PS::Result::Ok);

    PS::Get().Shutdown();
    PS::Config config;
    config.AppName = "PlatformServicesTests";
    config.SaveRootOverride = TestSaveRoot();
    ASSERT_EQ(PS::Get().Initialize(config), PS::Result::Ok);

    std::vector<uint8_t> readBack;
    ASSERT_EQ(PS::Get().ReadFile("slot1", "persist.bin", readBack), PS::Result::Ok);
    EXPECT_EQ(readBack, payload);
}

TEST_F(PlatformServicesTest, InvalidNamesAreRejected)
{
    const std::vector<uint8_t> payload = Bytes({1});
    const char* invalidNames[] = {"", "a/b", "a\\b", "..", ".hidden", "name with spaces", "a:b"};
    for (const char* name : invalidNames)
    {
        EXPECT_EQ(PS::Get().WriteFile("slot1", name, payload), PS::Result::InvalidArgument)
            << "file name: '" << name << "'";
        EXPECT_EQ(PS::Get().WriteFile(name, "file.bin", payload), PS::Result::InvalidArgument)
            << "slot name: '" << name << "'";
    }
}

TEST_F(PlatformServicesTest, FileAtSizeLimitWritesAndOversizeIsRejected)
{
    const std::vector<uint8_t> atLimit(PS::Limits::kMaxSaveFileBytes, 0xAB);
    ASSERT_EQ(PS::Get().WriteFile("slot1", "max.bin", atLimit), PS::Result::Ok);
    std::vector<uint8_t> readBack;
    ASSERT_EQ(PS::Get().ReadFile("slot1", "max.bin", readBack), PS::Result::Ok);
    EXPECT_EQ(readBack.size(), PS::Limits::kMaxSaveFileBytes);

    const std::vector<uint8_t> overLimit(PS::Limits::kMaxSaveFileBytes + 1, 0xAB);
    EXPECT_EQ(PS::Get().WriteFile("slot1", "over.bin", overLimit), PS::Result::FileTooLarge);
    // Nothing should have been created for the rejected write.
    EXPECT_EQ(PS::Get().ReadFile("slot1", "over.bin", readBack), PS::Result::NotFound);
}

TEST_F(PlatformServicesTest, NamesAtLimitAreAcceptedAndOverLimitRejected)
{
    const std::string maxSlot(PS::Limits::kMaxSlotNameLength, 'a');
    const std::string maxFile(PS::Limits::kMaxFileNameLength, 'b');
    const std::vector<uint8_t> payload = Bytes({1});
    EXPECT_EQ(PS::Get().WriteFile(maxSlot, maxFile, payload), PS::Result::Ok);

    const std::string longSlot(PS::Limits::kMaxSlotNameLength + 1, 'a');
    const std::string longFile(PS::Limits::kMaxFileNameLength + 1, 'b');
    EXPECT_EQ(PS::Get().WriteFile(longSlot, "ok.bin", payload), PS::Result::InvalidArgument);
    EXPECT_EQ(PS::Get().WriteFile("slot1", longFile, payload), PS::Result::InvalidArgument);
}

TEST_F(PlatformServicesTest, UnlockAchievementSetsState)
{
    PS::AchievementState state;
    ASSERT_EQ(PS::Get().GetAchievementState("ach-first", state), PS::Result::Ok);
    EXPECT_FALSE(state.Unlocked);

    ASSERT_EQ(PS::Get().UnlockAchievement("ach-first"), PS::Result::Ok);
    ASSERT_EQ(PS::Get().GetAchievementState("ach-first", state), PS::Result::Ok);
    EXPECT_TRUE(state.Unlocked);
}

TEST_F(PlatformServicesTest, AchievementProgressAutoUnlocksAtTarget)
{
    ASSERT_EQ(PS::Get().SetAchievementProgress("ach-collect", 3, 10), PS::Result::Ok);
    PS::AchievementState state;
    ASSERT_EQ(PS::Get().GetAchievementState("ach-collect", state), PS::Result::Ok);
    EXPECT_FALSE(state.Unlocked);
    EXPECT_EQ(state.Current, 3u);
    EXPECT_EQ(state.Target, 10u);

    ASSERT_EQ(PS::Get().SetAchievementProgress("ach-collect", 10, 10), PS::Result::Ok);
    ASSERT_EQ(PS::Get().GetAchievementState("ach-collect", state), PS::Result::Ok);
    EXPECT_TRUE(state.Unlocked);
}

TEST_F(PlatformServicesTest, AchievementsPersistAcrossReinitialize)
{
    ASSERT_EQ(PS::Get().SetAchievementProgress("ach-collect", 7, 10), PS::Result::Ok);
    ASSERT_EQ(PS::Get().UnlockAchievement("ach-first"), PS::Result::Ok);

    PS::Get().Shutdown();
    PS::Config config;
    config.AppName = "PlatformServicesTests";
    config.SaveRootOverride = TestSaveRoot();
    ASSERT_EQ(PS::Get().Initialize(config), PS::Result::Ok);

    PS::AchievementState state;
    ASSERT_EQ(PS::Get().GetAchievementState("ach-collect", state), PS::Result::Ok);
    EXPECT_FALSE(state.Unlocked);
    EXPECT_EQ(state.Current, 7u);
    EXPECT_EQ(state.Target, 10u);
    ASSERT_EQ(PS::Get().GetAchievementState("ach-first", state), PS::Result::Ok);
    EXPECT_TRUE(state.Unlocked);
}

TEST_F(PlatformServicesTest, CommitRejectsInvalidSlotName)
{
    EXPECT_EQ(PS::Get().Commit("a/b"), PS::Result::InvalidArgument);
    EXPECT_EQ(PS::Get().Commit(""), PS::Result::InvalidArgument);
}

TEST_F(PlatformServicesTest, EntitlementsComeFromConfig)
{
    bool has = false;
    ASSERT_EQ(PS::Get().HasEntitlement("base-game", has), PS::Result::Ok);
    EXPECT_TRUE(has);
    ASSERT_EQ(PS::Get().HasEntitlement("dlc-99", has), PS::Result::Ok);
    EXPECT_FALSE(has);

    std::vector<std::string> entitlements;
    ASSERT_EQ(PS::Get().ListEntitlements(entitlements), PS::Result::Ok);
    EXPECT_EQ(entitlements, (std::vector<std::string>{"base-game", "dlc-1"}));
}

TEST_F(PlatformServicesTest, PlatformUIIsUnsupported)
{
    EXPECT_EQ(PS::Get().ShowAchievements(), PS::Result::Unsupported);
    EXPECT_EQ(PS::Get().ShowAccountPicker(), PS::Result::Unsupported);
    EXPECT_EQ(PS::Get().ShowStorePage("product-1"), PS::Result::Unsupported);
}

// ABI-level contract checks against the linked provider table.
TEST(PlatformProviderAbi, TableReportsCompatibleVersionAndSize)
{
    const GE_PlatformProviderV1* table = GE_GetPlatformProviderV1();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->abiMajor, 1u);
    EXPECT_GE(table->sizeBytes, sizeof(GE_PlatformProviderV1));
}

TEST(PlatformProviderAbi, RequiredFunctionsAreNonNull)
{
    const GE_PlatformProviderV1* table = GE_GetPlatformProviderV1();
    ASSERT_NE(table, nullptr);
    EXPECT_NE(table->Initialize, nullptr);
    EXPECT_NE(table->Shutdown, nullptr);
    EXPECT_NE(table->Tick, nullptr);
    EXPECT_NE(table->GetProviderInfo, nullptr);
    EXPECT_NE(table->GetCapabilities, nullptr);
}

TEST(PlatformProviderAbi, OptionalUiFunctionsMayBeNullAndMapToUnsupported)
{
    const GE_PlatformProviderV1* table = GE_GetPlatformProviderV1();
    ASSERT_NE(table, nullptr);
    // Local ships no native UI; the facade must map these to Unsupported
    // (verified end-to-end by PlatformServicesTest.PlatformUIIsUnsupported).
    EXPECT_EQ(table->ShowAchievements, nullptr);
    EXPECT_EQ(table->ShowAccountPicker, nullptr);
    EXPECT_EQ(table->ShowStorePage, nullptr);
}

} // namespace
