#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::MarkupECS
{

// Where a mark-up covers the ground, on the world's ground plane (x, z) in meters: a ring
// (a region's base outline, a box's outline from above), or a circle (a sphere's) when the
// ring is empty. Heights are ignored: a sphere 50 m up marks the circle beneath it.
struct MarkupFootprint
{
    std::vector<Mathematics::Vector2> Ring; // counter-clockwise for a box; a region's as authored
    Mathematics::Vector2 Center{};          // a circle's center
    float32 Radius = 0.0f;                  // a circle's radius
    Mathematics::Vector2 Min{};             // the bounds, which the point test checks first
    Mathematics::Vector2 Max{};

    bool IsCircle() const { return Ring.empty(); }
    // Even-odd over the ring, or within the circle (its edge included).
    bool Contains(const Mathematics::Vector2& point) const;
};

// The footprint of a mark-up with a shape, placed by its WorldTransform: a region's base
// outline (its knots for a linear outline, else its evaluated ring decimated by
// DecimateRegionRing; its members ignored), the convex hull of a box's eight corners, a
// sphere's circle. A point test against a region with 32 member regions then crosses at most
// 33 x 256 edges. None for an entity that is not a live mark-up with a
// shape, a region whose spline is not a closed ring, and a zero scale.
std::optional<MarkupFootprint> ReadMarkupFootprint(const ECS::World& world, ECS::EntityHandle entity);

// What ReadMarkupFootprint reads a mark-up's footprint from: whether it is a live mark-up with a
// shape, which shape, its placement and, for a region, its outline's spline and version. Equal
// keys give the same footprint.
struct MarkupFootprintKey
{
    enum class Kind : uint8
    {
        None,
        Box,
        Sphere,
        Region,
    };
    Kind Shape = Kind::None;
    float32 Matrix[16] = {};
    uint32 SplineIndex = 0;
    uint32 SplineGeneration = 0;
    uint64 SplineVersion = 0;

    bool operator==(const MarkupFootprintKey&) const = default;
};

MarkupFootprintKey ReadMarkupFootprintKey(const ECS::World& world, ECS::EntityHandle entity);

// A region's area: its base outline and its members' footprints. Inside = (the base or any
// Include member) and not any Exclude member. A member region contributes its base outline
// only, so nothing recurses and no cycle can form; a member that is no longer a live mark-up
// with a shape (deleted, until undo revives its handle) is skipped.
class MarkupRegionArea
{
  public:
    struct Member
    {
        ECS::EntityHandle Entity{};
        Components::MarkupMemberMode Mode = Components::MarkupMemberMode::Include;
        std::optional<MarkupFootprint> Footprint; // none for a skipped member
    };

    // None for an entity without a MarkupRegion or without a base outline.
    static std::optional<MarkupRegionArea> Read(const ECS::World& world, ECS::EntityHandle region);

    bool Contains(const Mathematics::Vector2& point) const;
    const MarkupFootprint& GetBase() const { return m_Base; }
    std::span<const Member> GetMembers() const { return m_Members; }

  private:
    MarkupFootprint m_Base;
    std::vector<Member> m_Members;
};

// The refusal, naming the fix, for a member list `region` cannot hold: more than
// kMaxRegionMembers, the region itself, an entity listed twice, or one that is not a mark-up
// with a shape. Nullopt for a list it can hold.
std::optional<std::string> CheckRegionMembers(const ECS::World& world, ECS::EntityHandle region,
                                              std::span<const Components::MarkupRegionMember> members);

} // namespace GameEngine::MarkupECS
