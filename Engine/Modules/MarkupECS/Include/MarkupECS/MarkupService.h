#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::MarkupECS
{

// The vocabulary group whose tags are statuses; a mark-up carries exactly one of them.
inline constexpr std::string_view kMarkupStatusGroup = "status";

// A name in the project's one tag vocabulary. A tag in the status group is a status;
// a tag with no group is a free tag a mark-up may carry any number of.
struct MarkupTag
{
    std::string Name;
    std::string Group;
    float32 Color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // RGBA, as the gizmo colors
};

enum class MarkupEntryKind : uint8
{
    Created = 0,
    Comment = 1,
    StatusChange = 2,
    // A slot a scene line held that could not be read: it keeps the thread's numbering,
    // and a save writes the line back as authored. Readers skip it.
    Unreadable = 3,
    // An action on the mark-up itself (moved, resized, renamed, ...): its MarkupEditChange
    // flags. Consecutive edits by one author close together are one entry (RecordEdit).
    Edit = 4,
};

// What an Edit entry did, as flags; one entry may carry several ("moved and resized").
enum class MarkupEditChange : uint8
{
    Moved = 1u << 0,
    Resized = 1u << 1,
    Rotated = 1u << 2,
    Renamed = 1u << 3,
    Reshaped = 1u << 4, // the volume's shape kind (box, sphere)
    Recolored = 1u << 5,
    Described = 1u << 6, // the description
    Converted = 1u << 7, // a box turned into a region over its footprint
};

// Every change, in the order a thread names them.
inline constexpr MarkupEditChange kMarkupEditChangeOrder[] = {
    MarkupEditChange::Converted, MarkupEditChange::Moved,    MarkupEditChange::Resized,
    MarkupEditChange::Rotated,   MarkupEditChange::Renamed,  MarkupEditChange::Reshaped,
    MarkupEditChange::Recolored, MarkupEditChange::Described,
};

// The change's name in the scene file and over the debug port: "converted", "moved", "resized",
// "rotated", "renamed", "reshaped", "recolored", "described".
std::string_view MarkupEditChangeName(MarkupEditChange change);

// The flags `changes` holds, as one byte.
inline constexpr uint8 MarkupEditChanges(std::initializer_list<MarkupEditChange> changes)
{
    uint8 bits = 0;
    for (const MarkupEditChange change : changes)
        bits = static_cast<uint8>(bits | static_cast<uint8>(change));
    return bits;
}

// One line of a mark-up's thread. The author is data: an entry keeps the author it
// was written with.
struct MarkupEntry
{
    MarkupEntryKind Kind = MarkupEntryKind::Comment;
    Components::MarkupAuthor Author = Components::MarkupAuthor::User;
    int64 TimeUnix = 0;
    uint32 Status = 0; // the status set, for a StatusChange entry
    std::string Text; // a comment's text; for an Edit that renamed, the name it renamed to
    uint8 Changes = 0; // an Edit entry's MarkupEditChange flags
    uint32 ColorArgb = 0; // for an Edit that recolored, the color it gave (0xAARRGGBB); 0: the status color
};

// What a mark-up says, beside its entity: text that cannot be a component field.
struct MarkupNotes
{
    std::string Description;
    std::vector<uint32> Tags; // free tags, by vocabulary id; kInvalidTag is a slot a scene line held
                              // that could not be read, which readers skip
    std::vector<MarkupEntry> Entries;
};

// One world's mark-up notes and revision, as CaptureWorld takes them.
struct MarkupWorldNotes
{
    uint32 Revision = 0;
    std::unordered_map<uint32, MarkupNotes> Notes; // by EntityHandle::id
};

// The mark-ups' notes, the project's tag vocabulary and each world's mark-up revision.
//
// Notes are keyed by the world's id (World::GetWorldId, never its address) and the
// entity's full handle (index and generation). They outlive the entity: a delete leaves
// them dormant, a delete that preserves the handle (World::DestroyEntityImmediatePreserveHandle,
// the editor's delete) is revived with them, and an entity that reuses the index carries
// a new generation and starts with none. A world reset (World::Clear, as a scene load
// that replaces the world does) restarts entity versions, so a world's notes and
// revision belong to its lifecycle reset generation: after a reset the world reads as
// having none, and the first write drops the old ones. Only the editor's play session
// carries the notes across a reset that brings the same entities back under their
// handles, with CaptureWorld and RestoreWorld; the other in-place restores (the undo of a
// world snapshot command, an additive scene load, a version-control revert) drop them.
//
// Main thread only: the editor's tools, its debug server and scene load and save call it.
class MarkupService
{
public:
    static void Initialize();
    static void Shutdown();
    static MarkupService& Get();
    static MarkupService* TryGet();
    static bool IsInitialized();

    MarkupService();

    // --- Tag vocabulary -------------------------------------------------------------
    static constexpr uint32 kInvalidTag = 0xFFFFFFFFu;

    // Ids are indices, stable for the service's lifetime; the six status defaults
    // hold the ids Components::kMarkupStatus* name.
    uint32 GetTagCount() const { return static_cast<uint32>(m_Tags.size()); }
    const MarkupTag* GetTag(uint32 id) const;
    // Exact, case-sensitive match; kInvalidTag when no tag has the name.
    uint32 FindTag(std::string_view name) const;
    bool IsStatusTag(uint32 id) const;
    // The id of the tag named `name`, added to `group` with `color` when no tag has the
    // name (an existing tag keeps its group and color). kInvalidTag for an empty name or a
    // name holding a line break.
    uint32 AddTag(std::string_view name, std::string_view group, const float32 (&color)[4]);
    // Refused for an unknown id, an empty name, a name holding a line break or a name
    // another tag has.
    bool RenameTag(uint32 id, std::string_view name);
    // Removes the newest tag, which an undone edit had added. Refused for any other id (ids
    // are indices), a status tag, or a tag some mark-up's notes still hold.
    bool RemoveTag(uint32 id);
    bool SetTagColor(uint32 id, const float32 (&color)[4]);

    // --- Notes ------------------------------------------------------------------------
    // Null when the entity has no notes.
    MarkupNotes* FindNotes(const ECS::World& world, ECS::EntityHandle entity);
    const MarkupNotes* FindNotes(const ECS::World& world, ECS::EntityHandle entity) const;
    // The entity's notes, created empty when it has none.
    MarkupNotes& EnsureNotes(const ECS::World& world, ECS::EntityHandle entity);
    // Every live entity of `world` that has notes and a Markup, in no particular order.
    std::vector<ECS::EntityHandle> GetMarkups(const ECS::World& world) const;

    // --- Writers ------------------------------------------------------------------------
    // Each changes one mark-up on behalf of `by` at `nowUnix` and touches it (Touch).
    // Each refuses an entity with no Markup.

    // Stamps a new mark-up: author, creation time and one Created entry.
    bool BeginMarkup(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by, int64 nowUnix);
    bool AddComment(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by, int64 nowUnix,
                    std::string_view text);
    // Sets the description and records the edit (RecordEdit, Described).
    bool SetDescription(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by, int64 nowUnix,
                        std::string_view description);
    // How far apart two edits by one author may be and still be one Edit entry.
    static constexpr int64 kEditCoalesceSeconds = 120;
    // Records an action on the mark-up (`changes`, MarkupEditChange flags): when the thread's
    // last entry is an Edit by `by` at most kEditCoalesceSeconds before `nowUnix`, that entry
    // takes the new flags and time, else an Edit entry is appended. So a drag's commit, a
    // rename typed in three goes and a move then a resize read as one line in the thread
    // and the Activity tab. A rename keeps the mark-up's name (its Name) as it is now in the
    // entry's Text, a recolor its own color (Markup::Color) as it is now in ColorArgb. Refuses no
    // change (0).
    bool RecordEdit(ECS::World& world, ECS::EntityHandle entity, uint8 changes, Components::MarkupAuthor by,
                    int64 nowUnix);
    // Sets the status and appends a StatusChange entry; refuses a tag outside the status group.
    bool SetStatus(ECS::World& world, ECS::EntityHandle entity, uint32 status, Components::MarkupAuthor by,
                   int64 nowUnix);
    // Replaces the free tags; refuses an unknown id or a status tag. A slot an unreadable
    // scene line holds (kInvalidTag) stays where it is, so a save still writes that line back
    // under its own key; the tags fill the other slots in order and the rest are appended.
    // Fewer tags than readable slots drop the last readable slots, which renumbers an
    // unreadable slot after them; its authored text moves with it to the new key.
    bool SetTags(ECS::World& world, ECS::EntityHandle entity, std::span<const uint32> tags,
                 Components::MarkupAuthor by, int64 nowUnix);

    // How an edit stamps a mark-up's UpdatedUnix, UpdatedBy and Revision: it advances the
    // world's revision, so the mark-up's Revision is the new world revision. The writers
    // above and the editor's change subscriber call it; a scene load restores the stamps
    // as saved instead. Notifies nothing. Refuses an entity with no Markup.
    bool Touch(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by, int64 nowUnix);

    // The world's mark-up revision: 0 until the first touch, then the last touch's.
    uint32 GetRevision(const ECS::World& world) const;
    // A loaded mark-up's saved revision: the world's revision becomes at least `revision`.
    // A reader's `since` from before the reopen then orders what follows, except when the
    // mark-up touched last was deleted before the save, which takes its revision with it.
    void RestoreRevision(const ECS::World& world, uint32 revision);

    // --- Restoring a world in place ----------------------------------------------------
    // The world's notes and revision as they are now.
    MarkupWorldNotes CaptureWorld(const ECS::World& world) const;
    // Makes `notes` the world's notes and revision under its current reset generation,
    // for a reset that restored the entities `notes` was captured from under the same
    // handles; the notes of any other entity would attach to whatever holds its handle.
    void RestoreWorld(const ECS::World& world, MarkupWorldNotes notes);

private:
    struct WorldMarkups
    {
        uint64 ResetGeneration = 0; // World::GetLifecycleResetGeneration these belong to
        MarkupWorldNotes State;
    };

    // The world's state for writing; a reset since the state was written drops it first.
    WorldMarkups& WritableState(const ECS::World& world);
    // The world's state for reading; null when it has none or a reset outdated it.
    const WorldMarkups* ReadableState(const ECS::World& world) const;

    std::vector<MarkupTag> m_Tags;
    std::unordered_map<uint64, WorldMarkups> m_Worlds; // by World::GetWorldId

    static std::unique_ptr<MarkupService> s_Instance;
};

} // namespace GameEngine::MarkupECS
