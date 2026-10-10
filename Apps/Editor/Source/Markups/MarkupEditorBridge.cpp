#include "Markups/MarkupEditorBridge.h"

#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"
#include "SceneViewController.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupPresentation.h"
#include "SceneView/SceneViewCameraRig.h"
#include "SceneView/SceneViewFraming.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

MarkupEditorBridge* s_Installed = nullptr;

// The keys of this feature's blocks in the project's and the user's project settings.
constexpr const char* kVocabularyKey = "markupTags";
constexpr const char* kViewerStateKey = "markupViewer";

// One tag of the saved vocabulary, in id order.
struct SavedTag
{
    std::string Name;
    std::string Group;
    float32 Color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// The saved viewer state as read: per scene key, the hidden tags and each tag's last look.
using HiddenByScene = std::unordered_map<std::string, std::unordered_set<std::string>>;
using SeenByScene = std::unordered_map<std::string, std::unordered_map<std::string, int64>>;

bool IsTouchingKind(EditorChangeNotifications::ChangeKind kind)
{
    return kind == EditorChangeNotifications::ChangeKind::Commit ||
           kind == EditorChangeNotifications::ChangeKind::UndoRedo;
}

bool IsMarkupPart(ECS::ComponentTypeId type)
{
    return type == ECS::GetComponentTypeId<Components::Markup>() ||
           type == ECS::GetComponentTypeId<Components::MarkupVolume>() ||
           type == ECS::GetComponentTypeId<Components::MarkupRegion>() ||
           type == ECS::GetComponentTypeId<Components::SplineComponent>() ||
           type == ECS::GetComponentTypeId<Components::Transform>() ||
           type == ECS::GetComponentTypeId<Components::Name>();
}

// A region is always closed: one whose outline arrives open, read from a scene file or written
// through set_component, is closed again, with a warning naming it.
void CloseOpenRegionOutline(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
    SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    if (!spline || !splines || !world.GetComponent<Components::MarkupRegion>(entity))
        return;
    const SplineECS::SplineHandle handle(spline->SplineDataIndex, spline->SplineDataGeneration);
    Spline::SplineData* data = splines->GetSplineData(handle);
    if (!data || data->Closed)
        return;
    data->Closed = true;
    splines->RebuildCache(handle);
    Logger::Log::Warning("Mark-up region '{}' had an open outline; a region is always closed, so it was closed "
                         "again. Save the scene to keep it closed.",
                         MarkupTitle(world, entity));
}

// How far two transform values may differ and still be the same: a commit writes back what
// it read, rounding included.
constexpr float32 kEditEpsilon = 1.0e-4f;

bool Differs(float32 a, float32 b)
{
    return std::fabs(a - b) > kEditEpsilon * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}

float32 ColumnLength(const float32 (&matrix)[16], int column)
{
    const float32* c = matrix + column * 4;
    return std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
}

// Moved, Resized and Rotated as the two column-major matrices differ: the translation, the
// axes' lengths, and the axes' directions.
uint8 TransformChanges(const float32 (&before)[16], const float32 (&after)[16])
{
    using MarkupECS::MarkupEditChange;
    uint8 changes = 0;
    if (Differs(before[12], after[12]) || Differs(before[13], after[13]) || Differs(before[14], after[14]))
        changes |= static_cast<uint8>(MarkupEditChange::Moved);
    bool resized = false;
    bool rotated = false;
    for (int column = 0; column < 3; ++column)
    {
        const float32 lengthBefore = ColumnLength(before, column);
        const float32 lengthAfter = ColumnLength(after, column);
        resized = resized || Differs(lengthBefore, lengthAfter);
        if (lengthBefore <= 0.0f || lengthAfter <= 0.0f)
            continue;
        for (int row = 0; row < 3; ++row)
            rotated = rotated || Differs(before[column * 4 + row] / lengthBefore, after[column * 4 + row] / lengthAfter);
    }
    if (resized)
        changes |= static_cast<uint8>(MarkupEditChange::Resized);
    if (rotated)
        changes |= static_cast<uint8>(MarkupEditChange::Rotated);
    return changes;
}

std::string SceneTagOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity);
    return tag ? std::string(tag->View()) : std::string();
}

// The vocabulary block as this editor writes it, or nullopt when it is in any other form:
// an array of objects, each with a non-empty string name, an optional string group and an
// optional [r, g, b, a] of numbers.
std::optional<std::vector<SavedTag>> ReadVocabularyBlock(const nlohmann::json& block)
{
    if (!block.is_array())
        return std::nullopt;
    std::vector<SavedTag> tags;
    for (const nlohmann::json& entry : block)
    {
        if (!entry.is_object())
            return std::nullopt;
        SavedTag tag;
        const auto name = entry.find("name");
        if (name == entry.end() || !name->is_string() || name->get<std::string>().empty())
            return std::nullopt;
        tag.Name = name->get<std::string>();
        if (const auto group = entry.find("group"); group != entry.end())
        {
            if (!group->is_string())
                return std::nullopt;
            tag.Group = group->get<std::string>();
        }
        if (const auto color = entry.find("color"); color != entry.end())
        {
            if (!color->is_array() || color->size() != 4)
                return std::nullopt;
            for (size_t channel = 0; channel < 4; ++channel)
            {
                if (!(*color)[channel].is_number())
                    return std::nullopt;
                tag.Color[channel] = (*color)[channel].get<float32>();
            }
        }
        tags.push_back(std::move(tag));
    }
    return tags;
}

// Saved in id order, so a renamed or recolored default keeps its id.
void ApplyVocabulary(MarkupECS::MarkupService& service, const std::vector<SavedTag>& tags)
{
    for (uint32 id = 0; id < tags.size(); ++id)
    {
        const SavedTag& tag = tags[id];
        if (id < service.GetTagCount())
        {
            if (!service.RenameTag(id, tag.Name))
                Logger::Log::Warning("Mark-ups: project tag {} '{}' clashes with another tag's name; kept '{}'", id,
                                     tag.Name, service.GetTag(id)->Name);
            (void)service.SetTagColor(id, tag.Color);
        }
        else
        {
            (void)service.AddTag(tag.Name, tag.Group, tag.Color);
        }
    }
}

// The viewer block as this editor writes it: {"hidden": {scene: [tag]}, "seen": {scene:
// {tag: unix seconds}}}, both optional. False for any other form.
bool ReadViewerBlock(const nlohmann::json& block, HiddenByScene& outHidden, SeenByScene& outSeen)
{
    if (!block.is_object())
        return false;
    if (const auto hidden = block.find("hidden"); hidden != block.end())
    {
        if (!hidden->is_object())
            return false;
        for (const auto& [scene, tags] : hidden->items())
        {
            if (!tags.is_array())
                return false;
            for (const nlohmann::json& tag : tags)
            {
                if (!tag.is_string())
                    return false;
                outHidden[scene].insert(tag.get<std::string>());
            }
        }
    }
    if (const auto seen = block.find("seen"); seen != block.end())
    {
        if (!seen->is_object())
            return false;
        for (const auto& [scene, looks] : seen->items())
        {
            if (!looks.is_object())
                return false;
            for (const auto& [tag, updatedUnix] : looks.items())
            {
                if (!updatedUnix.is_number_integer())
                    return false;
                outSeen[scene][tag] = updatedUnix.get<int64>();
            }
        }
    }
    return true;
}

void WarnUnreadableBlock(const SettingsStore& store, const char* key)
{
    Logger::Log::Warning("Mark-ups: '{}' in {} is not in the form this editor writes, so it is ignored and the "
                         "defaults hold. Correct the entry or delete it; the editor writes it again at its next "
                         "change.",
                         key, store.GetFilePath().string());
}

} // namespace

MarkupEditorBridge::MarkupEditorBridge(EditorChangeNotifications& notifications, Clock clock, ScenePath scenePath)
    : m_Notifications(notifications), m_Clock(std::move(clock)), m_ScenePath(std::move(scenePath))
{
    m_ComponentToken = m_Notifications.SubscribeComponentChanged(
        [this](const EditorChangeNotifications::ComponentChangedEvent& event) { OnComponentChanged(event); });
    m_StructureToken = m_Notifications.SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent& event) { OnWorldStructureChanged(event); });
}

MarkupEditorBridge::~MarkupEditorBridge()
{
    m_Notifications.Unsubscribe(m_ComponentToken);
    m_Notifications.Unsubscribe(m_StructureToken);
    if (s_Installed == this)
        s_Installed = nullptr;
}

MarkupEditorBridge* MarkupEditorBridge::TryGet()
{
    return s_Installed;
}

void MarkupEditorBridge::Install(MarkupEditorBridge* bridge)
{
    s_Installed = bridge;
}

MarkupEditorBridge::AttributionScope::AttributionScope(MarkupEditorBridge& bridge, Components::MarkupAuthor author)
    : m_Bridge(bridge)
{
    m_Bridge.m_Scopes.push_back(author);
}

MarkupEditorBridge::AttributionScope::~AttributionScope()
{
    m_Bridge.m_Scopes.pop_back();
}

Components::MarkupAuthor MarkupEditorBridge::CurrentAuthor() const
{
    return m_Scopes.empty() ? Components::MarkupAuthor::User : m_Scopes.back();
}

void MarkupEditorBridge::BeginAgentRequest()
{
    m_Scopes.push_back(Components::MarkupAuthor::Agent);
}

void MarkupEditorBridge::EndAgentRequest()
{
    if (!m_Scopes.empty())
        m_Scopes.pop_back();
}

std::string MarkupEditorBridge::SceneKey() const
{
    const std::optional<std::filesystem::path> scene = m_ScenePath ? m_ScenePath() : std::nullopt;
    if (!scene || scene->empty())
        return {};
    const std::filesystem::path normal = scene->lexically_normal();
    if (!m_WorkspaceRoot.empty())
    {
        const std::filesystem::path relative = normal.lexically_relative(m_WorkspaceRoot.lexically_normal());
        if (!relative.empty() && *relative.begin() != "..")
            return relative.generic_string();
    }
    return normal.generic_string();
}

MarkupEditorBridge::EditBaseline MarkupEditorBridge::ReadBaseline(const ECS::World& world, ECS::EntityHandle entity)
{
    EditBaseline baseline;
    if (const auto* transform = world.GetComponent<Components::Transform>(entity))
        std::copy(std::begin(transform->matrix), std::end(transform->matrix), baseline.Matrix);
    if (const auto* name = world.GetComponent<Components::Name>(entity))
        baseline.Name = name->View();
    if (const auto* volume = world.GetComponent<Components::MarkupVolume>(entity))
        baseline.Shape = static_cast<uint8>(volume->Shape);
    baseline.Region = world.GetComponent<Components::MarkupRegion>(entity) != nullptr;
    if (const auto* markup = world.GetComponent<Components::Markup>(entity))
        std::copy(std::begin(markup->Color), std::end(markup->Color), baseline.Color);
    return baseline;
}

void MarkupEditorBridge::CaptureBaselines(ECS::World& world)
{
    WorldState& state = StateOf(world);
    // A deleted entity's or a removed Markup's baseline goes, so a Markup added again later
    // compares against its own first state, not the old one.
    std::erase_if(state.Baselines, [&world](const auto& baseline) {
        return !world.IsValid(baseline.first) || !world.GetComponent<Components::Markup>(baseline.first);
    });
    world.Query<ECS::Read<Components::Markup>>().Each([&](ECS::EntityHandle entity, const Components::Markup&) {
        if (!state.Baselines.contains(entity))
            state.Baselines.emplace(entity, ReadBaseline(world, entity));
    });
}

uint8 MarkupEditorBridge::TakeEditChanges(const ECS::World& world, ECS::EntityHandle entity)
{
    using MarkupECS::MarkupEditChange;
    WorldState& state = StateOf(world);
    EditBaseline now = ReadBaseline(world, entity);
    const auto found = state.Baselines.find(entity);
    if (found == state.Baselines.end())
    {
        state.Baselines.emplace(entity, std::move(now));
        return 0;
    }
    const EditBaseline& before = found->second;
    // A box converted to a region stands at its label point over the same footprint: the new
    // transform is the conversion's, not a move or a resize.
    uint8 changes = !before.Region && now.Region ? static_cast<uint8>(MarkupEditChange::Converted)
                                                 : TransformChanges(before.Matrix, now.Matrix);
    if (before.Name != now.Name)
        changes |= static_cast<uint8>(MarkupEditChange::Renamed);
    if (before.Shape != now.Shape)
        changes |= static_cast<uint8>(MarkupEditChange::Reshaped);
    if (!std::equal(std::begin(before.Color), std::end(before.Color), std::begin(now.Color)))
        changes |= static_cast<uint8>(MarkupEditChange::Recolored);
    found->second = std::move(now);
    return changes;
}

MarkupEditorBridge::WorldState& MarkupEditorBridge::StateOf(const ECS::World& world) const
{
    WorldState& state = m_Worlds[world.GetWorldId()];
    const uint64 generation = world.GetLifecycleResetGeneration();
    if (state.Generation != generation)
    {
        state = WorldState{};
        state.Generation = generation;
    }
    return state;
}

bool MarkupEditorBridge::ClaimTag(const ECS::World& world, WorldState& state, const std::string& tag,
                                  ECS::EntityHandle entity)
{
    if (const auto copied = state.CopiedTags.find(entity); copied != state.CopiedTags.end())
    {
        if (copied->second == tag)
            return false;
        state.CopiedTags.erase(copied);
    }
    const auto [it, claimed] = state.TagOwners.try_emplace(tag, entity);
    if (claimed || it->second == entity)
        return true;
    if (world.IsValid(it->second) && SceneTagOf(world, it->second) == tag)
        return false;
    it->second = entity;
    return true;
}

void MarkupEditorBridge::ReadSaved(const ECS::World& world, WorldState& state, ECS::EntityHandle entity) const
{
    if (!state.Read.insert(entity).second || m_Saved.empty())
        return;
    const std::string tag = SceneTagOf(world, entity);
    if (tag.empty() || !ClaimTag(world, state, tag, entity))
        return;
    const auto scene = m_Saved.find(SceneKey());
    if (scene == m_Saved.end())
        return;
    if (scene->second.Hidden.contains(tag))
        state.Hidden.insert(entity);
    if (const auto seen = scene->second.Seen.find(tag); seen != scene->second.Seen.end())
        state.Seen[entity] = SeenMark{seen->second, 0, 0, false};
}

void MarkupEditorBridge::WriteViewerState(const ECS::World& world, WorldState& state,
                                          std::span<const ECS::EntityHandle> changed) const
{
    if (m_WorkspaceRoot.empty())
        return;
    std::vector<ECS::EntityHandle> pending = std::move(state.Unwritten);
    state.Unwritten.clear();
    pending.insert(pending.end(), changed.begin(), changed.end());

    const std::string scene = SceneKey();
    SavedSceneState* saved = scene.empty() ? nullptr : &m_Saved[scene];
    bool wrote = false;
    for (const ECS::EntityHandle entity : pending)
    {
        if (!world.IsValid(entity))
            continue;
        const std::string tag = SceneTagOf(world, entity);
        if (!saved || tag.empty() || !ClaimTag(world, state, tag, entity))
        {
            if (std::find(state.Unwritten.begin(), state.Unwritten.end(), entity) == state.Unwritten.end())
                state.Unwritten.push_back(entity);
            continue;
        }
        if (state.Hidden.contains(entity))
            saved->Hidden.insert(tag);
        else
            saved->Hidden.erase(tag);
        if (const auto seen = state.Seen.find(entity); seen != state.Seen.end())
            saved->Seen[tag] = seen->second.UpdatedUnix;
        else
            saved->Seen.erase(tag);
        wrote = true;
    }
    if (wrote)
        SaveViewerFile();
}

void MarkupEditorBridge::OnSceneSaved(const ECS::World& world)
{
    WorldState& state = StateOf(world);
    if (!state.Unwritten.empty())
        WriteViewerState(world, state, {});
}

bool MarkupEditorBridge::IsHidden(const ECS::World& world, ECS::EntityHandle entity) const
{
    WorldState& state = StateOf(world);
    ReadSaved(world, state, entity);
    return state.Hidden.contains(entity);
}

void MarkupEditorBridge::SetHidden(const ECS::World& world, std::span<const ECS::EntityHandle> entities, bool hidden)
{
    WorldState& state = StateOf(world);
    std::vector<ECS::EntityHandle> changed;
    for (const ECS::EntityHandle entity : entities)
    {
        ReadSaved(world, state, entity);
        if (hidden ? state.Hidden.insert(entity).second : state.Hidden.erase(entity) > 0)
            changed.push_back(entity);
    }
    if (!changed.empty())
        WriteViewerState(world, state, changed);
}

bool MarkupEditorBridge::HasUnseenUpdate(const ECS::World& world, ECS::EntityHandle entity) const
{
    const auto* markup = world.GetComponent<Components::Markup>(entity);
    if (!markup || markup->UpdatedBy != Components::MarkupAuthor::Agent)
        return false;
    WorldState& state = StateOf(world);
    ReadSaved(world, state, entity);
    const auto it = state.Seen.find(entity);
    if (it == state.Seen.end())
        return true;
    if (it->second.RevisionKnown && markup->Revision != 0)
        return markup->Revision > it->second.Revision;
    return markup->UpdatedUnix > it->second.UpdatedUnix;
}

void MarkupEditorBridge::MarkSeen(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* markup = world.GetComponent<Components::Markup>(entity);
    if (!markup)
        return;
    WorldState& state = StateOf(world);
    ReadSaved(world, state, entity);
    const MarkupECS::MarkupNotes* notes =
        MarkupECS::MarkupService::TryGet() ? MarkupECS::MarkupService::Get().FindNotes(world, entity) : nullptr;
    const SeenMark mark{markup->UpdatedUnix, markup->Revision, notes ? notes->Entries.size() : 0, true};
    // Every inspector build and selection marks its mark-up seen: nothing to write unless
    // the mark changed.
    if (const auto seen = state.Seen.find(entity); seen != state.Seen.end() && seen->second == mark)
        return;
    state.Seen[entity] = mark;
    WriteViewerState(world, state, {&entity, 1});
    ++m_SeenVersion;
}

std::size_t MarkupEditorBridge::FirstUnseenEntry(const ECS::World& world, ECS::EntityHandle entity) const
{
    const auto* service = MarkupECS::MarkupService::TryGet();
    const MarkupECS::MarkupNotes* notes = service ? service->FindNotes(world, entity) : nullptr;
    if (!notes)
        return 0;
    const std::size_t length = notes->Entries.size();
    if (!HasUnseenUpdate(world, entity))
        return length;
    const WorldState& state = StateOf(world);
    const auto it = state.Seen.find(entity);
    if (it == state.Seen.end())
        return 0;
    if (it->second.RevisionKnown)
        return std::min(it->second.EntryCount, length);
    const auto first = std::find_if(notes->Entries.begin(), notes->Entries.end(), [&](const MarkupECS::MarkupEntry& entry) {
        return entry.TimeUnix > it->second.UpdatedUnix;
    });
    return static_cast<std::size_t>(first - notes->Entries.begin());
}

std::size_t MarkupEditorBridge::CountUnseen(const ECS::World& world) const
{
    const auto* service = MarkupECS::MarkupService::TryGet();
    if (!service)
        return 0;
    const UnseenCountKey key{world.GetWorldId(), world.GetLifecycleResetGeneration(), service->GetRevision(world),
                             m_StructureVersion, m_SeenVersion};
    if (m_UnseenCountValid && key == m_UnseenCountKey)
        return m_UnseenCount;
    const std::vector<ECS::EntityHandle> markups = service->GetMarkups(world);
    m_UnseenCount = 0;
    for (const ECS::EntityHandle entity : markups)
    {
        const MarkupECS::MarkupNotes* notes = service->FindNotes(world, entity);
        if (!notes)
            continue;
        const std::size_t first = FirstUnseenEntry(world, entity);
        m_UnseenCount += static_cast<std::size_t>(
            std::count_if(notes->Entries.begin() + static_cast<std::ptrdiff_t>(first), notes->Entries.end(),
                          [](const MarkupECS::MarkupEntry& entry) {
                              return entry.Author == Components::MarkupAuthor::Agent &&
                                     entry.Kind != MarkupECS::MarkupEntryKind::Unreadable;
                          }));
    }
    m_UnseenCountKey = key;
    m_UnseenCountValid = true;
    return m_UnseenCount;
}

void MarkupEditorBridge::LoadProjectState(const std::filesystem::path& workspaceRoot)
{
    m_WorkspaceRoot = workspaceRoot;
    m_Worlds.clear();
    m_Saved.clear();
    ++m_SeenVersion;
    if (workspaceRoot.empty())
        return;

    if (auto* service = MarkupECS::MarkupService::TryGet())
    {
        SettingsStore project = OpenProjectSettings(workspaceRoot);
        (void)project.Load();
        if (const auto block = project.Json().find(kVocabularyKey); block != project.Json().end())
        {
            if (const std::optional<std::vector<SavedTag>> tags = ReadVocabularyBlock(*block))
                ApplyVocabulary(*service, *tags);
            else
                WarnUnreadableBlock(project, kVocabularyKey);
        }
    }

    SettingsStore user = OpenUserProjectSettings(workspaceRoot);
    (void)user.Load();
    if (const auto block = user.Json().find(kViewerStateKey); block != user.Json().end())
    {
        HiddenByScene hidden;
        SeenByScene seen;
        if (!ReadViewerBlock(*block, hidden, seen))
        {
            WarnUnreadableBlock(user, kViewerStateKey);
            return;
        }
        for (auto& [scene, tags] : hidden)
            m_Saved[scene].Hidden = std::move(tags);
        for (auto& [scene, looks] : seen)
            m_Saved[scene].Seen = std::move(looks);
    }
}

void MarkupEditorBridge::SaveVocabulary() const
{
    const auto* service = MarkupECS::MarkupService::TryGet();
    if (m_WorkspaceRoot.empty() || !service)
        return;
    nlohmann::json tags = nlohmann::json::array();
    for (uint32 id = 0; id < service->GetTagCount(); ++id)
    {
        const MarkupECS::MarkupTag* tag = service->GetTag(id);
        tags.push_back({{"name", tag->Name},
                        {"group", tag->Group},
                        {"color", {tag->Color[0], tag->Color[1], tag->Color[2], tag->Color[3]}}});
    }
    SettingsStore project = OpenProjectSettings(m_WorkspaceRoot);
    (void)project.Load();
    project.SetJson(kVocabularyKey, tags);
    std::string error;
    if (!project.Save(&error))
        Logger::Log::Warning("Mark-ups: could not save the project's tag vocabulary: {}", error);
}

void MarkupEditorBridge::SaveViewerFile() const
{
    nlohmann::json hidden = nlohmann::json::object();
    nlohmann::json seen = nlohmann::json::object();
    for (const auto& [scene, state] : m_Saved)
    {
        if (!state.Hidden.empty())
        {
            std::vector<std::string> tags(state.Hidden.begin(), state.Hidden.end());
            std::sort(tags.begin(), tags.end());
            hidden[scene] = tags;
        }
        if (!state.Seen.empty())
        {
            nlohmann::json& looks = seen[scene] = nlohmann::json::object();
            for (const auto& [tag, updatedUnix] : state.Seen)
                looks[tag] = updatedUnix;
        }
    }
    SettingsStore user = OpenUserProjectSettings(m_WorkspaceRoot);
    (void)user.Load();
    user.SetJson(kViewerStateKey, {{"hidden", hidden}, {"seen", seen}});
    std::string error;
    if (!user.Save(&error))
        Logger::Log::Warning("Mark-ups: could not save the hidden and seen state: {}", error);
}

void MarkupEditorBridge::SelectMarkup(ECS::World& world, ECS::EntityHandle entity, bool frame)
{
    SceneViewController* view = m_SceneViewProvider ? m_SceneViewProvider() : nullptr;
    if (!view || !world.IsValid(entity))
        return;
    view->OnEntityPicked(entity);
    MarkSeen(world, entity);
    if (!frame)
        return;
    // The volume, not the entity's origin: a mark-up carries no bounds the Scene View's
    // own framing reads. Its bounding sphere fits the view's field of view.
    const std::optional<MarkupWorldVolume> volume = ReadMarkupWorldVolume(world, entity);
    if (!volume)
    {
        view->FrameOrigin();
        return;
    }
    SceneViewCameraPose pose = view->GetCameraPose();
    const CameraRig::Vec3d look = CameraRig::LookDirection(pose.YawDeg, pose.PitchDeg);
    const Mathematics::Vector3 direction(static_cast<float>(look.X), static_cast<float>(look.Y),
                                         static_cast<float>(look.Z));
    if (ComputeLookAtPose(volume->Center, direction, MarkupFrameDistance(*volume, MarkupFrameTanHalfFov(view)), pose))
        view->SetCameraPose(pose);
}

MarkupHighlightState MarkupEditorBridge::GetHighlight() const
{
    MarkupHighlightState highlight;
    highlight.Hovered = m_HoveredRow;
    if (const SceneViewController* view = GetSceneView())
    {
        highlight.Hovered = MarkupHoveredEntity(view->GetHoveredEntity(), m_HoveredRow);
        highlight.Selected = view->GetSelectedEntities();
    }
    return highlight;
}

void MarkupEditorBridge::HoverMarkup(ECS::EntityHandle entity)
{
    m_HoveredRow = entity;
    if (SceneViewController* view = m_SceneViewProvider ? m_SceneViewProvider() : nullptr)
        view->SetHoverEntity(entity, false, SceneViewController::HoverEntitySource::Panel);
}

void MarkupEditorBridge::AdoptMarkupsWithoutNotes(ECS::World& world)
{
    auto* service = MarkupECS::MarkupService::TryGet();
    if (!service)
        return;
    std::vector<ECS::EntityHandle> orphans;
    world.Query<ECS::Read<Components::Markup>>().Each([&](ECS::EntityHandle entity, const Components::Markup&) {
        if (!service->FindNotes(world, entity))
            orphans.push_back(entity);
    });
    if (orphans.empty())
        return;
    // A duplicate carries the original's scene tag until its first save: its viewer state
    // starts empty instead of being read under the original's tag, and is never written
    // under it.
    WorldState& state = StateOf(world);
    for (const ECS::EntityHandle entity : orphans)
    {
        (void)service->BeginMarkup(world, entity, CurrentAuthor(), Now());
        state.Read.insert(entity);
        if (std::string tag = SceneTagOf(world, entity); !tag.empty())
            state.CopiedTags[entity] = std::move(tag);
    }
}

const MarkupRegionFrameGround& MarkupEditorBridge::RegionFrameGroundOf(ECS::World& world, uint64 frame) const
{
    WorldState& state = StateOf(world);
    if (state.FrameGroundFrame != frame)
    {
        state.FrameGround = ReadMarkupRegionFrameGround(world);
        state.FrameGroundFrame = frame;
    }
    return state.FrameGround;
}

void MarkupEditorBridge::OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event)
{
    if (!event.world || !IsTouchingKind(event.kind) || !IsMarkupPart(event.componentType))
        return;
    auto* service = MarkupECS::MarkupService::TryGet();
    if (!service || !event.world->IsValid(event.entity) || !event.world->GetComponent<Components::Markup>(event.entity))
    {
        // No longer a mark-up: a Markup added to it again starts from a fresh baseline.
        StateOf(*event.world).Baselines.erase(event.entity);
        return;
    }
    if (event.componentType == ECS::GetComponentTypeId<Components::SplineComponent>() ||
        event.componentType == ECS::GetComponentTypeId<Components::MarkupRegion>())
        CloseOpenRegionOutline(*event.world, event.entity);
    const MarkupECS::MarkupNotes* notes = service->FindNotes(*event.world, event.entity);
    if (!notes)
    {
        (void)service->BeginMarkup(*event.world, event.entity, CurrentAuthor(), Now());
        return;
    }
    // An undo can shorten the thread below the length at the last look; the entries
    // written after it are then new to the viewer, so the boundary follows it down.
    WorldState& state = StateOf(*event.world);
    if (const auto seen = state.Seen.find(event.entity);
        seen != state.Seen.end() && seen->second.RevisionKnown && seen->second.EntryCount > notes->Entries.size())
        seen->second.EntryCount = notes->Entries.size();
    if (const uint8 changes = TakeEditChanges(*event.world, event.entity); changes != 0)
        (void)service->RecordEdit(*event.world, event.entity, changes, CurrentAuthor(), Now());
    else
        (void)service->Touch(*event.world, event.entity, CurrentAuthor(), Now());
}

void MarkupEditorBridge::OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& event)
{
    ++m_StructureVersion;
    if (event.world && IsTouchingKind(event.kind))
    {
        AdoptMarkupsWithoutNotes(*event.world);
        event.world->Query<ECS::Read<Components::MarkupRegion>>().Each(
            [world = event.world](ECS::EntityHandle entity, const Components::MarkupRegion&) {
                CloseOpenRegionOutline(*world, entity);
            });
    }
    if (event.world)
        CaptureBaselines(*event.world);
}

} // namespace GameEngine::Editor
