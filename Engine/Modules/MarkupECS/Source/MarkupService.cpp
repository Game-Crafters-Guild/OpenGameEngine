#include "MarkupECS/MarkupService.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/UnresolvedComponentStore.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::MarkupECS
{

namespace
{

// The index of a `tag<N>` line key, any case, or nullopt for any other key.
std::optional<size_t> TagLineIndex(std::string_view key)
{
    constexpr std::string_view kPrefix = "tag";
    if (key.size() <= kPrefix.size())
        return std::nullopt;
    for (size_t i = 0; i < kPrefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(key[i])) != kPrefix[i])
            return std::nullopt;
    }
    size_t index = 0;
    const char* first = key.data() + kPrefix.size();
    const char* last = key.data() + key.size();
    const auto [end, error] = std::from_chars(first, last, index);
    if (error != std::errc() || end != last)
        return std::nullopt;
    return index;
}

// The authored text a scene load kept for an unreadable tag line lives in the world's
// UnresolvedComponentStore under the line's key, and a save puts it back into the line of
// that key. A slot that moved takes its text to its new key, so the save writes it there.
void RenumberPreservedTagLines(ECS::World& world, ECS::EntityHandle entity,
                               std::span<const std::pair<size_t, size_t>> renumbered)
{
    if (!world.TryGetUnresolvedComponents())
        return;
    std::vector<ECS::PreservedField>* fields = world.GetUnresolvedComponents().FieldsForMutable(entity);
    if (!fields)
        return;
    const ECS::ComponentTypeId markupType = ECS::GetComponentTypeId<Components::Markup>();
    for (ECS::PreservedField& field : *fields)
    {
        const std::optional<size_t> index = field.TypeId == markupType ? TagLineIndex(field.Field) : std::nullopt;
        if (!index)
            continue;
        const auto move = std::find_if(renumbered.begin(), renumbered.end(),
                                       [&](const std::pair<size_t, size_t>& slot) { return slot.first == *index; });
        if (move != renumbered.end())
            field.Field = "tag" + std::to_string(move->second);
    }
}

struct DefaultStatus
{
    uint32 Id;
    std::string_view Name;
    float32 Color[4];
};

// The status group every vocabulary starts with, in id order.
constexpr DefaultStatus kDefaultStatuses[] = {
    {Components::kMarkupStatusProposed, "Proposed", {0.55f, 0.36f, 0.96f, 1.0f}},     // violet
    {Components::kMarkupStatusRequested, "Requested", {0.23f, 0.51f, 0.96f, 1.0f}},   // blue
    {Components::kMarkupStatusInProgress, "InProgress", {0.96f, 0.62f, 0.04f, 1.0f}}, // amber
    {Components::kMarkupStatusComplete, "Complete", {0.13f, 0.77f, 0.37f, 1.0f}},     // green
    {Components::kMarkupStatusRevision, "Revision", {0.39f, 0.40f, 0.95f, 1.0f}},     // indigo
    {Components::kMarkupStatusProblem, "Problem", {0.94f, 0.27f, 0.27f, 1.0f}},       // red
};

void CopyColor(const float32 (&from)[4], float32 (&to)[4])
{
    std::copy(std::begin(from), std::end(from), std::begin(to));
}

} // namespace

std::unique_ptr<MarkupService> MarkupService::s_Instance;

void MarkupService::Initialize()
{
    assert(!s_Instance && "MarkupService already initialized");
    s_Instance = std::make_unique<MarkupService>();
}

void MarkupService::Shutdown()
{
    s_Instance.reset();
}

MarkupService& MarkupService::Get()
{
    assert(s_Instance && "MarkupService not initialized");
    return *s_Instance;
}

MarkupService* MarkupService::TryGet()
{
    return s_Instance.get();
}

bool MarkupService::IsInitialized()
{
    return s_Instance != nullptr;
}

MarkupService::MarkupService()
{
    for (const DefaultStatus& status : kDefaultStatuses)
    {
        assert(status.Id == m_Tags.size() && "default status ids are their vocabulary indices");
        MarkupTag& tag = m_Tags.emplace_back();
        tag.Name = status.Name;
        tag.Group = kMarkupStatusGroup;
        CopyColor(status.Color, tag.Color);
    }
}

const MarkupTag* MarkupService::GetTag(uint32 id) const
{
    return id < m_Tags.size() ? &m_Tags[id] : nullptr;
}

uint32 MarkupService::FindTag(std::string_view name) const
{
    for (uint32 id = 0; id < m_Tags.size(); ++id)
    {
        if (m_Tags[id].Name == name)
            return id;
    }
    return kInvalidTag;
}

bool MarkupService::IsStatusTag(uint32 id) const
{
    const MarkupTag* tag = GetTag(id);
    return tag && tag->Group == kMarkupStatusGroup;
}

uint32 MarkupService::AddTag(std::string_view name, std::string_view group, const float32 (&color)[4])
{
    if (name.empty() || name.find_first_of("\r\n") != std::string_view::npos)
        return kInvalidTag;
    if (const uint32 existing = FindTag(name); existing != kInvalidTag)
        return existing;
    MarkupTag& tag = m_Tags.emplace_back();
    tag.Name = name;
    tag.Group = group;
    CopyColor(color, tag.Color);
    return static_cast<uint32>(m_Tags.size() - 1);
}

bool MarkupService::RenameTag(uint32 id, std::string_view name)
{
    if (id >= m_Tags.size() || name.empty() || name.find_first_of("\r\n") != std::string_view::npos)
        return false;
    const uint32 holder = FindTag(name);
    if (holder != kInvalidTag && holder != id)
        return false;
    m_Tags[id].Name = name;
    return true;
}

bool MarkupService::RemoveTag(uint32 id)
{
    if (m_Tags.empty() || id != m_Tags.size() - 1 || IsStatusTag(id))
        return false;
    for (const auto& [worldId, state] : m_Worlds)
    {
        for (const auto& [entityId, notes] : state.State.Notes)
        {
            if (std::find(notes.Tags.begin(), notes.Tags.end(), id) != notes.Tags.end())
                return false;
        }
    }
    m_Tags.pop_back();
    return true;
}

bool MarkupService::SetTagColor(uint32 id, const float32 (&color)[4])
{
    if (id >= m_Tags.size())
        return false;
    CopyColor(color, m_Tags[id].Color);
    return true;
}

MarkupService::WorldMarkups& MarkupService::WritableState(const ECS::World& world)
{
    WorldMarkups& state = m_Worlds[world.GetWorldId()];
    const uint64 generation = world.GetLifecycleResetGeneration();
    if (state.ResetGeneration != generation)
        state = WorldMarkups{generation};
    return state;
}

const MarkupService::WorldMarkups* MarkupService::ReadableState(const ECS::World& world) const
{
    const auto it = m_Worlds.find(world.GetWorldId());
    if (it == m_Worlds.end() || it->second.ResetGeneration != world.GetLifecycleResetGeneration())
        return nullptr;
    return &it->second;
}

MarkupNotes* MarkupService::FindNotes(const ECS::World& world, ECS::EntityHandle entity)
{
    return const_cast<MarkupNotes*>(std::as_const(*this).FindNotes(world, entity));
}

const MarkupNotes* MarkupService::FindNotes(const ECS::World& world, ECS::EntityHandle entity) const
{
    const WorldMarkups* state = ReadableState(world);
    if (!state)
        return nullptr;
    const auto it = state->State.Notes.find(entity.id);
    return it == state->State.Notes.end() ? nullptr : &it->second;
}

MarkupNotes& MarkupService::EnsureNotes(const ECS::World& world, ECS::EntityHandle entity)
{
    return WritableState(world).State.Notes[entity.id];
}

std::vector<ECS::EntityHandle> MarkupService::GetMarkups(const ECS::World& world) const
{
    std::vector<ECS::EntityHandle> markups;
    const WorldMarkups* state = ReadableState(world);
    if (!state)
        return markups;
    for (const auto& entry : state->State.Notes)
    {
        const ECS::EntityHandle entity(entry.first);
        if (world.IsValid(entity) && world.GetComponent<Components::Markup>(entity))
            markups.push_back(entity);
    }
    return markups;
}

bool MarkupService::BeginMarkup(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by,
                                int64 nowUnix)
{
    auto* markup = world.GetComponentForWrite<Components::Markup>(entity);
    if (!markup)
        return false;
    markup->Author = by;
    markup->CreatedUnix = nowUnix;
    EnsureNotes(world, entity).Entries.push_back(MarkupEntry{MarkupEntryKind::Created, by, nowUnix, markup->Status, {}});
    return Touch(world, entity, by, nowUnix);
}

bool MarkupService::AddComment(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by,
                               int64 nowUnix, std::string_view text)
{
    if (!world.GetComponent<Components::Markup>(entity))
        return false;
    EnsureNotes(world, entity).Entries.push_back(
        MarkupEntry{MarkupEntryKind::Comment, by, nowUnix, 0, std::string(text)});
    return Touch(world, entity, by, nowUnix);
}

bool MarkupService::SetDescription(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by,
                                   int64 nowUnix, std::string_view description)
{
    if (!world.GetComponent<Components::Markup>(entity))
        return false;
    EnsureNotes(world, entity).Description = description;
    return RecordEdit(world, entity, MarkupEditChanges({MarkupEditChange::Described}), by, nowUnix);
}

std::string_view MarkupEditChangeName(MarkupEditChange change)
{
    switch (change)
    {
    case MarkupEditChange::Moved: return "moved";
    case MarkupEditChange::Resized: return "resized";
    case MarkupEditChange::Rotated: return "rotated";
    case MarkupEditChange::Renamed: return "renamed";
    case MarkupEditChange::Reshaped: return "reshaped";
    case MarkupEditChange::Recolored: return "recolored";
    case MarkupEditChange::Described: return "described";
    case MarkupEditChange::Converted: return "converted";
    }
    return {};
}

namespace
{

// The mark-up's own color as opaque 0xAARRGGBB, 0 when it has none (Markup::Color all zero).
uint32 OwnColorArgb(const Components::Markup& markup)
{
    const float32* c = markup.Color;
    if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f)
        return 0;
    const auto byte = [](float32 v) { return static_cast<uint32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]);
}

} // namespace

bool MarkupService::RecordEdit(ECS::World& world, ECS::EntityHandle entity, uint8 changes, Components::MarkupAuthor by,
                               int64 nowUnix)
{
    if (changes == 0 || !world.GetComponent<Components::Markup>(entity))
        return false;
    std::vector<MarkupEntry>& entries = EnsureNotes(world, entity).Entries;
    MarkupEntry* edit = nullptr;
    if (!entries.empty())
    {
        MarkupEntry& last = entries.back();
        if (last.Kind == MarkupEntryKind::Edit && last.Author == by && nowUnix >= last.TimeUnix &&
            nowUnix - last.TimeUnix <= kEditCoalesceSeconds)
            edit = &last;
    }
    if (!edit)
        edit = &entries.emplace_back(MarkupEntry{MarkupEntryKind::Edit, by, nowUnix, 0, {}});
    edit->Changes = static_cast<uint8>(edit->Changes | changes);
    edit->TimeUnix = nowUnix;
    if ((changes & static_cast<uint8>(MarkupEditChange::Renamed)) != 0)
    {
        const auto* name = world.GetComponent<Components::Name>(entity);
        edit->Text = name ? std::string(name->View()) : std::string();
    }
    if ((changes & static_cast<uint8>(MarkupEditChange::Recolored)) != 0)
        edit->ColorArgb = OwnColorArgb(*world.GetComponent<Components::Markup>(entity));
    return Touch(world, entity, by, nowUnix);
}

bool MarkupService::SetStatus(ECS::World& world, ECS::EntityHandle entity, uint32 status,
                              Components::MarkupAuthor by, int64 nowUnix)
{
    auto* markup = world.GetComponentForWrite<Components::Markup>(entity);
    if (!markup || !IsStatusTag(status))
        return false;
    markup->Status = status;
    EnsureNotes(world, entity).Entries.push_back(
        MarkupEntry{MarkupEntryKind::StatusChange, by, nowUnix, status, {}});
    return Touch(world, entity, by, nowUnix);
}

bool MarkupService::SetTags(ECS::World& world, ECS::EntityHandle entity, std::span<const uint32> tags,
                            Components::MarkupAuthor by, int64 nowUnix)
{
    if (!world.GetComponent<Components::Markup>(entity))
        return false;
    for (const uint32 tag : tags)
    {
        if (!GetTag(tag) || IsStatusTag(tag))
            return false;
    }
    std::vector<uint32>& slots = EnsureNotes(world, entity).Tags;
    std::vector<uint32> next;
    next.reserve(std::max(slots.size(), tags.size()));
    std::vector<std::pair<size_t, size_t>> renumbered; // an unreadable slot's index before and after
    size_t given = 0;
    for (size_t index = 0; index < slots.size(); ++index)
    {
        if (slots[index] == kInvalidTag)
        {
            if (index != next.size())
                renumbered.emplace_back(index, next.size());
            next.push_back(kInvalidTag);
        }
        else if (given < tags.size())
        {
            next.push_back(tags[given++]);
        }
    }
    next.insert(next.end(), tags.begin() + static_cast<std::ptrdiff_t>(given), tags.end());
    slots = std::move(next);
    if (!renumbered.empty())
        RenumberPreservedTagLines(world, entity, renumbered);
    return Touch(world, entity, by, nowUnix);
}

bool MarkupService::Touch(ECS::World& world, ECS::EntityHandle entity, Components::MarkupAuthor by, int64 nowUnix)
{
    auto* markup = world.GetComponentForWrite<Components::Markup>(entity);
    if (!markup)
        return false;
    WorldMarkups& state = WritableState(world);
    ++state.State.Revision;
    markup->UpdatedUnix = nowUnix;
    markup->UpdatedBy = by;
    markup->Revision = state.State.Revision;
    return true;
}

uint32 MarkupService::GetRevision(const ECS::World& world) const
{
    const WorldMarkups* state = ReadableState(world);
    return state ? state->State.Revision : 0u;
}

void MarkupService::RestoreRevision(const ECS::World& world, uint32 revision)
{
    WorldMarkups& state = WritableState(world);
    state.State.Revision = std::max(state.State.Revision, revision);
}

MarkupWorldNotes MarkupService::CaptureWorld(const ECS::World& world) const
{
    const WorldMarkups* state = ReadableState(world);
    return state ? state->State : MarkupWorldNotes{};
}

void MarkupService::RestoreWorld(const ECS::World& world, MarkupWorldNotes notes)
{
    WritableState(world).State = std::move(notes);
}

} // namespace GameEngine::MarkupECS
