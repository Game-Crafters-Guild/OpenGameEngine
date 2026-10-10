#pragma once

#include "JobSystem/TaskHandle.h"
#include "UI/Controls/SearchDialog.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetRegistry;
namespace ECS
{
class World;
struct EntityHandle;
}

enum class UniversalSearchKind : std::uint8_t
{
    Command,
    Panel,
    Setting,
    Asset,
    Entity,
    File,
    Class,
    Function,
    Symbol,
    Comment,
};

struct UniversalSearchEntry
{
    UniversalSearchKind Kind = UniversalSearchKind::Command;
    std::string Label;
    std::string Detail;
    std::string Keywords;
    std::string Provider;
    SearchIcon Icon;
    int Priority = 0;
    std::function<void()> Execute;
};

/// Cached multi-domain provider used by the editor's global search/command palette.
/// Querying never touches the filesystem: source indexing and lexical searches run as
/// job-system tasks, while assets/entities are snapshotted when the palette is opened.
class UniversalSearchProvider final : public ISearchProvider
{
  public:
    explicit UniversalSearchProvider(JobSystem::WorkStealingThreadPool& jobSystem);
    ~UniversalSearchProvider() override;

    void AddEntry(UniversalSearchEntry entry);
    void SetScriptEntries(std::vector<UniversalSearchEntry> entries);
    void SetSettingsEntries(std::vector<UniversalSearchEntry> entries);

    void SetAssetRegistry(AssetRegistry* registry) { m_AssetRegistry = registry; }
    void SetWorld(ECS::World* world) { m_World = world; }
    void SetOnAssetSelected(std::function<void(const std::filesystem::path&)> callback);
    void SetOnEntitySelected(std::function<void(ECS::EntityHandle)> callback);
    void SetOnCodeSelected(std::function<void(const std::filesystem::path&, std::size_t)> callback);

    /// Refresh the small live indexes. Call immediately before Show().
    void RefreshRuntimeEntries();

    /// Start the project code index as a background job if it isn't already
    /// built (or building) for this root.
    void StartCodeIndex(const std::filesystem::path& projectRoot);

    /// Discard any built or in-flight code index and rebuild from scratch.
    void RestartCodeIndex(const std::filesystem::path& projectRoot);

    void BeginSearch(const std::string& query, ResultSink sink) override;
    void CancelSearch() override;
    std::string GetPlaceholderText() const override;

  private:
    struct ParsedQuery
    {
        std::string Text;
        std::optional<UniversalSearchKind> Kind;
        bool CodeKindsOnly = false;
    };

    struct IndexedEntry
    {
        UniversalSearchEntry Entry;
        std::string NormalizedLabel;
        std::string NormalizedSearchText;
    };

    /// Immutable, shareable group of entries. Layers are published once and
    /// never mutated afterwards, so search snapshots can reference them from
    /// the lexical search job without copying.
    using EntryLayer = std::shared_ptr<const std::vector<IndexedEntry>>;

    struct SearchSnapshot;
    struct LexicalState;

    static std::string NormalizeForSearch(std::string_view text);
    static int FuzzyScore(std::string_view query, std::string_view candidate);
    static const char* KindName(UniversalSearchKind kind);
    static ParsedQuery ParseQuery(std::string_view query);
    static IndexedEntry MakeIndexed(UniversalSearchEntry entry);
    static std::vector<IndexedEntry> BuildCodeIndex(
        const std::filesystem::path& root,
        const std::function<void(const std::filesystem::path&, std::size_t)>& onCodeSelected,
        std::atomic<bool>* cancel);
    /// BuildCodeIndex as a published layer; logs and returns null if indexing throws.
    static EntryLayer BuildCodeIndexLayer(
        const std::filesystem::path& root,
        const std::function<void(const std::filesystem::path&, std::size_t)>& onCodeSelected,
        std::atomic<bool>* cancel);
    void PollCodeIndex();
    void RebuildAssetEntries();
    void RebuildEntityEntries();
    void RefreshSearchSnapshot();
    static void RunLexicalSearch(LexicalState& lexical,
                                 const std::string& query,
                                 const std::shared_ptr<const SearchSnapshot>& snapshot,
                                 ResultSink sink,
                                 std::uint64_t version);

    AssetRegistry* m_AssetRegistry = nullptr;
    ECS::World* m_World = nullptr;
    std::function<void(const std::filesystem::path&)> m_OnAssetSelected;
    std::function<void(ECS::EntityHandle)> m_OnEntitySelected;
    std::function<void(const std::filesystem::path&, std::size_t)> m_OnCodeSelected;

    std::vector<IndexedEntry> m_Registered;
    std::vector<IndexedEntry> m_Scripts;
    std::vector<IndexedEntry> m_Settings;
    EntryLayer m_Assets;
    EntryLayer m_Entities;
    EntryLayer m_Code;

    JobSystem::WorkStealingThreadPool& m_JobSystem;
    // Background job returning the finished code index as an EntryLayer.
    JobSystem::TaskHandle m_CodeTask;
    std::filesystem::path m_CodeRoot;
    std::filesystem::path m_QueuedCodeRoot;
    std::atomic<bool> m_CodeCancel{false};
    std::uint64_t m_SnapshotGeneration = 0;
    std::shared_ptr<const SearchSnapshot> m_SearchSnapshot;
    std::unique_ptr<LexicalState> m_Lexical;
};

} // namespace GameEngine
