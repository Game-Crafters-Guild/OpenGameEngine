// Registers the SQLite-backed AssetDbCache factory for the AssetSystemTests binary,
// the same way the editor does in Apps/Editor/Source/main.cpp.
//
// AssetRegistry now creates its derived cache through AssetRegistry::SetAssetDbCacheFactory
// so the shared Engine.dll and the Player ship no SQL. The factory is per-binary, so a test
// executable that doesn't register one gets a null cache from AssetRegistry::Initialize —
// which makes the registry-backed hardening tests (e.g. AssetDbHardeningTests, which assert
// on pinned->Cache) fail. A GTest global environment registers it once before any test runs,
// restoring the SQLite-backed behavior these tests intend to exercise.
#include "Assets/AssetRegistry.h"
#include "AssetDatabase/AssetDbCache_Sqlite.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{

struct AssetDbCacheFactoryEnvironment : ::testing::Environment
{
    void SetUp() override
    {
        GameEngine::AssetRegistry::SetAssetDbCacheFactory(
            [] { return std::make_unique<GameEngine::AssetDatabase::AssetDbCache_Sqlite>(); });
    }
};

// GTest takes ownership and runs SetUp() before RUN_ALL_TESTS().
[[maybe_unused]] const ::testing::Environment* const g_assetDbCacheFactoryEnv =
    ::testing::AddGlobalTestEnvironment(new AssetDbCacheFactoryEnvironment());

} // namespace
