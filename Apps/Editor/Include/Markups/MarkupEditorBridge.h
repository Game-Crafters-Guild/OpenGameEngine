#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/ECS.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupRegionDisplayCache.h"
#include "Markups/MarkupHighlightState.h"
#include "Markups/MarkupRegionDisplay.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
class SceneViewController;
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// The editor side of world mark-ups: the one subscriber to the editor's change
// notifications for them, who stamps an edit to a mark-up with its author and time
// (MarkupECS::MarkupService::Touch), and the holder of what is the viewer's rather
// than the scene's: which mark-ups are hidden and which updates were seen.
//
// Attribution: an edit is the user's unless an attribution scope is open. The mark-ups'
// debug-request gate (MarkupRequestGate.h) opens an Agent scope around every request's
// synchronous handling (BeginAgentRequest/EndAgentRequest), so markup_*, set_component,
// undo and redo over the port stamp Agent while the same edits from the editor's own
// input stamp User. The input-injection methods (send_key, input_text, click_element,
// perform_drop and the rest the gate lists) run outside that scope, so what the injected
// input does counts as the user's.
//
// Touch rule: a Commit or UndoRedo change to Markup, MarkupVolume, Transform or Name
// of an entity that carries a Markup touches it; a Preview never does. A mark-up with
// no notes (a duplicate, whose component bytes were copied) gets its own notes on
// first sight: one Created entry by the current author.
//
// Edit rule: such a change that moved, resized or rotated the volume (its Transform),
// renamed it (Name), changed its shape (MarkupVolume) or its own color (Markup::Color) is
// recorded in its thread by the current author (MarkupService::RecordEdit, which joins
// close edits into one entry). What changed is read against the mark-up's state as of the
// last structure change or recorded edit, so a drag's Previews are one edit at its Commit;
// a change to a mark-up seen for the first time records nothing and becomes its baseline.
//
// Per project: the tag vocabulary's names and colors (the project's settings, shared),
// and the hidden set and seen state (the user's project settings). In a session the
// hidden and seen state belong to the entity's handle, so a save that gives a mark-up
// its scene tag and a duplicate that copies the original's tag change nothing. They are
// saved under the scene (its path in the project) and the scene tag, and read back per
// mark-up on first sight; a duplicate's copied tag is never read or written as its own.
// State the file cannot key yet (a mark-up before its first save, a scene never saved) is
// written when the save that gives it a key completes (OnSceneSaved). Main thread only.
class MarkupEditorBridge
{
public:
    // Unix seconds now; injectable so tests own the clock.
    using Clock = std::function<int64()>;
    // The open scene's file, or nullopt for a scene never saved.
    using ScenePath = std::function<std::optional<std::filesystem::path>()>;

    MarkupEditorBridge(EditorChangeNotifications& notifications, Clock clock, ScenePath scenePath);
    ~MarkupEditorBridge();

    MarkupEditorBridge(const MarkupEditorBridge&) = delete;
    MarkupEditorBridge& operator=(const MarkupEditorBridge&) = delete;

    // The bridge the editor installed, or null (tests and tools without an editor).
    static MarkupEditorBridge* TryGet();
    // Makes `bridge` (or null) the one TryGet returns.
    static void Install(MarkupEditorBridge* bridge);

    // Opens an attribution scope for its lifetime; scopes nest.
    class AttributionScope
    {
    public:
        AttributionScope(MarkupEditorBridge& bridge, Components::MarkupAuthor author);
        ~AttributionScope();
        AttributionScope(const AttributionScope&) = delete;
        AttributionScope& operator=(const AttributionScope&) = delete;

    private:
        MarkupEditorBridge& m_Bridge;
    };

    Components::MarkupAuthor CurrentAuthor() const;
    int64 Now() const { return m_Clock(); }

    // The mark-ups' debug-request gate: one Agent scope per request.
    void BeginAgentRequest();
    void EndAgentRequest();

    // --- Viewer state ---------------------------------------------------------------
    bool IsHidden(const ECS::World& world, ECS::EntityHandle entity) const;
    // A view state: no scene edit, no undo step. One write of the user's project
    // settings for the whole set.
    void SetHidden(const ECS::World& world, std::span<const ECS::EntityHandle> entities, bool hidden);

    // An update the viewer has not seen: the agent changed the mark-up after the
    // viewer's last look.
    bool HasUnseenUpdate(const ECS::World& world, ECS::EntityHandle entity) const;
    void MarkSeen(const ECS::World& world, ECS::EntityHandle entity);
    // The position in the mark-up's thread from which its agent entries are unseen: the
    // thread's length when it has no unseen update, the length at the last look in this
    // session, else the first entry stamped after the last look's time.
    std::size_t FirstUnseenEntry(const ECS::World& world, ECS::EntityHandle entity) const;
    // How many agent entries of `world`'s mark-ups are unseen (FirstUnseenEntry): the
    // Activity badge, the design's "activity entries newer than the viewer's last look".
    // Read every frame, so it is counted again only when the world's revision, its
    // structure or the viewer's seen state changed since the last count.
    std::size_t CountUnseen(const ECS::World& world) const;

    // Writes the viewer state the save of `world`'s scene has just given a key: a mark-up's
    // first scene tag, a scene's first path. The editor calls it after every scene save.
    void OnSceneSaved(const ECS::World& world);

    // --- Project state --------------------------------------------------------------
    // Reads the vocabulary, the hidden set and the seen state of the project at
    // `workspaceRoot`; an empty root reads nothing and saves nothing. A block not in the
    // form this editor writes is ignored whole, with a warning naming the file and the
    // fix, and its defaults hold.
    void LoadProjectState(const std::filesystem::path& workspaceRoot);
    // Writes the vocabulary's names and colors to the project's settings.
    void SaveVocabulary() const;

    // Gives every Markup entity of `world` without notes its own (the duplicate rule).
    void AdoptMarkupsWithoutNotes(ECS::World& world);

    EditorChangeNotifications& GetNotifications() const { return m_Notifications; }

    // --- The editor's views, for the panel and the inspector ----------------------
    // The main window's Scene View, which selection, hover and framing go through.
    using SceneViewProvider = std::function<SceneViewController*()>;
    void SetSceneViewProvider(SceneViewProvider provider) { m_SceneViewProvider = std::move(provider); }
    // Whether the editor is playing or paused: mark-ups are edited in the edit world only,
    // so the editor's own surfaces refuse while it is, and say so with kPlayModeNotice.
    static constexpr std::string_view kPlayModeNotice =
        "Mark-ups are edited outside play mode: stop play, then place or edit them";
    using PlayModeProvider = std::function<bool()>;
    void SetPlayModeProvider(PlayModeProvider provider) { m_PlayModeProvider = std::move(provider); }
    bool IsInPlayMode() const { return m_PlayModeProvider && m_PlayModeProvider(); }
    // Selects the entity as a click in the Scene View would (the Inspector and the Hierarchy
    // follow): a mark-up, or any entity a comment links to (the chip's click and its frame glyph). A mark-up's updates are marked
    // seen. `frame` also frames it: a mark-up's volume, else the entity.
    void SelectMarkup(ECS::World& world, ECS::EntityHandle entity, bool frame);
    // A Mark-ups panel row's or a comment's entity link's hover: outlines the entity as a
    // Hierarchy hover does and, for a mark-up, highlights its volume; an invalid handle clears
    // both.
    void HoverMarkup(ECS::EntityHandle entity);
    // Whether a Mark-ups panel row or an entity link holds the hover (HoverMarkup).
    bool IsPanelRowHovered() const { return m_HoveredRow.IsValid(); }
    // What the Scene View draws highlighted now. The hover is the Scene View's (so a
    // Hierarchy row's hover over a mark-up highlights it as its panel row's does), else the
    // panel row's; the selection is the Scene View's. Both are read when asked; the
    // selection is valid until it next changes.
    MarkupHighlightState GetHighlight() const;
    // The main window's Scene View, or null (no view yet, or tests).
    SceneViewController* GetSceneView() const { return m_SceneViewProvider ? m_SceneViewProvider() : nullptr; }
    // The displays of `world`'s regions, shared by every Scene View; a reset world (a scene
    // closed or opened) starts with none. A cache, not viewer state: drawing fills it.
    MarkupECS::MarkupRegionDisplayCache& RegionDisplaysOf(const ECS::World& world) const
    {
        return StateOf(world).RegionDisplays;
    }
    // The ground `world`'s regions share on `frame` (ReadMarkupRegionFrameGround), read on the
    // first call of the frame, however many views draw it.
    const MarkupRegionFrameGround& RegionFrameGroundOf(ECS::World& world, uint64 frame) const;

private:
    struct SeenMark
    {
        int64 UpdatedUnix = 0;
        uint32 Revision = 0;
        std::size_t EntryCount = 0; // the thread's length at the look, when RevisionKnown
        bool RevisionKnown = false; // this session saw it; revisions do not outlive a session
        bool operator==(const SeenMark&) const = default;
    };

    using HandleSet = std::unordered_set<ECS::EntityHandle, ECS::EntityHandleHash>;

    // One scene's saved state, by scene tag.
    struct SavedSceneState
    {
        std::unordered_set<std::string> Hidden;
        std::unordered_map<std::string, int64> Seen; // the last look's UpdatedUnix
    };

    // What the edit rule compares a change against.
    struct EditBaseline
    {
        float32 Matrix[16]{};
        std::string Name;
        uint8 Shape = 0;
        float32 Color[4]{};
        bool Region = false; // carries a MarkupRegion
    };

    // One world's state for its current lifecycle (a reset restarts its handles).
    struct WorldState
    {
        std::unordered_map<ECS::EntityHandle, EditBaseline, ECS::EntityHandleHash> Baselines;
        uint64 Generation = 0;
        HandleSet Hidden;
        std::unordered_map<ECS::EntityHandle, SeenMark, ECS::EntityHandleHash> Seen;
        HandleSet Read; // handles whose saved state was read, or that were set this session
        std::unordered_map<std::string, ECS::EntityHandle> TagOwners; // the handle a scene tag's saved state is
        std::vector<ECS::EntityHandle> Unwritten; // changed before the file could key them
        // A duplicate's scene tag as copied from its original: not its own until a save
        // gives it another.
        std::unordered_map<ECS::EntityHandle, std::string, ECS::EntityHandleHash> CopiedTags;
        MarkupECS::MarkupRegionDisplayCache RegionDisplays;
        MarkupRegionFrameGround FrameGround;
        std::optional<uint64> FrameGroundFrame; // the frame FrameGround was read on
    };

    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event);
    void OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& event);

    static EditBaseline ReadBaseline(const ECS::World& world, ECS::EntityHandle entity);
    // Every mark-up of `world` without a baseline gets its current state as one; the baselines
    // of entities that are gone or carry no Markup now are dropped.
    void CaptureBaselines(ECS::World& world);
    // The MarkupEditChange flags of what changed on `entity` since its baseline, which becomes
    // its state now; 0, and a new baseline, for one it had none for.
    uint8 TakeEditChanges(const ECS::World& world, ECS::EntityHandle entity);

    // The world's state; a reset world starts empty.
    WorldState& StateOf(const ECS::World& world) const;
    // Reads `entity`'s saved state on first sight, when it owns its scene tag.
    void ReadSaved(const ECS::World& world, WorldState& state, ECS::EntityHandle entity) const;
    // Makes `entity` the handle its scene tag's saved state is, unless a live handle already
    // is or the tag is one the entity copied as a duplicate.
    static bool ClaimTag(const ECS::World& world, WorldState& state, const std::string& tag, ECS::EntityHandle entity);
    // The open scene's key in the saved state: its path in the project, empty for none.
    std::string SceneKey() const;
    // Writes `changed` (and anything still unwritten) under the scene key, one file write.
    void WriteViewerState(const ECS::World& world, WorldState& state, std::span<const ECS::EntityHandle> changed) const;
    void SaveViewerFile() const;

    EditorChangeNotifications& m_Notifications;
    EditorChangeNotifications::SubscriptionToken m_ComponentToken;
    EditorChangeNotifications::SubscriptionToken m_StructureToken;
    Clock m_Clock;
    ScenePath m_ScenePath;
    std::vector<Components::MarkupAuthor> m_Scopes;
    std::filesystem::path m_WorkspaceRoot;
    // Filled by the const readers too: the saved state is read per mark-up on first sight.
    mutable std::unordered_map<uint64, WorldState> m_Worlds; // by World::GetWorldId
    mutable std::unordered_map<std::string, SavedSceneState> m_Saved; // by scene key

    // What the last CountUnseen counted against; any change counts again.
    struct UnseenCountKey
    {
        uint64 WorldId = 0;
        uint64 ResetGeneration = 0;
        uint32 Revision = 0;
        uint64 Structure = 0;
        uint64 Seen = 0;
        bool operator==(const UnseenCountKey&) const = default;
    };
    uint64 m_StructureVersion = 0; // advanced by every structure change
    uint64 m_SeenVersion = 0;      // advanced by every MarkSeen and project load
    mutable bool m_UnseenCountValid = false;
    mutable UnseenCountKey m_UnseenCountKey;
    mutable std::size_t m_UnseenCount = 0;
    SceneViewProvider m_SceneViewProvider;
    PlayModeProvider m_PlayModeProvider;
    ECS::EntityHandle m_HoveredRow{};
};

} // namespace GameEngine::Editor
