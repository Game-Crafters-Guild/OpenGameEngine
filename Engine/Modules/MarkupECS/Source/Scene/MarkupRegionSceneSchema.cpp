// The scene lines of a region mark-up's MarkupRegion: its height and its member list. The
// outline is the entity's SplineComponent, saved by the Spline schema.
//
//   MarkupRegion.extrudeHeight = 8
//   MarkupRegion.member0 = exclude "lake"
//   MarkupRegion.member1 = include "east_field"
//
// A member names its mark-up by scene id. A member whose entity is not part of the save (it was
// deleted, or it is a stranger) is dropped and the members after it renumbered, so a saved
// list holds only members that load. On load, member lines are numbered in order from 0 (a
// line may restate an index already read or add the next one) up to kMaxRegionMembers; a
// member whose id names no entity of the scene loads as a dangling member, which the area
// skips and the next save drops. This schema owns the type under its own token, so the
// reflection fallback never writes the same fields a second time.

#include "Components/Markup/Markup.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Scene
{
namespace
{

using Components::MarkupMemberMode;
using Components::MarkupRegion;

constexpr std::string_view kMemberPrefix = "member";
constexpr float32 kMinExtrudeHeight = 1.0f;
constexpr float32 kMaxExtrudeHeight = 100.0f;

bool Refuse(std::string* outError, std::string why)
{
    if (outError)
        *outError = std::move(why);
    return false;
}

std::string_view ModeName(MarkupMemberMode mode)
{
    return mode == MarkupMemberMode::Exclude ? "exclude" : "include";
}

// `include "<id>"` or `exclude "<id>"`: the mode, then the quoted scene id.
bool ParseMember(const SceneLoadContext& ctx, std::string_view value, Components::MarkupRegionMember& out,
                 std::string* outError)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    const size_t space = value.find_first_of(" \t");
    const std::string_view mode = value.substr(0, space);
    if (mode == "include")
        out.Mode = MarkupMemberMode::Include;
    else if (mode == "exclude")
        out.Mode = MarkupMemberMode::Exclude;
    else
        return Refuse(outError, "MarkupRegion member must be include \"<id>\" or exclude \"<id>\"");
    if (space == std::string_view::npos || !TryResolveEntityReference(ctx, value.substr(space + 1), out.Entity))
        return Refuse(outError, "MarkupRegion member names its mark-up by a quoted scene id, as in exclude \"lake\"");
    return true;
}

class MarkupRegionSceneSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "MarkupRegion"; }

    bool IsPresent(const ECS::World& world, ECS::EntityHandle entity) const override
    {
        return world.GetComponent<MarkupRegion>(entity) != nullptr;
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* region = world.GetComponent<MarkupRegion>(entity);
        if (!region)
            return;
        outLines.push_back("MarkupRegion.extrudeHeight = " + FormatFloat(region->ExtrudeHeight));
        size_t written = 0;
        const uint32 count = std::min(region->MemberCount, Components::kMaxRegionMembers);
        for (uint32 i = 0; i < count; ++i)
        {
            const std::string id = FormatEntityReferenceForSave(ctx, region->Members[i].Entity);
            if (id.empty())
                continue;
            outLines.push_back("MarkupRegion.member" + std::to_string(written++) + " = " +
                               std::string(ModeName(region->Members[i].Mode)) + " " + id);
        }
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value, std::string* outError) const override
    {
        MarkupRegion region{};
        if (const auto* existing = world.GetComponent<MarkupRegion>(entity))
            region = *existing;

        if (property == "extrudeheight")
        {
            float height = 0.0f;
            if (!ParseFloat(value, height) || !(height >= kMinExtrudeHeight && height <= kMaxExtrudeHeight))
                return Refuse(outError, "MarkupRegion extrudeHeight must be meters from 1 to 100");
            region.ExtrudeHeight = height;
        }
        else if (property.substr(0, kMemberPrefix.size()) == kMemberPrefix)
        {
            uint32 index = 0;
            if (ParseIntegerToken(property.substr(kMemberPrefix.size()), index) != IntegerTokenResult::Ok)
                return Refuse(outError, "Unknown MarkupRegion property");
            if (index >= Components::kMaxRegionMembers)
                return Refuse(outError, "A region holds at most 32 members; MarkupRegion." + std::string(property) +
                                            " is past the last");
            if (index > region.MemberCount)
                return Refuse(outError, "MarkupRegion." + std::string(property) + " comes before member" +
                                            std::to_string(region.MemberCount) +
                                            "; member lines are numbered in order from 0");
            Components::MarkupRegionMember member{};
            if (!ParseMember(ctx, value, member, outError))
                return false;
            if (member.Entity == entity)
                return Refuse(outError, "A region cannot list itself as a member; drop the line");
            region.Members[index] = member;
            region.MemberCount = std::max(region.MemberCount, index + 1);
        }
        else
        {
            return Refuse(outError, "Unknown MarkupRegion property");
        }
        world.AddComponentImmediate(entity, region);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(MarkupRegionSceneSchema)

} // namespace
} // namespace GameEngine::Scene
