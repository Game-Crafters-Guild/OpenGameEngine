#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Who wrote a mark-up, its last change, or an entry in its thread.
enum class MarkupAuthor : uint8
{
    User = 0,
    Agent = 1,
};

// Ids of the six status tags every project's tag vocabulary starts with
// (MarkupECS::MarkupService). A project may rename and recolor them and add its own.
inline constexpr uint32 kMarkupStatusProposed = 0;   // the agent drew it and asks
inline constexpr uint32 kMarkupStatusRequested = 1;  // the user asks for the work
inline constexpr uint32 kMarkupStatusInProgress = 2;
inline constexpr uint32 kMarkupStatusComplete = 3;
inline constexpr uint32 kMarkupStatusRevision = 4;   // the user asks again on finished work
inline constexpr uint32 kMarkupStatusProblem = 5;

// A mark-up: a note the user and the agent attach to a place in the world. The
// entity's Name is its title and its shape component (MarkupVolume) is the place;
// the description, the free tags and the thread live in MarkupService, keyed by
// the entity. A game export strips it with its shape and title; the entity stays.
// @ge-no-add        created by the Scene View mark-up tool and the agent, never by hand
// @ge-editor-only
struct Markup
{
    // The status: the id of a tag in the vocabulary's status group.
    uint32 Status = kMarkupStatusRequested;
    MarkupAuthor Author = MarkupAuthor::User;     // @ge-readonly
    MarkupAuthor UpdatedBy = MarkupAuthor::User;  // @ge-readonly
    uint8 _Pad[2] = {};  // @ge-hidden
    // RGBA, as the gizmo colors. All zero draws the status tag's color.
    float32 Color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // Unix seconds. An edit stamps UpdatedUnix, UpdatedBy and Revision through
    // MarkupService::Touch; a scene load restores them as saved.
    int64 CreatedUnix = 0;  // @ge-readonly
    int64 UpdatedUnix = 0;  // @ge-readonly
    // The scene's mark-up revision at this mark-up's last change; ordering, not time.
    uint32 Revision = 0;  // @ge-readonly
};

static_assert(std::is_trivially_copyable_v<Markup>, "Markup must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<Markup>, "Markup must be standard layout for ECS storage");

enum class MarkupVolumeShape : uint8
{
    Box = 0,
    Sphere = 1,
};

// The place a volume mark-up covers: the entity's transform scales, rotates and
// places a unit shape (a box's scale is its full extents, a sphere's scale.x its
// diameter), as for the other local volumes.
// @ge-no-add        part of a mark-up, created with it
// @ge-editor-only
struct MarkupVolume
{
    MarkupVolumeShape Shape = MarkupVolumeShape::Box;
    uint8 _Pad[3] = {};  // @ge-hidden
};

static_assert(std::is_trivially_copyable_v<MarkupVolume>, "MarkupVolume must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<MarkupVolume>, "MarkupVolume must be standard layout for ECS storage");

// How a member mark-up changes a region's area: an Include adds its footprint, an Exclude
// cuts it out.
enum class MarkupMemberMode : uint8
{
    Include = 0,
    Exclude = 1,
};

// One mark-up a region includes or excludes.
struct MarkupRegionMember
{
    // Another mark-up with a shape: a box, a sphere or a region, which contributes its base
    // outline only (its own members do not apply), so membership never recurses. A handle
    // that no longer names a live mark-up (the member was deleted; undo revives the same
    // handle) is skipped, and a save drops it.
    ECS::EntityHandle Entity{};
    MarkupMemberMode Mode = MarkupMemberMode::Include;
    uint8 _Pad[3] = {};  // @ge-hidden

    bool operator==(const MarkupRegionMember&) const = default;
};

// The most members a region holds.
inline constexpr uint32 kMaxRegionMembers = 32;

// An area of ground a mark-up names. The entity's closed SplineComponent (Linear unless
// authored smooth, its knots entity-local) is the base outline; its area is the evaluated
// ring (SplineData::ClosedPolygonXZ) placed by the WorldTransform. Inside = (the base or any
// Include member) and not any Exclude member, over ground-plane footprints: heights are
// ignored (MarkupECS::MarkupRegionArea).
// @ge-no-add        created by the agent and the box-to-region conversion, never by hand
// @ge-editor-only
struct MarkupRegion
{
    // How tall the region stands above its ground, in meters.
    float32 ExtrudeHeight = 8.0f;  // @ge-range 1 100
    // Members in [0, MemberCount); never above kMaxRegionMembers, and never the region itself.
    uint32 MemberCount = 0;  // @ge-hidden
    MarkupRegionMember Members[kMaxRegionMembers] = {};  // @ge-hidden
};

static_assert(sizeof(MarkupRegionMember) == 8, "MarkupRegionMember is a 4-byte handle and a mode, padded");
static_assert(std::is_trivially_copyable_v<MarkupRegion>, "MarkupRegion must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<MarkupRegion>, "MarkupRegion must be standard layout for ECS storage");

} // namespace GameEngine::Components
