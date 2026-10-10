#include "Markups/MarkupRequests.h"

#include "Components/Hierarchy.h"
#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "DebugServer/DebugServerReply.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/ConvertMarkupShapeCommand.h"
#include "Markups/MarkupPathPort.h"
#include "Markups/MarkupRegionDisplay.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupNotesEditCommand.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRegionPort.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneViewController.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"
#include "UndoRedo/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using nlohmann::json;
using Components::Markup;
using Components::MarkupAuthor;
using Components::MarkupMemberMode;
using Components::MarkupRegion;
using Components::MarkupRegionMember;
using Components::MarkupVolume;
using Components::MarkupVolumeShape;
using MarkupECS::MarkupService;
using Mathematics::Vector2;

constexpr const char* kCreateMarkupLabel = "Create Mark-up";
constexpr const char* kUpdateMarkupLabel = "Update Mark-up";
constexpr const char* kTitleRefusal = "Missing title: name the place, for example \"Village center\"";
constexpr const char* kSizeRefusal = "halfExtents must be [x, y, z] in meters, all positive (a sphere's radius is x)";

// The geometry of a volume mark-up, read from its transform: a box's scale is its
// full extents, a sphere's scale.x its diameter.
struct VolumeGeometry
{
    MarkupVolumeShape Shape = MarkupVolumeShape::Box;
    Mathematics::Vector3 Center{};
    Mathematics::Vector3 HalfExtents{};
    Mathematics::Quaternion Rotation{};
    float Radius = 0.0f; // of the bounding sphere
};

json Refusal(const char* reason)
{
    return RefuseRequest(reason);
}

std::optional<json> CheckContext(const MarkupRequestContext& context)
{
    if (context.PlayMode)
        return Refusal("Exit play mode first: mark-ups are read and edited in the edit world");
    if (!context.World || !context.Bridge || !MarkupService::TryGet())
        return Refusal("No scene is open");
    return std::nullopt;
}

std::string Iso8601(int64 unixSeconds)
{
    const std::time_t time = static_cast<std::time_t>(unixSeconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

const char* AuthorName(MarkupAuthor author)
{
    return author == MarkupAuthor::Agent ? "Agent" : "User";
}

std::optional<MarkupAuthor> ParseAuthor(const json& value)
{
    if (value == "User")
        return MarkupAuthor::User;
    if (value == "Agent")
        return MarkupAuthor::Agent;
    return std::nullopt;
}

const char* ShapeName(MarkupVolumeShape shape)
{
    return shape == MarkupVolumeShape::Sphere ? "sphere" : "box";
}

std::string TagName(uint32 id)
{
    const MarkupECS::MarkupTag* tag = MarkupService::Get().GetTag(id);
    return tag ? tag->Name : std::string();
}

json Vector3Json(const Mathematics::Vector3& v)
{
    return json::array({v.x, v.y, v.z});
}

bool ReadVector3(const json& value, Mathematics::Vector3& out)
{
    if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() ||
        !value[2].is_number())
        return false;
    out = Mathematics::Vector3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
    return true;
}

VolumeGeometry ReadGeometry(const ECS::World& world, ECS::EntityHandle entity)
{
    VolumeGeometry geometry;
    if (const auto* volume = world.GetComponent<MarkupVolume>(entity))
        geometry.Shape = volume->Shape;
    if (const auto* transform = world.GetComponent<Components::Transform>(entity))
    {
        const Mathematics::Vector3 scale = transform->GetScale();
        geometry.Center = transform->GetPosition();
        geometry.Rotation = transform->GetRotation();
        if (geometry.Shape == MarkupVolumeShape::Sphere)
        {
            const float radius = scale.x * 0.5f;
            geometry.HalfExtents = Mathematics::Vector3(radius, radius, radius);
            geometry.Radius = radius;
        }
        else
        {
            geometry.HalfExtents = scale * 0.5f;
            geometry.Radius = std::sqrt(Mathematics::Vector3::Dot(geometry.HalfExtents, geometry.HalfExtents));
        }
    }
    return geometry;
}

// Which run of revisions an answer's `revision` belongs to: the world and its lifecycle
// reset generation. A reader whose epoch changed since its last answer (the scene was
// reopened or replaced) lists everything again instead of trusting `since`.
std::string Epoch(const ECS::World& world)
{
    return std::to_string(world.GetWorldId()) + "." + std::to_string(world.GetLifecycleResetGeneration());
}

// The thread's entries a reader sees: an unreadable slot a scene line held is skipped.
std::size_t ReadableEntryCount(const MarkupECS::MarkupNotes* notes)
{
    if (!notes)
        return 0;
    return static_cast<std::size_t>(std::count_if(notes->Entries.begin(), notes->Entries.end(), [](const auto& entry) {
        return entry.Kind != MarkupECS::MarkupEntryKind::Unreadable;
    }));
}

// A path's line as the Scene View draws it, on the ground; empty without a display.
std::span<const Mathematics::Vector3> PathDrapedLine(const MarkupRequestContext& context, ECS::EntityHandle path)
{
    const MarkupECS::MarkupRegionDisplayCache::Entry* display = ResolveMarkupDisplayNow(*context.World, *context.Bridge, path);
    return display ? std::span<const Mathematics::Vector3>(display->Ground.Outline) : std::span<const Mathematics::Vector3>();
}

// The sphere around a path's line on the ground (its knots without a display): what its row
// reports and markup_frame fits.
std::optional<MarkupWorldVolume> PathFrameVolume(const MarkupRequestContext& context, ECS::EntityHandle path)
{
    const std::vector<Mathematics::Vector3> knots = PathWorldPoints(*context.World, path);
    const std::span<const Mathematics::Vector3> draped = PathDrapedLine(context, path);
    const std::span<const Mathematics::Vector3> points = draped.empty() ? std::span<const Mathematics::Vector3>(knots) : draped;
    if (points.empty())
        return std::nullopt;
    Mathematics::Vector3 min = points.front();
    Mathematics::Vector3 max = min;
    for (const Mathematics::Vector3& point : points)
    {
        min = Mathematics::Vector3(std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z));
        max = Mathematics::Vector3(std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z));
    }
    MarkupWorldVolume volume;
    volume.Shape = MarkupVolumeShape::Sphere;
    volume.Center = (min + max) * 0.5f;
    volume.BoundingRadius = std::max((max - min).Length() * 0.5f, 1.0f);
    volume.HalfExtents = Mathematics::Vector3(volume.BoundingRadius, volume.BoundingRadius, volume.BoundingRadius);
    volume.TopY = max.y;
    return volume;
}

json BuildRow(const MarkupRequestContext& context, ECS::EntityHandle entity)
{
    const ECS::World& world = *context.World;
    const Markup& markup = *world.GetComponent<Markup>(entity);
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(world, entity);
    const VolumeGeometry geometry = ReadGeometry(world, entity);

    json tags = json::array();
    if (notes)
    {
        for (const uint32 tag : notes->Tags)
        {
            if (tag != MarkupService::kInvalidTag)
                tags.push_back(TagName(tag));
        }
    }
    const auto* name = world.GetComponent<Components::Name>(entity);
    const auto* sceneTag = world.GetComponent<Components::SceneEntityTag>(entity);
    Mathematics::Vector3 center = geometry.Center;
    float radius = geometry.Radius;
    const char* kind = world.GetComponent<MarkupVolume>(entity) ? "volume" : "unknown";
    if (world.GetComponent<MarkupRegion>(entity))
    {
        kind = "region";
        const std::optional<RegionExtent> extent = ReadRegionExtent(world, entity);
        center = extent ? extent->Center : geometry.Center;
        radius = extent ? extent->Radius : 0.0f;
    }
    else if (IsMarkupPath(world, entity))
    {
        kind = "path";
        const std::optional<MarkupWorldVolume> extent = PathFrameVolume(context, entity);
        center = extent ? extent->Center : geometry.Center;
        radius = extent ? extent->BoundingRadius : 0.0f;
    }
    return json{{"entityId", entity.id},
                {"tag", sceneTag ? std::string(sceneTag->View()) : std::string()},
                {"title", name ? std::string(name->View()) : std::string()},
                {"kind", kind},
                {"status", TagName(markup.Status)},
                {"tags", tags},
                {"author", AuthorName(markup.Author)},
                {"updatedBy", AuthorName(markup.UpdatedBy)},
                {"created", Iso8601(markup.CreatedUnix)},
                {"updated", Iso8601(markup.UpdatedUnix)},
                {"revision", markup.Revision},
                {"hidden", context.Bridge->IsHidden(world, entity)},
                {"hasUpdate", context.Bridge->HasUnseenUpdate(world, entity)},
                {"center", Vector3Json(center)},
                {"radius", radius},
                {"entryCount", ReadableEntryCount(notes)}};
}

// The mark-up `params.entityId` names, or a refusal naming the fix.
std::optional<json> ResolveMarkup(const MarkupRequestContext& context, const json& params,
                                  ECS::EntityHandle& outEntity)
{
    if (!params.contains("entityId") || !params["entityId"].is_number_unsigned() ||
        params["entityId"].get<uint64>() > std::numeric_limits<uint32>::max())
        return Refusal("Missing entityId: pass the entityId markup_list returned");
    outEntity = ECS::EntityHandle(params["entityId"].get<uint32>());
    if (!context.World->IsValid(outEntity) || !context.World->GetComponent<Markup>(outEntity))
        return Refusal("No mark-up has that entityId; call markup_list for the current ones");
    return std::nullopt;
}

// The author a write is attributed to: `params.author` when given, else the open scope's.
std::optional<json> ResolveAuthor(const MarkupRequestContext& context, const json& params, MarkupAuthor& outAuthor)
{
    outAuthor = context.Bridge->CurrentAuthor();
    if (!params.contains("author"))
        return std::nullopt;
    const std::optional<MarkupAuthor> author = ParseAuthor(params["author"]);
    if (!author)
        return Refusal("author must be \"User\" or \"Agent\"");
    outAuthor = *author;
    return std::nullopt;
}

std::optional<json> ResolveStatus(const json& value, uint32& outStatus)
{
    const MarkupService& service = MarkupService::Get();
    outStatus = value.is_string() ? service.FindTag(value.get<std::string>()) : MarkupService::kInvalidTag;
    if (!service.IsStatusTag(outStatus))
    {
        std::string names;
        for (uint32 id = 0; id < service.GetTagCount(); ++id)
        {
            if (service.IsStatusTag(id))
                names += (names.empty() ? "" : ", ") + service.GetTag(id)->Name;
        }
        return RefuseRequest("Unknown status; the project's statuses are: " + names);
    }
    return std::nullopt;
}

std::optional<json> ResolveShape(const json& value, MarkupVolumeShape& outShape)
{
    if (value == "box")
        outShape = MarkupVolumeShape::Box;
    else if (value == "sphere")
        outShape = MarkupVolumeShape::Sphere;
    else
        return Refusal("shape must be \"box\" or \"sphere\"");
    return std::nullopt;
}

std::optional<json> ReadColor(const json& value, float32 (&outColor)[4])
{
    if (!value.is_array() || value.size() != 4)
        return Refusal("color must be [r, g, b, a] in 0..1, or [0, 0, 0, 0] for the status color");
    for (size_t channel = 0; channel < 4; ++channel)
    {
        if (!value[channel].is_number())
            return Refusal("color must be [r, g, b, a] in 0..1, or [0, 0, 0, 0] for the status color");
        outColor[channel] = value[channel].get<float32>();
    }
    return std::nullopt;
}

// `params[key]` into `out` when present; false when it is present and not text.
bool ReadOptionalText(const json& params, const char* key, std::string& out)
{
    const auto it = params.find(key);
    if (it == params.end())
        return true;
    if (!it->is_string())
        return false;
    out = it->get<std::string>();
    return true;
}

// `params[key]` into `out` when present; false when it is present and not true or false.
bool ReadOptionalFlag(const json& params, const char* key, bool& out)
{
    const auto it = params.find(key);
    if (it == params.end())
        return true;
    if (!it->is_boolean())
        return false;
    out = it->get<bool>();
    return true;
}

// A volume has a size: a sphere's radius (x) is positive, and a box's every half extent.
bool HasSize(MarkupVolumeShape shape, const Mathematics::Vector3& halfExtents)
{
    return halfExtents.x > 0.0f &&
           (shape == MarkupVolumeShape::Sphere || (halfExtents.y > 0.0f && halfExtents.z > 0.0f));
}

// The transform a volume of `shape` centered at `center` with `halfExtents` has; a
// sphere takes its radius from halfExtents.x.
Components::Transform VolumeTransform(MarkupVolumeShape shape, const Mathematics::Vector3& center,
                                      const Mathematics::Vector3& halfExtents, const Mathematics::Quaternion& rotation)
{
    const Mathematics::Vector3 scale = shape == MarkupVolumeShape::Sphere
                                           ? Mathematics::Vector3(halfExtents.x, halfExtents.x, halfExtents.x) * 2.0f
                                           : halfExtents * 2.0f;
    return Components::Transform::FromTRS(center, rotation, scale);
}

std::optional<json> ReadRotation(const json& value, Mathematics::Quaternion& outRotation)
{
    if (!value.is_array() || value.size() != 4 ||
        !std::all_of(value.begin(), value.end(), [](const json& part) { return part.is_number(); }))
        return Refusal("rotation must be a quaternion [x, y, z, w] of numbers");
    outRotation = Mathematics::Quaternion(value[3].get<float>(), value[0].get<float>(), value[1].get<float>(),
                                          value[2].get<float>())
                      .Normalized();
    return std::nullopt;
}

// A change made outside the inspector, announced as set_component announces one, so open
// inspectors rebuild (UndoRedo kind) and the bridge stamps it.
template <typename T>
void NotifyChanged(const MarkupRequestContext& context, ECS::EntityHandle entity)
{
    if (context.Notifications)
        context.Notifications->NotifyComponentChange<T>(context.World, entity,
                                                        EditorChangeNotifications::ChangeKind::UndoRedo);
}

json EntryJson(const MarkupECS::MarkupEntry& entry)
{
    using MarkupECS::MarkupEntryKind;
    const char* kind = entry.Kind == MarkupEntryKind::Created        ? "created"
                       : entry.Kind == MarkupEntryKind::StatusChange ? "statusChange"
                       : entry.Kind == MarkupEntryKind::Edit         ? "edit"
                                                                     : "comment";
    json out{{"kind", kind}, {"author", AuthorName(entry.Author)}, {"time", Iso8601(entry.TimeUnix)},
             {"text", entry.Text}};
    if (entry.Kind == MarkupEntryKind::Created || entry.Kind == MarkupEntryKind::StatusChange)
        out["status"] = TagName(entry.Status);
    if (entry.Kind == MarkupEntryKind::Edit)
    {
        json changes = json::array();
        for (const MarkupECS::MarkupEditChange change : MarkupECS::kMarkupEditChangeOrder)
        {
            if ((entry.Changes & static_cast<uint8>(change)) != 0)
                changes.push_back(std::string(MarkupECS::MarkupEditChangeName(change)));
        }
        out["changes"] = std::move(changes);
        if (entry.ColorArgb != 0)
        {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%06X", static_cast<unsigned>(entry.ColorArgb & 0x00FFFFFFu));
            out["color"] = hex;
        }
    }
    return out;
}

// What markup_create adds: the entity with its components, stamped with its author, its
// time and its first revision, and its description. The shape's components are the caller's.
struct NewMarkup
{
    std::string Title;
    Markup Component;
    Components::Transform Transform;
    MarkupAuthor Author = MarkupAuthor::Agent;
    std::string Description;
};

// A path's parts as markup_create reads them.
struct NewPath
{
    std::vector<Mathematics::Vector3> Points;
    Spline::SplineType Type = Spline::SplineType::Linear;
};

// A region's parts as markup_create reads them.
struct NewRegion
{
    std::vector<Vector2> Knots;
    Spline::SplineType Type = Spline::SplineType::Linear;
    float32 ExtrudeHeight = 8.0f;
    std::vector<MarkupRegionMember> Members;
    std::vector<std::vector<Vector2>> Exclusions;
};

ECS::EntityHandle AddMarkupEntity(ECS::World& world, const NewMarkup& added, int64 nowUnix)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    if (!entity.IsValid())
        return entity;
    Components::Name name{};
    std::strncpy(name.value, added.Title.c_str(), sizeof(name.value) - 1);
    world.AddComponentImmediate(entity, added.Transform);
    world.AddComponentImmediate(entity, name);
    world.AddComponentImmediate(entity, added.Component);
    world.AddComponentImmediate(entity, NextHierarchyOrderAtBottom(&world));
    MarkupService& service = MarkupService::Get();
    (void)service.BeginMarkup(world, entity, added.Author, nowUnix);
    // The description it is created with is part of the creation, not an edit of it.
    service.EnsureNotes(world, entity).Description = added.Description;
    return entity;
}

// "<parent> exclusion <n>" for the smallest n from `from` that no mark-up in the world is titled,
// as it is stored: `parent` is cut so the title fits a Name with its suffix whole.
std::string FreeExclusionTitle(ECS::World& world, const std::string& parent, std::size_t& from)
{
    constexpr std::size_t kTitleCapacity = sizeof(Components::Name::value) - 1;
    for (;; ++from)
    {
        const std::string suffix = " exclusion " + std::to_string(from);
        const std::string title = parent.substr(0, kTitleCapacity - suffix.size()) + suffix;
        bool taken = false;
        world.Query<ECS::Read<Components::Markup>, ECS::Read<Components::Name>>().Each(
            [&taken, &title](ECS::EntityHandle, const Components::Markup&, const Components::Name& name) {
                taken = taken || name.View() == title;
            });
        if (!taken)
            return title;
    }
}

// One Exclude region per polygon, parented under `region` (placed by `regionWorld`), titled
// after it with the next index no mark-up's title uses, by the same author with the same status;
// the new entities are appended to `created` and returned as Exclude members.
std::vector<MarkupRegionMember> AddExclusionRegions(ECS::World& world, ECS::EntityHandle region,
                                                    const float32 (&regionWorld)[16], const NewMarkup& parent,
                                                    std::span<const std::vector<Vector2>> exclusions,
                                                    float32 extrudeHeight, int64 nowUnix,
                                                    std::vector<ECS::EntityHandle>& created)
{
    std::vector<MarkupRegionMember> members;
    std::size_t index = 1;
    for (std::size_t i = 0; i < exclusions.size(); ++i)
    {
        NewMarkup exclusion = parent;
        exclusion.Title = FreeExclusionTitle(world, parent.Title, index);
        exclusion.Description.clear();
        const RegionPlacement placement = NewRegionPlacement(exclusions[i], regionWorld, regionWorld[13]);
        exclusion.Transform = placement.Local;
        const ECS::EntityHandle entity = AddMarkupEntity(world, exclusion, nowUnix);
        if (!entity.IsValid())
            continue;
        world.AddComponentImmediate(entity, Components::Parent{region});
        AddRegionParts(world, entity, placement.World.matrix, exclusions[i], Spline::SplineType::Linear,
                       extrudeHeight);
        created.push_back(entity);
        members.push_back({entity, MarkupMemberMode::Exclude});
    }
    return members;
}

// The create's redo: the command brings back the stamped component and the notes revive
// with the handle; the redo itself is an edit by whoever asked for it, so it is stamped
// with the bridge's author then, and a reader's `since` finds the mark-up again.
std::function<void()> StampRedoOfCreate(MarkupEditorBridge* bridge, ECS::World* world, ECS::EntityHandle entity)
{
    return [bridge, world, entity]() {
        (void)MarkupService::Get().Touch(*world, entity, bridge->CurrentAuthor(), bridge->Now());
    };
}

json SceneRevision(const ECS::World& world, json answer)
{
    answer["revision"] = MarkupService::Get().GetRevision(world);
    answer["epoch"] = Epoch(world);
    return answer;
}

// A region's parameters (outline, type, extrudeHeight, members, exclusions), all read before
// anything is written.
std::optional<json> ReadNewRegion(const MarkupRequestContext& context, const json& params, NewRegion& out)
{
    if (!params.contains("outline"))
        return Refusal("Missing outline: pass [[x, z], ...] in meters, 3 to 256 points in order around the area");
    if (auto refusal = ReadRegionOutline(params["outline"], out.Knots))
        return RefuseRequest(*refusal);
    if (params.contains("type"))
    {
        if (auto refusal = ReadRegionType(params["type"], out.Type))
            return RefuseRequest(*refusal);
    }
    if (params.contains("extrudeHeight"))
    {
        if (auto refusal = ReadExtrudeHeight(params["extrudeHeight"], out.ExtrudeHeight))
            return RefuseRequest(*refusal);
    }
    if (params.contains("members"))
    {
        if (auto refusal = ReadRegionMembers(*context.World, ECS::EntityHandle{}, params["members"], out.Members))
            return RefuseRequest(*refusal);
    }
    if (params.contains("exclusions"))
    {
        if (auto refusal = ReadExclusions(params["exclusions"], out.Exclusions))
            return RefuseRequest(*refusal);
    }
    if (out.Members.size() + out.Exclusions.size() > Components::kMaxRegionMembers)
        return RefuseRequest("A region holds at most 32 members; members and exclusions together come to " +
                             std::to_string(out.Members.size() + out.Exclusions.size()));
    return std::nullopt;
}

// Adds the region `added` describes with its exclusions: every entity it creates is appended
// to `created`, the region first.
void AddRegionMarkup(ECS::World& world, const NewMarkup& added, const NewRegion& region, int64 nowUnix,
                     std::vector<ECS::EntityHandle>& created)
{
    const ECS::EntityHandle entity = AddMarkupEntity(world, added, nowUnix);
    if (!entity.IsValid())
        return;
    created.push_back(entity);
    AddRegionParts(world, entity, added.Transform.matrix, region.Knots, region.Type, region.ExtrudeHeight);
    std::vector<MarkupRegionMember> members = region.Members;
    const std::vector<MarkupRegionMember> exclusions = AddExclusionRegions(
        world, entity, added.Transform.matrix, added, region.Exclusions, region.ExtrudeHeight, nowUnix, created);
    members.insert(members.end(), exclusions.begin(), exclusions.end());
    SetRegionMembers(world, entity, members);
}

// Adds the mark-up `added` describes: the region `region` describes with its exclusions, the path
// `path` describes, or a volume of `shape` when both are null. Every entity it creates is
// appended to `created`, the mark-up first.
void AddNewMarkup(ECS::World& world, const NewMarkup& added, const NewRegion* region, const NewPath* path,
                  MarkupVolumeShape shape, int64 nowUnix, std::vector<ECS::EntityHandle>& created)
{
    if (region)
    {
        AddRegionMarkup(world, added, *region, nowUnix, created);
        return;
    }
    const ECS::EntityHandle entity = AddMarkupEntity(world, added, nowUnix);
    if (!entity.IsValid())
        return;
    if (path)
        AddPathParts(world, entity, added.Transform.matrix, path->Points, path->Type);
    else
        world.AddComponentImmediate(entity, MarkupVolume{shape, {}});
    created.push_back(entity);
}

} // namespace

json ListMarkups(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;

    std::optional<uint32> status;
    if (params.contains("status"))
    {
        uint32 id = 0;
        if (const auto refusal = ResolveStatus(params["status"], id))
            return *refusal;
        status = id;
    }
    std::optional<MarkupAuthor> author;
    if (params.contains("author") && !(author = ParseAuthor(params["author"])))
        return Refusal("author must be \"User\" or \"Agent\"");
    std::optional<MarkupAuthor> changedBy;
    if (params.contains("changedBy") && !(changedBy = ParseAuthor(params["changedBy"])))
        return Refusal("changedBy must be \"User\" or \"Agent\"");
    if (params.contains("since") && !params["since"].is_number_unsigned())
        return Refusal("since must be a revision a previous answer returned (a whole number)");
    const bool hasSince = params.contains("since");
    const uint32 since = params.value("since", 0u);
    bool includeHidden = true;
    if (!ReadOptionalFlag(params, "includeHidden", includeHidden))
        return Refusal("includeHidden must be true or false");

    std::vector<ECS::EntityHandle> markups = MarkupService::Get().GetMarkups(*context.World);
    const ECS::World& world = *context.World;
    std::erase_if(markups, [&](ECS::EntityHandle entity) {
        const Markup& markup = *world.GetComponent<Markup>(entity);
        return (status && markup.Status != *status) || (author && markup.Author != *author) ||
               (changedBy && markup.UpdatedBy != *changedBy) || (hasSince && markup.Revision <= since) ||
               (!includeHidden && context.Bridge->IsHidden(world, entity));
    });
    std::sort(markups.begin(), markups.end(), [&](ECS::EntityHandle a, ECS::EntityHandle b) {
        const Markup& left = *world.GetComponent<Markup>(a);
        const Markup& right = *world.GetComponent<Markup>(b);
        if (left.UpdatedUnix != right.UpdatedUnix)
            return left.UpdatedUnix > right.UpdatedUnix;
        return left.Revision > right.Revision;
    });

    json rows = json::array();
    for (const ECS::EntityHandle entity : markups)
        rows.push_back(BuildRow(context, entity));
    return json{{"revision", MarkupService::Get().GetRevision(world)}, {"epoch", Epoch(world)}, {"markups", rows}};
}

namespace
{

struct RegionGroundRange
{
    float32 Min = 0.0f;
    float32 Max = 0.0f;
};

// The lowest and highest heights the region stands on: its display's ground (its walls' feet and
// its lid), from the Scene View's cache, built there now for a region no view has drawn.
std::optional<RegionGroundRange> ReadRegionGroundRange(const MarkupRequestContext& context, ECS::EntityHandle region)
{
    const MarkupECS::MarkupRegionDisplayCache::Entry* display = ResolveMarkupDisplayNow(*context.World, *context.Bridge, region);
    if (!display)
        return std::nullopt;
    RegionGroundRange range{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::lowest()};
    for (const std::vector<Mathematics::Vector3>* points : {&display->Ground.Outline, &display->Ground.LidPoints})
    {
        for (const Mathematics::Vector3& point : *points)
        {
            range.Min = std::min(range.Min, point.y);
            range.Max = std::max(range.Max, point.y);
        }
    }
    return range;
}

} // namespace

json GetMarkup(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;
    ECS::EntityHandle entity{};
    if (const auto refusal = ResolveMarkup(context, params, entity))
        return *refusal;

    json row = BuildRow(context, entity);
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(*context.World, entity);
    row["description"] = notes ? notes->Description : std::string();
    json entries = json::array();
    if (notes)
    {
        for (const MarkupECS::MarkupEntry& entry : notes->Entries)
        {
            if (entry.Kind != MarkupECS::MarkupEntryKind::Unreadable)
                entries.push_back(EntryJson(entry));
        }
    }
    row["entries"] = entries;

    if (context.World->GetComponent<MarkupRegion>(entity))
    {
        row["shape"] = RegionShapeJson(*context.World, entity);
        if (const std::optional<RegionGroundRange> range = ReadRegionGroundRange(context, entity))
            row["shape"]["groundRange"] = json{{"min", range->Min}, {"max", range->Max}};
        return SceneRevision(*context.World, json{{"markup", row}});
    }
    if (IsMarkupPath(*context.World, entity))
    {
        row["shape"] = PathShapeJson(*context.World, entity, PathDrapedLine(context, entity));
        return SceneRevision(*context.World, json{{"markup", row}});
    }
    const VolumeGeometry geometry = ReadGeometry(*context.World, entity);
    const glm::quat& rotation = geometry.Rotation.GetGLM();
    row["shape"] = json{{"shape", ShapeName(geometry.Shape)},
                        {"center", Vector3Json(geometry.Center)},
                        {"halfExtents", Vector3Json(geometry.HalfExtents)},
                        {"rotation", json::array({rotation.x, rotation.y, rotation.z, rotation.w})}};
    return SceneRevision(*context.World, json{{"markup", row}});
}

json CreateMarkup(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;

    NewMarkup added;
    if (!ReadOptionalText(params, "title", added.Title) || added.Title.empty())
        return Refusal(kTitleRefusal);
    std::string kind = "volume";
    if (!ReadOptionalText(params, "kind", kind) || (kind != "volume" && kind != "region" && kind != "path"))
        return Refusal("kind must be \"volume\" (a box or sphere: a place), \"region\" (an outline: an area) or "
                       "\"path\" (points along a way: a river, a road)");

    MarkupVolumeShape shape = MarkupVolumeShape::Box;
    NewRegion region;
    NewPath path;
    if (kind == "path")
    {
        if (!params.contains("points"))
            return Refusal("Missing points: pass [[x, y, z], ...] in meters, 2 to 256 points in order along the way");
        if (auto refusal = ReadPathPoints(params["points"], path.Points))
            return RefuseRequest(*refusal);
        StandPathOnGround(*context.World, path.Points);
        if (params.contains("type"))
        {
            if (auto refusal = ReadRegionType(params["type"], path.Type))
                return RefuseRequest(*refusal);
        }
        added.Transform = NewPathPlacement(path.Points);
    }
    else if (kind == "region")
    {
        if (const auto refusal = ReadNewRegion(context, params, region))
            return *refusal;
        const float32 groundY =
            MarkupTerrainHeightAt(*context.World, MarkupECS::RegionLabelPoint(region.Knots)).value_or(0.0f);
        added.Transform = NewRegionPlacement(region.Knots, Components::WorldTransform{}.matrix, groundY).Local;
    }
    else
    {
        if (params.contains("shape"))
        {
            if (const auto refusal = ResolveShape(params["shape"], shape))
                return *refusal;
        }
        Mathematics::Vector3 center{};
        Mathematics::Vector3 halfExtents{};
        if (!params.contains("center") || !ReadVector3(params["center"], center))
            return Refusal("Missing center: pass [x, y, z] in meters");
        if (!params.contains("halfExtents") || !ReadVector3(params["halfExtents"], halfExtents) ||
            !HasSize(shape, halfExtents))
            return Refusal(kSizeRefusal);
        Mathematics::Quaternion rotation{};
        if (params.contains("rotation"))
        {
            if (const auto refusal = ReadRotation(params["rotation"], rotation))
                return *refusal;
        }
        added.Transform = VolumeTransform(shape, center, halfExtents, rotation);
    }

    added.Component.Status = Components::kMarkupStatusProposed;
    if (params.contains("status"))
    {
        if (const auto refusal = ResolveStatus(params["status"], added.Component.Status))
            return *refusal;
    }
    if (params.contains("color"))
    {
        if (const auto refusal = ReadColor(params["color"], added.Component.Color))
            return *refusal;
    }
    if (const auto refusal = ResolveAuthor(context, params, added.Author))
        return *refusal;
    if (!ReadOptionalText(params, "description", added.Description))
        return Refusal("description must be text");

    MarkupEditorBridge::AttributionScope scope(*context.Bridge, added.Author);
    ECS::World& world = *context.World;
    const int64 now = context.Bridge->Now();
    std::vector<ECS::EntityHandle> created;
    if (context.Undo)
    {
        CommitGenericEdit(world, *context.Undo, kCreateMarkupLabel, [&]() {
            AddNewMarkup(world, added, kind == "region" ? &region : nullptr, kind == "path" ? &path : nullptr, shape,
                         now, created);
            if (created.empty())
                return;
            // Created and stamped before the command is built, so it is committed as already
            // applied and captures the stamped bytes: undo removes the entities, redo revives
            // the same handles (which find their notes and outlines again) and stamps the redo.
            context.Undo->CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
                kCreateMarkupLabel, &world, context.Notifications, created,
                StampRedoOfCreate(context.Bridge, &world, created.front()), nullptr));
        });
    }
    else
    {
        AddNewMarkup(world, added, kind == "region" ? &region : nullptr, kind == "path" ? &path : nullptr, shape, now,
                     created);
    }
    if (created.empty())
        return Refusal("The world refused a new entity");

    NotifyWorldStructure(context.Notifications, &world);
    const ECS::EntityHandle entity = created.front();
    json answer{{"entityId", entity.id}, {"markup", BuildRow(context, entity)}};
    if (created.size() > 1)
    {
        json exclusions = json::array();
        for (std::size_t i = 1; i < created.size(); ++i)
            exclusions.push_back(created[i].id);
        answer["exclusionIds"] = std::move(exclusions);
    }
    return SceneRevision(world, std::move(answer));
}

namespace
{

constexpr const char* kExclusionsLabel = "Add Mark-up Exclusions";

// The writes of one markup_update that every mark-up takes (its title, color, status,
// description, tags and a volume's shape), validated before the first is made. `Geometry` is the
// volume's geometry with the request's center, halfExtents and rotation applied.
struct MarkupUpdate
{
    std::optional<std::string> Title;
    bool HasColor = false;
    float32 Color[4] = {};
    std::optional<uint32> Status;
    std::optional<std::string> Description;
    std::optional<std::vector<std::string>> Tags;
    std::optional<MarkupVolumeShape> Shape;
    bool Moves = false;
    VolumeGeometry Geometry;

    bool Any() const { return Title || HasColor || Status || Description || Tags || Moves; }
};

// Makes the writes of `update` on behalf of `author`, each announced.
void ApplyMarkupUpdate(const MarkupRequestContext& context, ECS::EntityHandle entity, MarkupAuthor author,
                       const MarkupUpdate& update, int64 nowUnix)
{
    ECS::World& world = *context.World;
    MarkupService& service = MarkupService::Get();
    if (update.Title)
    {
        Components::Name name{};
        std::strncpy(name.value, update.Title->c_str(), sizeof(name.value) - 1);
        world.AddComponentImmediate(entity, name);
        NotifyChanged<Components::Name>(context, entity);
    }
    if (update.HasColor)
    {
        Markup* markup = world.GetComponentForWrite<Markup>(entity);
        std::copy(std::begin(update.Color), std::end(update.Color), std::begin(markup->Color));
        NotifyChanged<Markup>(context, entity);
    }
    if (update.Status && *update.Status != world.GetComponent<Markup>(entity)->Status)
    {
        (void)service.SetStatus(world, entity, *update.Status, author, nowUnix);
        NotifyChanged<Markup>(context, entity);
    }
    if (update.Description)
    {
        (void)service.SetDescription(world, entity, author, nowUnix, *update.Description);
        NotifyChanged<Markup>(context, entity);
    }
    if (update.Tags)
    {
        constexpr float32 kNewTagColor[4] = {0.6f, 0.6f, 0.6f, 1.0f};
        std::vector<uint32> tags;
        for (const std::string& name : *update.Tags)
            tags.push_back(service.AddTag(name, {}, kNewTagColor));
        (void)service.SetTags(world, entity, tags, author, nowUnix);
        NotifyChanged<Markup>(context, entity);
    }
    if (update.Moves)
    {
        if (update.Shape)
            world.AddComponentImmediate(entity, MarkupVolume{*update.Shape, {}});
        const MarkupVolumeShape current = world.GetComponent<MarkupVolume>(entity)->Shape;
        const VolumeGeometry& geometry = update.Geometry;
        world.AddComponentImmediate(entity, VolumeTransform(current, geometry.Center, geometry.HalfExtents, geometry.Rotation));
        NotifyChanged<Components::Transform>(context, entity);
    }
}

// `update` recorded (with `context.Undo`) as the mark-up's notes and Markup component
// (CommitMarkupNotesEdit) inside its components (CommitEntityComponentsEdit), so an undo puts
// the components back first and the notes last, which leaves the thread as it was before.
// Tags the update added to the vocabulary are saved, and an undo removes them again.
void CommitMarkupUpdate(const MarkupRequestContext& context, ECS::EntityHandle entity, MarkupAuthor author,
                        const MarkupUpdate& update, int64 nowUnix)
{
    ECS::World& world = *context.World;
    MarkupEditorBridge* bridge = context.Bridge;
    (void)CommitEntityComponentsEdit(world, entity, context.Undo, context.Notifications, kUpdateMarkupLabel, [&]() {
        return CommitMarkupNotesEdit(
            world, entity, context.Undo, context.Notifications, kUpdateMarkupLabel,
            [&]() {
                ApplyMarkupUpdate(context, entity, author, update, nowUnix);
                return true;
            },
            [bridge]() { bridge->SaveVocabulary(); });
    });
}

// Runs `write` as a snapshot edit of `target` (with `context.Undo`), announced by the edit;
// without an undo stack, or when the target cannot be captured, it runs directly and is
// announced as an UndoRedo change.
void RecordRegionWrite(const MarkupRequestContext& context, UndoRedoService::SnapshotTarget target,
                       const std::function<void()>& write)
{
    UndoRedoService::InteractiveEdit edit;
    if (context.Undo)
        edit = context.Undo->BeginInteractiveEdit(kUpdateMarkupLabel, target);
    if (!edit)
    {
        write();
        if (target.Notify)
            target.Notify(EditorChangeNotifications::ChangeKind::UndoRedo);
        return;
    }
    edit.Preview(write);
    edit.Commit();
}

// What markup_update changes on a region, or converts a volume into (a region or a path), read
// before anything is written.
struct RegionUpdate
{
    bool Convert = false; // a volume becomes a region (kind: "region")
    std::optional<ConvertedPath> ToPath; // a volume becomes a path (kind: "path" with points)
    std::optional<std::vector<Vector2>> Knots;
    std::optional<Spline::SplineType> Type;
    std::optional<float32> ExtrudeHeight;
    std::optional<std::vector<MarkupRegionMember>> Members;
    std::vector<std::vector<Vector2>> Exclusions;

    bool Any() const
    {
        return Convert || ToPath || Knots || Type || ExtrudeHeight || Members || !Exclusions.empty();
    }
};

bool HasRegionParameter(const json& params)
{
    return params.contains("outline") || params.contains("type") || params.contains("extrudeHeight") ||
           params.contains("members") || params.contains("exclusions");
}

bool HasVolumeParameter(const json& params)
{
    return params.contains("shape") || params.contains("center") || params.contains("halfExtents") ||
           params.contains("rotation");
}

std::optional<json> ReadRegionUpdate(const MarkupRequestContext& context, const json& params, ECS::EntityHandle entity,
                                     RegionUpdate& out)
{
    const ECS::World& world = *context.World;
    const bool isRegion = world.GetComponent<MarkupRegion>(entity) != nullptr;
    if (params.contains("kind") && params["kind"] == "path")
    {
        if (isRegion)
            return Refusal("A region does not become a path; the conversion is from a box or a sphere");
        if (HasVolumeParameter(params) || params.contains("outline") || params.contains("extrudeHeight") ||
            params.contains("members") || params.contains("exclusions"))
            return Refusal("A path's shape is its points: pass points and type, not a volume's or a region's fields");
        if (!params.contains("points"))
            return Refusal("Converting to a path takes points: [[x, y, z], ...] in meters, 2 to 256 along the way");
        ConvertedPath& path = out.ToPath.emplace();
        if (auto refusal = ReadPathPoints(params["points"], path.Points))
            return RefuseRequest(*refusal);
        if (params.contains("type"))
        {
            if (auto refusal = ReadRegionType(params["type"], path.Type))
                return RefuseRequest(*refusal);
        }
        return std::nullopt;
    }
    if (params.contains("points"))
        return Refusal("points belong to a path: pass kind: \"path\" to convert this volume");
    if (params.contains("kind"))
    {
        if (params["kind"] == "region")
            out.Convert = !isRegion;
        else if (params["kind"] == "volume" && isRegion)
            return Refusal("A region does not become a volume again; the conversion is one way");
        else if (params["kind"] != "volume")
            return Refusal("kind must be \"region\" or \"path\" to convert a volume; a mark-up keeps its kind "
                           "otherwise");
    }
    if (params.contains("closed") && params["closed"] != true)
        return Refusal("A region is always closed; closed: false is refused");
    const bool regionAfter = isRegion || out.Convert;
    if (!regionAfter && HasRegionParameter(params))
        return Refusal("outline, type, extrudeHeight, members and exclusions belong to a region; pass kind: "
                       "\"region\" to convert this volume");
    if (regionAfter && HasVolumeParameter(params))
        return Refusal("A region's shape is its outline: pass outline, not shape, center, halfExtents or rotation");
    if (!regionAfter)
        return std::nullopt;

    if (params.contains("outline"))
    {
        out.Knots.emplace();
        if (auto refusal = ReadRegionOutline(params["outline"], *out.Knots))
            return RefuseRequest(*refusal);
    }
    if (params.contains("type"))
    {
        out.Type.emplace();
        if (auto refusal = ReadRegionType(params["type"], *out.Type))
            return RefuseRequest(*refusal);
    }
    if (params.contains("extrudeHeight"))
    {
        out.ExtrudeHeight.emplace();
        if (auto refusal = ReadExtrudeHeight(params["extrudeHeight"], *out.ExtrudeHeight))
            return RefuseRequest(*refusal);
    }
    if (out.Convert && !out.Knots)
    {
        const auto box = BoxAsRegion(world, entity);
        if (!box)
            return Refusal("Converting a sphere takes an outline: pass outline [[x, z], ...]");
        out.Knots = box->Knots;
        if (!out.ExtrudeHeight)
            out.ExtrudeHeight = box->ExtrudeHeight;
    }
    if (params.contains("members"))
    {
        out.Members.emplace();
        if (auto refusal = ReadRegionMembers(world, entity, params["members"], *out.Members))
            return RefuseRequest(*refusal);
    }
    if (params.contains("exclusions"))
    {
        if (auto refusal = ReadExclusions(params["exclusions"], out.Exclusions))
            return RefuseRequest(*refusal);
    }
    const auto* region = world.GetComponent<MarkupRegion>(entity);
    const std::size_t kept = out.Members ? out.Members->size() : (region ? region->MemberCount : 0u);
    if (kept + out.Exclusions.size() > Components::kMaxRegionMembers)
        return RefuseRequest("A region holds at most 32 members; members and exclusions together come to " +
                             std::to_string(kept + out.Exclusions.size()));
    return std::nullopt;
}

std::vector<MarkupRegionMember> CurrentMembers(const ECS::World& world, ECS::EntityHandle region)
{
    const auto* component = world.GetComponent<MarkupRegion>(region);
    if (!component)
        return {};
    return std::vector<MarkupRegionMember>(component->Members,
                                           component->Members + std::min(component->MemberCount,
                                                                         Components::kMaxRegionMembers));
}

// The new Exclude regions under `region` and their place in its member list, as one undo step:
// undo detaches them and removes them, redo brings both back.
void AddExclusionsToRegion(const MarkupRequestContext& context, ECS::EntityHandle region, MarkupAuthor author,
                           std::span<const std::vector<Vector2>> exclusions, int64 nowUnix)
{
    ECS::World& world = *context.World;
    NewMarkup parent;
    parent.Title = MarkupTitle(world, region);
    parent.Component = *world.GetComponent<Markup>(region);
    parent.Component.Color[0] = parent.Component.Color[1] = parent.Component.Color[2] = parent.Component.Color[3] = 0.0f;
    parent.Author = author;
    const float32 height = world.GetComponent<MarkupRegion>(region)->ExtrudeHeight;
    Components::WorldTransform regionWorld{};
    std::copy_n(MarkupPlacement(world, region).Data(), 16, regionWorld.matrix);
    std::vector<ECS::EntityHandle> created;
    std::vector<MarkupRegionMember> members = CurrentMembers(world, region);
    const auto attach = [&]() {
        const std::vector<MarkupRegionMember> added =
            AddExclusionRegions(world, region, regionWorld.matrix, parent, exclusions, height, nowUnix, created);
        members.insert(members.end(), added.begin(), added.end());
    };
    if (!context.Undo)
    {
        attach();
        SetRegionMembers(world, region, members);
        return;
    }
    CommitGenericEdit(world, *context.Undo, kExclusionsLabel, [&]() {
        attach();
        if (created.empty())
            return;
        context.Undo->CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
            kExclusionsLabel, &world, context.Notifications, created, nullptr, nullptr));
        UndoRedoService::InteractiveEdit edit = context.Undo->BeginInteractiveEdit(
            kExclusionsLabel, MultiEntityUndo::MakeComponentSnapshotTarget(
                                  &world, {region}, ECS::GetComponentTypeId<MarkupRegion>(), context.Notifications,
                                  kExclusionsLabel));
        edit.Preview([&]() { SetRegionMembers(world, region, members); });
        edit.Commit();
    });
}

// Writes what `update` holds, after every field was read.
void ApplyRegionUpdate(const MarkupRequestContext& context, ECS::EntityHandle entity, const RegionUpdate& update,
                       MarkupAuthor author, int64 nowUnix)
{
    ECS::World& world = *context.World;
    if (update.ToPath)
    {
        ConvertedPath path = *update.ToPath;
        StandPathOnGround(world, path.Points);
        auto command = std::make_unique<ConvertMarkupShapeCommand>(kConvertToPathLabel, world, entity,
                                                                   context.Notifications, std::move(path));
        if (context.Undo)
            context.Undo->Execute(std::move(command));
        else
            command->Do();
    }
    else if (update.Convert)
    {
        auto command = std::make_unique<ConvertMarkupShapeCommand>(
            kConvertToRegionLabel, world, entity, context.Notifications,
            ConvertedRegion{*update.Knots, update.Type.value_or(Spline::SplineType::Linear),
                            update.ExtrudeHeight.value_or(8.0f)});
        if (context.Undo)
            context.Undo->Execute(std::move(command));
        else
            command->Do();
    }
    else if (const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
             spline && (update.Knots || update.Type))
    {
        const SplineECS::SplineHandle handle(spline->SplineDataIndex, spline->SplineDataGeneration);
        RecordRegionWrite(context,
                          SplineUndo::MakeSplineEditableSnapshotTarget(SplineECS::SplineService::TryGet(), handle,
                                                                       &world, entity, context.Notifications,
                                                                       kUpdateMarkupLabel),
                          [&]() {
                              RewriteRegionOutline(world, entity,
                                                   update.Knots ? std::span<const Vector2>(*update.Knots)
                                                                : std::span<const Vector2>(),
                                                   update.Type);
                          });
    }
    // After a conversion, so its members join the region the conversion made and an undo
    // followed by a redo brings them back with it.
    if ((update.ExtrudeHeight && !update.Convert) || update.Members)
    {
        RecordRegionWrite(context,
                          MultiEntityUndo::MakeComponentSnapshotTarget(&world, {entity},
                                                                       ECS::GetComponentTypeId<MarkupRegion>(),
                                                                       context.Notifications, kUpdateMarkupLabel),
                          [&]() {
                              if (update.ExtrudeHeight && !update.Convert)
                                  world.GetComponentForWrite<MarkupRegion>(entity)->ExtrudeHeight = *update.ExtrudeHeight;
                              if (update.Members)
                                  SetRegionMembers(world, entity, *update.Members);
                          });
    }
    if (!update.Exclusions.empty())
    {
        AddExclusionsToRegion(context, entity, author, update.Exclusions, nowUnix);
        NotifyWorldStructure(context.Notifications, &world);
    }
}

// The whole update: the region's writes, then the rest. Each records its own commands (with
// `context.Undo`); the caller makes them one step.
void WriteMarkupUpdate(const MarkupRequestContext& context, ECS::EntityHandle entity, MarkupAuthor author,
                       const MarkupUpdate& update, const RegionUpdate& region)
{
    const int64 now = context.Bridge->Now();
    if (region.Any())
        ApplyRegionUpdate(context, entity, region, author, now);
    if (update.Any())
        CommitMarkupUpdate(context, entity, author, update, now);
}

} // namespace

json UpdateMarkup(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;
    ECS::EntityHandle entity{};
    if (const auto refusal = ResolveMarkup(context, params, entity))
        return *refusal;
    MarkupAuthor author = MarkupAuthor::Agent;
    if (const auto refusal = ResolveAuthor(context, params, author))
        return *refusal;

    // Validate everything before the first write, so a bad field changes nothing.
    if (IsMarkupPath(*context.World, entity) &&
        (params.contains("kind") || params.contains("points") || HasRegionParameter(params) ||
         HasVolumeParameter(params)))
        return Refusal("A path keeps its kind and its points here: move its points with Edit path in the "
                       "inspector, or create the path again with markup_create");
    RegionUpdate region;
    if (const auto refusal = ReadRegionUpdate(context, params, entity, region))
        return *refusal;
    MarkupUpdate update;
    if (params.contains("status"))
    {
        uint32 id = 0;
        if (const auto refusal = ResolveStatus(params["status"], id))
            return *refusal;
        update.Status = id;
    }
    if (params.contains("color"))
    {
        if (const auto refusal = ReadColor(params["color"], update.Color))
            return *refusal;
        update.HasColor = true;
    }
    if (params.contains("shape"))
    {
        MarkupVolumeShape value{};
        if (const auto refusal = ResolveShape(params["shape"], value))
            return *refusal;
        update.Shape = value;
    }
    VolumeGeometry& geometry = update.Geometry;
    geometry = ReadGeometry(*context.World, entity);
    update.Moves = params.contains("center") || params.contains("halfExtents") || params.contains("rotation") || update.Shape;
    if (params.contains("center") && !ReadVector3(params["center"], geometry.Center))
        return Refusal("center must be [x, y, z] in meters");
    if (params.contains("halfExtents") &&
        (!ReadVector3(params["halfExtents"], geometry.HalfExtents) || !HasSize(update.Shape.value_or(geometry.Shape), geometry.HalfExtents)))
        return Refusal(kSizeRefusal);
    if (params.contains("rotation"))
    {
        if (const auto refusal = ReadRotation(params["rotation"], geometry.Rotation))
            return *refusal;
    }
    if (params.contains("title"))
    {
        update.Title.emplace();
        if (!ReadOptionalText(params, "title", *update.Title) || update.Title->empty())
            return Refusal(kTitleRefusal);
    }
    if (params.contains("description"))
    {
        update.Description.emplace();
        if (!ReadOptionalText(params, "description", *update.Description))
            return Refusal("description must be text");
    }
    std::optional<bool> hidden;
    if (params.contains("hidden"))
    {
        hidden.emplace();
        if (!ReadOptionalFlag(params, "hidden", *hidden))
            return Refusal("hidden must be true or false");
    }
    if (params.contains("tags"))
    {
        if (!params["tags"].is_array())
            return Refusal("tags must be an array of names");
        update.Tags.emplace();
        for (const json& name : params["tags"])
        {
            if (!name.is_string() || name.get<std::string>().empty())
                return Refusal("tags must be an array of names");
            const uint32 id = MarkupService::Get().FindTag(name.get<std::string>());
            if (id != MarkupService::kInvalidTag && MarkupService::Get().IsStatusTag(id))
                return Refusal("A status is not a tag; set it with status");
            update.Tags->push_back(name.get<std::string>());
        }
    }

    // A port edit is one undo step: everything the update writes, hidden aside.
    MarkupEditorBridge::AttributionScope scope(*context.Bridge, author);
    ECS::World& world = *context.World;
    if (context.Undo && (update.Any() || region.Any()))
        CommitGenericEdit(world, *context.Undo, kUpdateMarkupLabel,
                          [&]() { WriteMarkupUpdate(context, entity, author, update, region); });
    else
        WriteMarkupUpdate(context, entity, author, update, region);
    // Hiding is the viewer's state, not a scene edit: no undo step.
    if (hidden)
        context.Bridge->SetHidden(world, {&entity, 1}, *hidden);

    return SceneRevision(world, json{{"markup", BuildRow(context, entity)}});
}

json CommentOnMarkup(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;
    ECS::EntityHandle entity{};
    if (const auto refusal = ResolveMarkup(context, params, entity))
        return *refusal;
    std::string text;
    if (!ReadOptionalText(params, "text", text) || text.empty())
        return Refusal("Missing text: the comment to add");
    MarkupAuthor author = MarkupAuthor::Agent;
    if (const auto refusal = ResolveAuthor(context, params, author))
        return *refusal;

    MarkupEditorBridge::AttributionScope scope(*context.Bridge, author);
    MarkupService& service = MarkupService::Get();
    (void)service.AddComment(*context.World, entity, author, context.Bridge->Now(), text);
    NotifyChanged<Markup>(context, entity);
    return SceneRevision(*context.World,
                         json{{"entry", EntryJson(service.FindNotes(*context.World, entity)->Entries.back())}});
}

namespace
{

// What markup_frame fits for a region: the sphere around its ring and its height, standing on
// the label point.
std::optional<MarkupWorldVolume> RegionFrameVolume(const ECS::World& world, ECS::EntityHandle region)
{
    const std::optional<RegionExtent> extent = ReadRegionExtent(world, region);
    if (!extent)
        return std::nullopt;
    const float32 halfHeight = 0.5f * world.GetComponent<MarkupRegion>(region)->ExtrudeHeight;
    MarkupWorldVolume volume;
    volume.Shape = MarkupVolumeShape::Sphere;
    volume.Center = extent->Center + Mathematics::Vector3(0.0f, halfHeight, 0.0f);
    volume.BoundingRadius = std::hypot(extent->Radius, halfHeight);
    volume.HalfExtents = Mathematics::Vector3(volume.BoundingRadius, volume.BoundingRadius, volume.BoundingRadius);
    volume.TopY = volume.Center.y + halfHeight;
    return volume;
}

// The most points one markup_contains answers for.
constexpr std::size_t kMaxContainsPoints = 8192;

} // namespace

json ContainsInMarkup(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;
    ECS::EntityHandle entity{};
    if (const auto refusal = ResolveMarkup(context, params, entity))
        return *refusal;
    if (!params.contains("points") || !params["points"].is_array())
        return Refusal("Missing points: pass [[x, z], ...] (or [x, y, z], y ignored) in meters");
    if (params["points"].size() > kMaxContainsPoints)
        return RefuseRequest("At most " + std::to_string(kMaxContainsPoints) + " points per call; split the rest");
    std::vector<Vector2> points;
    points.reserve(params["points"].size());
    for (const json& point : params["points"])
    {
        if (!point.is_array() || (point.size() != 2 && point.size() != 3) ||
            !std::all_of(point.begin(), point.end(), [](const json& part) { return part.is_number(); }))
            return Refusal("Each point is [x, z] (or [x, y, z], y ignored) in meters");
        points.emplace_back(point[0].get<float32>(), point[point.size() - 1].get<float32>());
    }

    const ECS::World& world = *context.World;
    std::optional<MarkupECS::MarkupRegionArea> area;
    std::optional<MarkupECS::MarkupFootprint> footprint;
    if (world.GetComponent<MarkupRegion>(entity))
        area = MarkupECS::MarkupRegionArea::Read(world, entity);
    else
        footprint = MarkupECS::ReadMarkupFootprint(world, entity);
    if (IsMarkupPath(world, entity))
        return Refusal("A path encloses no area: markup_contains tests regions, boxes and spheres");
    if (!area && !footprint)
        return Refusal("The mark-up covers no ground yet: a region needs an outline of three points or more");
    json inside = json::array();
    std::size_t count = 0;
    for (const Vector2& point : points)
    {
        const bool in = area ? area->Contains(point) : footprint->Contains(point);
        count += in ? 1 : 0;
        inside.push_back(in);
    }
    return SceneRevision(world, json{{"inside", std::move(inside)}, {"insideCount", count}});
}

json FrameMarkup(const MarkupRequestContext& context, const json& params, SceneViewCameraPose& outPose)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;
    ECS::EntityHandle entity{};
    if (const auto refusal = ResolveMarkup(context, params, entity))
        return *refusal;

    Mathematics::Vector3 direction(1.0f, -0.5f, 1.0f);
    if (params.contains("direction") && !ReadVector3(params["direction"], direction))
        return Refusal("direction must be [x, y, z], from the camera toward the mark-up");
    // The volume where it is in the world, as the Scene View draws it: a parented mark-up's
    // local position is not where it stands.
    const std::optional<MarkupWorldVolume> volume = context.World->GetComponent<MarkupRegion>(entity)
                                                        ? RegionFrameVolume(*context.World, entity)
                                                    : IsMarkupPath(*context.World, entity)
                                                        ? PathFrameVolume(context, entity)
                                                        : ReadMarkupWorldVolume(*context.World, entity);
    if (!volume)
        return Refusal("The mark-up has no shape to frame; a region needs an outline of three points or more");
    float distance = MarkupFrameDistance(*volume, context.FrameTanHalfFov);
    if (params.contains("distance"))
    {
        if (!params["distance"].is_number() || params["distance"].get<float>() <= 0.0f)
            return Refusal("distance must be a positive number of meters");
        distance = params["distance"].get<float>();
    }
    if (!ComputeLookAtPose(volume->Center, direction, distance, outPose))
        return Refusal("direction has no length");
    return SceneRevision(*context.World, json{{"position", json::array({outPose.Pos[0], outPose.Pos[1], outPose.Pos[2]})},
                                              {"target", Vector3Json(volume->Center)},
                                              {"yawDeg", outPose.YawDeg},
                                              {"pitchDeg", outPose.PitchDeg},
                                              {"distance", outPose.Distance}});
}

json SetMarkupsVisible(const MarkupRequestContext& context, const json& params)
{
    if (const auto refusal = CheckContext(context))
        return *refusal;
    if (!params.contains("visible") || !params["visible"].is_boolean())
        return Refusal("Missing visible: true to show, false to hide");
    const bool hidden = !params["visible"].get<bool>();

    bool all = false;
    if (!ReadOptionalFlag(params, "all", all))
        return Refusal("all must be true or false");
    std::vector<ECS::EntityHandle> targets;
    if (all)
    {
        targets = MarkupService::Get().GetMarkups(*context.World);
    }
    else
    {
        ECS::EntityHandle entity{};
        if (const auto refusal = ResolveMarkup(context, params, entity))
            return RefuseRequest("Pass entityId, or all: true");
        targets.push_back(entity);
    }

    context.Bridge->SetHidden(*context.World, targets, hidden);
    json states = json::array();
    for (const ECS::EntityHandle entity : targets)
        states.push_back(json{{"entityId", entity.id}, {"hidden", hidden}});
    return SceneRevision(*context.World, json{{"markups", states}});
}

} // namespace GameEngine::Editor
