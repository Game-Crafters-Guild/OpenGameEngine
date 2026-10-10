// The material-library mint: the migration that turns a terrain's per-layer fields into a
// .terrainmatlib and binds it. It runs unattended on every scene save, so the cases that matter
// are the ones nobody watches — the terrain that already has a library and must not gain a
// second, and the target directory the asset store cannot persist a GUID for.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Terrain/TerrainMaterialLibraryMint.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "UndoRedo/UndoRedoService.h"

#include "../TestTempDir.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace GameEngine;

namespace
{

// Counts .terrainmatlib files directly inside `dir`, so a test can assert that a refused or
// no-op mint left nothing behind rather than only that it returned false.
std::size_t CountLibraryFiles(const std::filesystem::path& dir)
{
    std::size_t count = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".terrainmatlib")
            ++count;
    }
    return count;
}

class TerrainMaterialLibraryMintTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("ge_terrain_matlib_mint");
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
        m_AssetsRoot = m_Root / "Assets";
        m_OutsideRoot = m_Root / "Outside";
        std::filesystem::create_directories(m_AssetsRoot, ec);
        std::filesystem::create_directories(m_OutsideRoot, ec);

        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_Assets = std::make_unique<AssetManager>();
        // Initialize takes the asset root itself, not the project directory above it.
        ASSERT_TRUE(m_Assets->Initialize(m_AssetsRoot, m_Pool.get()));

        // Instrument check: the refusal under test is "outside THIS root", so a fixture whose
        // asset root resolved somewhere else would make every refusal assertion pass for the
        // wrong reason, and every happy-path assertion fail for one. Compared with equivalent()
        // rather than == because the registry case-folds its source root on non-Linux
        // filesystems — the same fold the mint's containment test has to apply, and the reason it
        // cannot use a plain lexically_relative.
        ASSERT_TRUE(std::filesystem::equivalent(m_Assets->GetAssetRoot(), m_AssetsRoot, ec))
            << m_Assets->GetAssetRoot().string() << " vs " << m_AssetsRoot.string();

        m_Terrain = m_World.Create<Components::Terrain>(Components::Terrain{}).GetHandle();
        ASSERT_TRUE(m_Terrain.IsValid());
    }

    void TearDown() override
    {
        if (m_Assets)
        {
            m_Assets->Shutdown();
            m_Assets.reset();
        }
        m_Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    const Components::Terrain& LiveTerrain()
    {
        const auto* t = m_World.GetComponent<Components::Terrain>(m_Terrain);
        EXPECT_NE(t, nullptr);
        return *t;
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_AssetsRoot;
    std::filesystem::path m_OutsideRoot;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    std::unique_ptr<AssetManager> m_Assets;
    ECS::World m_World;
    ECS::EntityHandle m_Terrain;
};

// The migration in full: a file appears, it re-parses as the four role materials, and the terrain
// comes out bound to it. Anything less and the terrain silently keeps shading from the fields the
// library was supposed to replace.
TEST_F(TerrainMaterialLibraryMintTest, MintWritesAReadableLibraryAndBindsIt)
{
    ASSERT_TRUE(LiveTerrain().MaterialLibraryGuid.IsNull());

    ASSERT_TRUE(Editor::MintTerrainMaterialLibraryFor(m_World, m_Terrain, m_AssetsRoot,
                                                      "Island Materials", *m_Assets,
                                                      /*undo*/ nullptr));

    const std::filesystem::path written = m_AssetsRoot / "Island Materials.terrainmatlib";
    ASSERT_TRUE(std::filesystem::exists(written));

    const GUID bound = LiveTerrain().MaterialLibraryGuid.ToGuid();
    ASSERT_FALSE(bound.IsNull()) << "file written but the terrain was left unbound";
    EXPECT_EQ(bound, m_Assets->GetRegistry().GetAssetGUID(written));

    TerrainMaterialLibraryAsset readBack(bound, written);
    ASSERT_TRUE(readBack.Load());
    ASSERT_EQ(readBack.GetMaterials().size(), Terrain::kTerrainLayerRoleCount);
    // Role r takes slot r, which is the component's identity default — that equality is what
    // makes the migration invisible on screen.
    for (std::uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
        EXPECT_EQ(readBack.GetMaterials()[role].SlotId, role);
}

// A directory outside the asset root registers with a freshly generated GUID that the store never
// persists, so the binding dies at restart. The refusal must land BEFORE the write: a file left
// on disk next to a terrain that is not bound to it is the confusing half of the failure.
TEST_F(TerrainMaterialLibraryMintTest, MintRefusesADirectoryOutsideTheAssetRoot)
{
    EXPECT_FALSE(Editor::MintTerrainMaterialLibraryFor(m_World, m_Terrain, m_OutsideRoot,
                                                       "Island Materials", *m_Assets,
                                                       /*undo*/ nullptr));

    EXPECT_EQ(CountLibraryFiles(m_OutsideRoot), 0u) << "refused after writing the file";
    EXPECT_TRUE(LiveTerrain().MaterialLibraryGuid.IsNull());
}

// Null means "not migrated yet", so a terrain that already binds a library is not a mint
// candidate. Without this the save path would mint a second library on every save.
TEST_F(TerrainMaterialLibraryMintTest, MintLeavesAnAlreadyBoundTerrainUntouched)
{
    ASSERT_TRUE(Editor::MintTerrainMaterialLibraryFor(m_World, m_Terrain, m_AssetsRoot,
                                                      "Island Materials", *m_Assets,
                                                      /*undo*/ nullptr));
    const GUID firstBinding = LiveTerrain().MaterialLibraryGuid.ToGuid();
    ASSERT_FALSE(firstBinding.IsNull());
    ASSERT_EQ(CountLibraryFiles(m_AssetsRoot), 1u);

    EXPECT_FALSE(Editor::MintTerrainMaterialLibraryFor(m_World, m_Terrain, m_AssetsRoot,
                                                       "Island Materials", *m_Assets,
                                                       /*undo*/ nullptr));

    EXPECT_EQ(LiveTerrain().MaterialLibraryGuid.ToGuid(), firstBinding);
    EXPECT_EQ(CountLibraryFiles(m_AssetsRoot), 1u) << "a second library appeared on re-mint";
}

// The save-path entry point over a whole world: it migrates what is unmigrated, counts only what
// it migrated, and does not touch a terrain that already has a library.
TEST_F(TerrainMaterialLibraryMintTest, MintMissingMigratesOnlyTheUnboundTerrains)
{
    const ECS::EntityHandle second =
        m_World.Create<Components::Terrain>(Components::Terrain{}).GetHandle();
    ASSERT_TRUE(second.IsValid());

    const std::filesystem::path scenePath = m_AssetsRoot / "Island.scene";
    EXPECT_EQ(Editor::MintMissingTerrainMaterialLibraries(m_World, scenePath, *m_Assets), 2u);

    const GUID firstBinding = LiveTerrain().MaterialLibraryGuid.ToGuid();
    const auto* secondTerrain = m_World.GetComponent<Components::Terrain>(second);
    ASSERT_NE(secondTerrain, nullptr);
    EXPECT_FALSE(firstBinding.IsNull());
    EXPECT_FALSE(secondTerrain->MaterialLibraryGuid.ToGuid().IsNull());
    // Two terrains in one scene get two files, not one shared by accident.
    EXPECT_NE(firstBinding, secondTerrain->MaterialLibraryGuid.ToGuid());
    EXPECT_EQ(CountLibraryFiles(m_AssetsRoot), 2u);

    // Everything is migrated now, so a second save mints nothing and writes nothing.
    EXPECT_EQ(Editor::MintMissingTerrainMaterialLibraries(m_World, scenePath, *m_Assets), 0u);
    EXPECT_EQ(CountLibraryFiles(m_AssetsRoot), 2u);
}

// File and binding undo together or not at all. They were separate entries: the file create was
// undoable and the bind was a bare component write, so one Ctrl+Z deleted the library and left
// the terrain bound to a path that no longer existed.
TEST_F(TerrainMaterialLibraryMintTest, UndoRestoresTheFileAndTheBindingTogether)
{
    Editor::UndoRedoService undo;

    ASSERT_TRUE(Editor::MintTerrainMaterialLibraryFor(m_World, m_Terrain, m_AssetsRoot,
                                                      "Island Materials", *m_Assets, &undo));
    const std::filesystem::path written = m_AssetsRoot / "Island Materials.terrainmatlib";
    ASSERT_TRUE(std::filesystem::exists(written));
    ASSERT_FALSE(LiveTerrain().MaterialLibraryGuid.IsNull());

    // One entry, not two: the whole mint is a single step in the user's history.
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    undo.Undo();
    EXPECT_FALSE(std::filesystem::exists(written)) << "undo kept the file";
    EXPECT_TRUE(LiveTerrain().MaterialLibraryGuid.IsNull())
        << "undo deleted the library but left the terrain bound to it";

    undo.Redo();
    EXPECT_TRUE(std::filesystem::exists(written));
    EXPECT_FALSE(LiveTerrain().MaterialLibraryGuid.IsNull());
}

// The save path writes beside the scene, so a scene saved outside the asset root hits the same
// refusal — and must not scatter libraries next to it.
TEST_F(TerrainMaterialLibraryMintTest, MintMissingRefusesASceneOutsideTheAssetRoot)
{
    const std::filesystem::path scenePath = m_OutsideRoot / "Island.scene";
    EXPECT_EQ(Editor::MintMissingTerrainMaterialLibraries(m_World, scenePath, *m_Assets), 0u);

    EXPECT_EQ(CountLibraryFiles(m_OutsideRoot), 0u);
    EXPECT_TRUE(LiveTerrain().MaterialLibraryGuid.IsNull());
}

} // namespace
