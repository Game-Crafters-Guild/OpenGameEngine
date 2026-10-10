#include "Terrain/TerrainMaterialLibraryMint.h"

#include "Assets/AssetCreation.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "TerrainECS/TerrainMaterialAuthoring.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kLibraryExtension = ".terrainmatlib";

// Holds one undo compound open for a scope, so every early return closes it. A compound left on
// the service's stack silently swallows every later command in the session.
class CompoundScope
{
public:
    CompoundScope(UndoRedoService* undo, std::string name) : m_Undo(undo)
    {
        if (m_Undo)
            m_Undo->BeginCompound(std::move(name));
    }
    ~CompoundScope()
    {
        if (m_Undo)
            m_Undo->EndCompound();
    }
    CompoundScope(const CompoundScope&) = delete;
    CompoundScope& operator=(const CompoundScope&) = delete;

private:
    UndoRedoService* m_Undo = nullptr;
};

// Binds a terrain to a library, reversibly. Deliberately narrower than a whole-component
// snapshot: it restores the one field it set, so an undo cannot roll back terrain edits the user
// made after the mint.
class BindMaterialLibraryCommand final : public IEditorCommand
{
public:
    BindMaterialLibraryCommand(ECS::World& world, ECS::EntityHandle entity, const GUID& guid)
        : m_World(world), m_Entity(entity), m_Guid(guid)
    {
    }

    const char* GetName() const override { return "Bind Terrain Material Library"; }
    const char* GetTypeName() const override { return "BindTerrainMaterialLibrary"; }

    void Do() override { Apply(m_Guid); }
    void Undo() override { Apply(GUID{}); }

private:
    void Apply(const GUID& guid)
    {
        if (!m_Entity.IsValid() || !m_World.IsValid(m_Entity))
            return;
        const auto* current = m_World.GetComponent<Components::Terrain>(m_Entity);
        if (!current)
            return;
        Components::Terrain updated = *current;
        if (guid.IsNull())
            updated.MaterialLibraryGuid.Clear();
        else
            updated.MaterialLibraryGuid.Set(guid);
        m_World.AddComponentImmediate(m_Entity, updated);
    }

    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    GUID m_Guid;
};

// Absolute, lexically normal, generic-separator form, case-folded on the case-insensitive
// filesystems. This is the registry's own key rule (NormalizePathForMap): a comparison that does
// NOT fold would reject a genuinely-contained directory on Windows, because the registry stores
// its source root already folded while callers pass authored casing.
std::string PathKeyForContainment(const std::filesystem::path& p)
{
    std::error_code ec;
    const std::filesystem::path abs =
        p.is_absolute() ? p.lexically_normal() : std::filesystem::absolute(p, ec).lexically_normal();
    std::string s = abs.generic_string();
#if !defined(__linux__)
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

// Whether `directory` sits inside the project's asset root. Registration outside it produces a
// GUID::Generate() binding the store never persists, so the terrain would resolve its library for
// this session only and load unbound after a restart. Purely lexical: the target directory need
// not exist yet.
bool DirectoryIsInsideAssetRoot(const std::filesystem::path& assetRoot,
                                const std::filesystem::path& directory)
{
    if (assetRoot.empty() || directory.empty())
        return false;

    const std::string root = PathKeyForContainment(assetRoot);
    const std::string dir = PathKeyForContainment(directory);
    if (root.empty() || dir.empty())
        return false;
    if (dir == root)
        return true;
    // The trailing separator keeps "…/Assets" from matching a sibling "…/AssetsBackup".
    return dir.rfind(root + '/', 0) == 0;
}

// The library file's stem. Named after the scene rather than the entity so a project's material
// lists are findable by the scene that uses them; MakeUniqueFilePath disambiguates a second
// terrain in the same scene.
std::string LibraryStemFor(const std::filesystem::path& scenePath)
{
    const std::string stem = scenePath.stem().string();
    return (stem.empty() ? std::string("Terrain") : stem) + " Materials";
}

} // namespace

bool MintTerrainMaterialLibraryFor(ECS::World& world, ECS::EntityHandle entity,
                                   const std::filesystem::path& directory,
                                   const std::string& baseName, AssetManager& assets,
                                   UndoRedoService* undo)
{
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    if (!terrain || !terrain->MaterialLibraryGuid.IsNull())
        return false;

    const std::vector<TerrainMaterialEntry> entries =
        TerrainECS::MintTerrainMaterialLibrary(*terrain);
    if (entries.size() != Terrain::kTerrainLayerRoleCount)
        return false;

    // Refused BEFORE the write, not after: a file outside the asset root registers with a freshly
    // generated GUID the store never persists, so the terrain would bind a reference that is dead
    // the next time the project opens — and the file would still be sitting there.
    const std::filesystem::path assetRoot = assets.GetAssetRoot();
    if (!DirectoryIsInsideAssetRoot(assetRoot, directory))
    {
        Logger::Log::Warning("Terrain materials: refusing to mint into '{}' — it is outside the "
                             "project's asset root '{}', so the binding would not survive a "
                             "restart. Save the scene inside the asset root, or create the library "
                             "there and assign it in the inspector.",
                             directory.string(), assetRoot.string());
        return false;
    }

    // File and binding are ONE undo entry from here down. Separately they tear apart: the file
    // create is undoable and the bind was not, so a Ctrl+Z deleted the library and left the
    // terrain pointing at a path that no longer exists. Opened after the last refusal so no
    // early return can leave a compound open on the service's stack.
    const CompoundScope compound(undo, "Create Terrain Material Library");

    const AssetFileResult created =
        CreateAssetFile(directory, baseName, kLibraryExtension,
                        [&entries](const std::string&)
                        { return TerrainMaterialLibraryAsset::MakeDocumentText(entries); },
                        "Create Terrain Material Library", undo, &assets);
    if (created.path.empty())
        return false;

    // Resolve through AssetManager, which registers the just-written file when nothing has
    // yet: a raw registry lookup finds nothing for an unregistered path, and the library
    // would never bind.
    const GUID guid = assets.ResolveAssetGuid(created.path);
    if (guid.IsNull())
    {
        Logger::Log::Warning("Terrain materials: {} was written but did not register — the terrain "
                             "keeps shading from its per-layer fields",
                             created.path.filename().string());
        return false;
    }

    // Never bind a library the runtime cannot read back. The file is re-parsed here rather than
    // trusted, because binding a half-written or unparseable one would drop the terrain to the
    // built-in materials and lose the per-layer authoring this mint exists to preserve.
    {
        TerrainMaterialLibraryAsset probe(guid, created.path);
        if (!probe.Load() ||
            probe.GetMaterials().size() != Terrain::kTerrainLayerRoleCount)
        {
            Logger::Log::Warning("Terrain materials: {} did not read back as {} materials — not "
                                 "binding it; the terrain keeps its per-layer fields",
                                 created.path.filename().string(),
                                 Terrain::kTerrainLayerRoleCount);
            return false;
        }
    }

    // The mint hands role r slot r, which is the component's identity default — so the migrated
    // terrain resolves the same four materials it shaded with a moment ago.
    if (undo)
        undo->Execute(std::make_unique<BindMaterialLibraryCommand>(world, entity, guid));
    else
        BindMaterialLibraryCommand(world, entity, guid).Do();

    Logger::Log::Info("Terrain materials: minted {} from the terrain's per-layer fields",
                      created.path.filename().string());
    return true;
}

std::uint32_t MintMissingTerrainMaterialLibraries(ECS::World& world,
                                                  const std::filesystem::path& scenePath,
                                                  AssetManager& assets)
{
    if (scenePath.empty())
        return 0u;

    // Collected first, then minted: the mint writes the component, and mutating inside a query is
    // exactly the shape that invalidates the iteration it is running under.
    std::vector<ECS::EntityHandle> unmigrated;
    world.Query<ECS::Read<Components::Terrain>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain)
        {
            if (terrain.MaterialLibraryGuid.IsNull())
                unmigrated.push_back(entity);
        });

    const std::filesystem::path directory = scenePath.parent_path();
    const std::string baseName = LibraryStemFor(scenePath);
    std::uint32_t minted = 0u;
    for (ECS::EntityHandle entity : unmigrated)
    {
        // No undo entry: a save is not a user edit, and an undo that deleted the file would leave
        // the just-saved scene referencing a library that is gone.
        if (MintTerrainMaterialLibraryFor(world, entity, directory, baseName, assets,
                                          /*undo*/ nullptr))
            ++minted;
    }
    return minted;
}

} // namespace GameEngine::Editor
