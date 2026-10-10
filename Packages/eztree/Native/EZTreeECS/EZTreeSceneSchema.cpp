#include "Scene/SceneSchemaRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneValue.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace GameEngine::EZTreeECS
{
namespace
{

using Scene::FormatFloat3;

std::string F(float v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

std::string B(bool v)
{
    return v ? "true" : "false";
}

bool ParseBool(std::string_view value, bool& out, std::string* outError)
{
    Scene::SceneValue v;
    if (!Scene::ParseValue(value, v, outError))
        return false;
    if (v.Kind != Scene::SceneValueKind::Bool)
    {
        if (outError)
            *outError = "Expected true/false";
        return false;
    }
    out = v.BoolValue;
    return true;
}

bool ParseFloat(std::string_view value, float& out, std::string* outError)
{
    Scene::SceneValue v;
    if (!Scene::ParseValue(value, v, outError))
        return false;
    if (v.Kind == Scene::SceneValueKind::Float)
    {
        out = static_cast<float>(v.FloatValue);
        return true;
    }
    if (v.Kind == Scene::SceneValueKind::Int)
    {
        out = static_cast<float>(v.IntValue);
        return true;
    }
    if (outError)
        *outError = "Expected number";
    return false;
}

bool ParseU32(std::string_view value, uint32& out, std::string* outError)
{
    Scene::SceneValue v;
    if (!Scene::ParseValue(value, v, outError))
        return false;
    if (v.Kind == Scene::SceneValueKind::Int && v.IntValue >= 0)
    {
        out = static_cast<uint32>(v.IntValue);
        return true;
    }
    if (v.Kind == Scene::SceneValueKind::Float && v.FloatValue >= 0.0)
    {
        out = static_cast<uint32>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected unsigned integer";
    return false;
}

bool ParseString(std::string_view value, std::string& out, std::string* outError)
{
    Scene::SceneValue v;
    if (!Scene::ParseValue(value, v, outError))
        return false;
    if (v.Kind == Scene::SceneValueKind::String || v.Kind == Scene::SceneValueKind::Identifier)
    {
        out = v.StringValue;
        return true;
    }
    if (outError)
        *outError = "Expected string";
    return false;
}

bool ParseVec3(std::string_view value, Mathematics::Vector3& out, std::string* outError)
{
    Scene::Float3 v;
    if (!Scene::ParseFloat3(value, v))
    {
        if (outError)
            *outError = "Expected (x, y, z)";
        return false;
    }
    out = {v.X, v.Y, v.Z};
    return true;
}

void WriteGuid(const GUID& guid, std::array<uint8, 16>& out)
{
    std::copy(guid.GetData().begin(), guid.GetData().end(), out.begin());
}

GUID ReadGuid(const std::array<uint8, 16>& bytes)
{
    GUID::Data data{};
    std::copy(bytes.begin(), bytes.end(), data.begin());
    return GUID(data);
}

bool ParseAssetRef(const Scene::SceneLoadContext& ctx,
                   std::string_view value,
                   AssetType type,
                   std::array<uint8, 16>& out,
                   std::string* outError)
{
    Scene::SceneValue sv;
    if (!Scene::ParseValue(value, sv, outError))
        return false;
    if (sv.Kind == Scene::SceneValueKind::Int && sv.IntValue == 0)
    {
        out.fill(0);
        return true;
    }
    AssetReference ref;
    std::string err;
    if (!Scene::TryResolveAssetReference(ctx, sv, type, ref, &err))
    {
        if (outError)
            *outError = err.empty() ? "Invalid asset reference" : err;
        return false;
    }
    WriteGuid(ref.guid, out);
    return true;
}

bool ParseMaterialRef(const Scene::SceneLoadContext& ctx,
                      std::string_view value,
                      std::array<uint8, 16>& out,
                      std::string* outError)
{
    return ParseAssetRef(ctx, value, AssetType::Material, out, outError);
}

bool ParseTextureRef(const Scene::SceneLoadContext& ctx,
                     std::string_view value,
                     std::array<uint8, 16>& out,
                     std::string* outError)
{
    return ParseAssetRef(ctx, value, AssetType::Texture, out, outError);
}

void AddAssetLine(std::vector<std::string>& out,
                  std::string_view property,
                  const Scene::SceneSaveContext& ctx,
                  const std::array<uint8, 16>& bytes)
{
    const GUID guid = ReadGuid(bytes);
    if (!guid.IsNull())
        out.push_back("EZTree." + std::string(property) + " = " + Scene::FormatAssetReferenceForSave(ctx, guid, ""));
}

void AddLevelLines(std::vector<std::string>& out, std::string_view prefix, const std::array<float, 4>& values)
{
    for (uint32 i = 0; i < values.size(); ++i)
        out.push_back("EZTree." + std::string(prefix) + std::to_string(i) + " = " + F(values[i]));
}

void AddLevelLines(std::vector<std::string>& out, std::string_view prefix, const std::array<uint32, 4>& values)
{
    for (uint32 i = 0; i < values.size(); ++i)
        out.push_back("EZTree." + std::string(prefix) + std::to_string(i) + " = " + std::to_string(values[i]));
}

class EZTreeSchema final : public Scene::ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "EZTree"; }

    bool IsPresent(const ECS::World& world, ECS::EntityHandle entity) const override
    {
        return world.GetComponent<Components::EZTree>(entity) != nullptr;
    }

    void Serialize(const ECS::World& world,
                   ECS::EntityHandle entity,
                   const Scene::SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* tree = world.GetComponent<Components::EZTree>(entity);
        if (!tree)
            return;
        const auto& o = tree->Options;
        outLines.push_back("EZTree.upstreamVersion = \"" + std::string(EZTree::kUpstreamVersion) + "\"");
        outLines.push_back("EZTree.upstreamCommit = \"" + std::string(EZTree::kUpstreamCommit) + "\"");
        outLines.push_back("EZTree.seed = " + std::to_string(o.seed));
        outLines.push_back("EZTree.type = \"" + EZTree::ToString(o.type) + "\"");
        outLines.push_back("EZTree.bark.type = \"" + EZTree::ToString(o.bark.type) + "\"");
        outLines.push_back("EZTree.bark.tint = " + std::to_string(o.bark.tint));
        outLines.push_back("EZTree.bark.flatShading = " + B(o.bark.flatShading));
        outLines.push_back("EZTree.bark.textured = " + B(o.bark.textured));
        outLines.push_back("EZTree.bark.textureScale = (" + F(o.bark.textureScale.x) + ", " + F(o.bark.textureScale.y) + ", 0)");
        outLines.push_back("EZTree.branch.levels = " + std::to_string(o.branch.levels));
        outLines.push_back("EZTree.branch.force.direction = " + FormatFloat3(o.branch.forceDirection.x, o.branch.forceDirection.y, o.branch.forceDirection.z));
        outLines.push_back("EZTree.branch.force.strength = " + F(o.branch.forceStrength));
        AddLevelLines(outLines, "branch.angle.", o.branch.angle);
        AddLevelLines(outLines, "branch.children.", o.branch.children);
        AddLevelLines(outLines, "branch.gnarliness.", o.branch.gnarliness);
        AddLevelLines(outLines, "branch.length.", o.branch.length);
        AddLevelLines(outLines, "branch.radius.", o.branch.radius);
        AddLevelLines(outLines, "branch.sections.", o.branch.sections);
        AddLevelLines(outLines, "branch.segments.", o.branch.segments);
        AddLevelLines(outLines, "branch.start.", o.branch.start);
        AddLevelLines(outLines, "branch.taper.", o.branch.taper);
        AddLevelLines(outLines, "branch.twist.", o.branch.twist);
        outLines.push_back("EZTree.leaves.type = \"" + EZTree::ToString(o.leaves.type) + "\"");
        outLines.push_back("EZTree.leaves.billboard = \"" + EZTree::ToString(o.leaves.billboard) + "\"");
        outLines.push_back("EZTree.leaves.angle = " + F(o.leaves.angle));
        outLines.push_back("EZTree.leaves.count = " + std::to_string(o.leaves.count));
        outLines.push_back("EZTree.leaves.start = " + F(o.leaves.start));
        outLines.push_back("EZTree.leaves.size = " + F(o.leaves.size));
        outLines.push_back("EZTree.leaves.sizeVariance = " + F(o.leaves.sizeVariance));
        outLines.push_back("EZTree.leaves.tint = " + std::to_string(o.leaves.tint));
        outLines.push_back("EZTree.leaves.alphaTest = " + F(o.leaves.alphaTest));
        outLines.push_back("EZTree.leaves.textureColumns = " + std::to_string(o.leaves.textureColumns));
        outLines.push_back("EZTree.leaves.textureRows = " + std::to_string(o.leaves.textureRows));
        outLines.push_back("EZTree.leaves.textureTile = " + std::to_string(o.leaves.textureTile));
        outLines.push_back("EZTree.leaves.textureScale = (" + F(o.leaves.textureScale.x) + ", " + F(o.leaves.textureScale.y) + ", 0)");
        outLines.push_back("EZTree.leaves.textureOffset = (" + F(o.leaves.textureOffset.x) + ", " + F(o.leaves.textureOffset.y) + ", 0)");
        outLines.push_back("EZTree.leaves.randomTextureTile = " + B(o.leaves.randomTextureTile));
        outLines.push_back("EZTree.leaves.roundedNormals = " + B(o.leaves.roundedNormals));
        outLines.push_back("EZTree.trellis.enabled = " + B(o.trellis.enabled));
        outLines.push_back("EZTree.trellis.position = " + FormatFloat3(o.trellis.position.x, o.trellis.position.y, o.trellis.position.z));
        outLines.push_back("EZTree.trellis.width = " + F(o.trellis.width));
        outLines.push_back("EZTree.trellis.height = " + F(o.trellis.height));
        outLines.push_back("EZTree.trellis.spacing = " + F(o.trellis.spacing));
        outLines.push_back("EZTree.trellis.force.strength = " + F(o.trellis.forceStrength));
        outLines.push_back("EZTree.trellis.force.maxDistance = " + F(o.trellis.forceMaxDistance));
        outLines.push_back("EZTree.trellis.force.falloff = " + F(o.trellis.forceFalloff));
        outLines.push_back("EZTree.trellis.cylinderRadius = " + F(o.trellis.cylinderRadius));
        outLines.push_back("EZTree.trellis.visible = " + B(o.trellis.visible));
        outLines.push_back("EZTree.trellis.color = " + std::to_string(o.trellis.color));
        outLines.push_back("EZTree.wind.enabled = " + B(o.wind.enabled));
        outLines.push_back("EZTree.wind.strength = " + FormatFloat3(o.wind.strength.x, o.wind.strength.y, o.wind.strength.z));
        outLines.push_back("EZTree.wind.frequency = " + F(o.wind.frequency));
        outLines.push_back("EZTree.wind.scale = " + F(o.wind.scale));
        outLines.push_back("EZTree.branchOverrides.count = " + std::to_string(o.branchOverrideCount));
        for (uint32 i = 0; i < o.branchOverrideCount && i < o.branchOverrides.size(); ++i)
        {
            const auto& ov = o.branchOverrides[i];
            const std::string p = "EZTree.branchOverrides." + std::to_string(i) + ".";
            outLines.push_back(p + "enabled = " + B(ov.enabled));
            outLines.push_back(p + "branchId = " + std::to_string(ov.branchId));
            outLines.push_back(p + "level = " + std::to_string(ov.level));
            outLines.push_back(p + "lengthScale = " + F(ov.lengthScale));
            outLines.push_back(p + "radiusScale = " + F(ov.radiusScale));
            outLines.push_back(p + "angleOffsetDegrees = " + F(ov.angleOffsetDegrees));
            outLines.push_back(p + "twistOffset = " + F(ov.twistOffset));
        }
        AddAssetLine(outLines, "materials.bark", ctx, o.barkMaterialGuid);
        AddAssetLine(outLines, "materials.leaf", ctx, o.leafMaterialGuid);
        AddAssetLine(outLines, "materials.trellis", ctx, o.trellisMaterialGuid);
        AddAssetLine(outLines, "textures.bark.color", ctx, o.barkColorTextureGuid);
        AddAssetLine(outLines, "textures.bark.normal", ctx, o.barkNormalTextureGuid);
        AddAssetLine(outLines, "textures.bark.roughness", ctx, o.barkRoughnessTextureGuid);
        AddAssetLine(outLines, "textures.bark.ao", ctx, o.barkAoTextureGuid);
        AddAssetLine(outLines, "textures.leaf.color", ctx, o.leafColorTextureGuid);
        AddAssetLine(outLines, "textures.trellis", ctx, o.trellisTextureGuid);
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const Scene::SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::EZTree comp{};
        if (auto* existing = world.GetComponent<Components::EZTree>(entity))
            comp = *existing;
        auto& o = comp.Options;
        bool ok = true;
        auto levelSuffix = [](std::string_view prop, std::string_view prefix, uint32& out) -> bool {
            if (!prop.starts_with(prefix))
                return false;
            const std::string s(prop.substr(prefix.size()));
            if (s.empty() || s.size() > 1 || s[0] < '0' || s[0] > '3')
                return false;
            out = static_cast<uint32>(s[0] - '0');
            return true;
        };

        uint32 idx = 0;
        if (property == "seed") ok = ParseU32(value, o.seed, outError);
        else if (property == "type") { std::string s; ok = ParseString(value, s, outError); if (ok) o.type = EZTree::TreeTypeFromString(s, o.type); }
        else if (property == "bark.type") { std::string s; ok = ParseString(value, s, outError); if (ok) o.bark.type = EZTree::BarkTypeFromString(s, o.bark.type); }
        else if (property == "bark.tint") ok = ParseU32(value, o.bark.tint, outError);
        else if (property == "bark.flatshading") ok = ParseBool(value, o.bark.flatShading, outError);
        else if (property == "bark.textured") ok = ParseBool(value, o.bark.textured, outError);
        else if (property == "bark.texturescale") { Mathematics::Vector3 v; ok = ParseVec3(value, v, outError); if (ok) o.bark.textureScale = {v.x, v.y}; }
        else if (property == "branch.levels") { ok = ParseU32(value, o.branch.levels, outError); o.branch.levels = std::min<uint32>(o.branch.levels, 3u); }
        else if (property == "branch.force.direction") ok = ParseVec3(value, o.branch.forceDirection, outError);
        else if (property == "branch.force.strength") ok = ParseFloat(value, o.branch.forceStrength, outError);
        else if (levelSuffix(property, "branch.angle.", idx)) ok = ParseFloat(value, o.branch.angle[idx], outError);
        else if (levelSuffix(property, "branch.children.", idx)) ok = ParseU32(value, o.branch.children[idx], outError);
        else if (levelSuffix(property, "branch.gnarliness.", idx)) ok = ParseFloat(value, o.branch.gnarliness[idx], outError);
        else if (levelSuffix(property, "branch.length.", idx)) ok = ParseFloat(value, o.branch.length[idx], outError);
        else if (levelSuffix(property, "branch.radius.", idx)) ok = ParseFloat(value, o.branch.radius[idx], outError);
        else if (levelSuffix(property, "branch.sections.", idx)) ok = ParseU32(value, o.branch.sections[idx], outError);
        else if (levelSuffix(property, "branch.segments.", idx)) ok = ParseU32(value, o.branch.segments[idx], outError);
        else if (levelSuffix(property, "branch.start.", idx)) ok = ParseFloat(value, o.branch.start[idx], outError);
        else if (levelSuffix(property, "branch.taper.", idx)) ok = ParseFloat(value, o.branch.taper[idx], outError);
        else if (levelSuffix(property, "branch.twist.", idx)) ok = ParseFloat(value, o.branch.twist[idx], outError);
        else if (property == "leaves.type") { std::string s; ok = ParseString(value, s, outError); if (ok) o.leaves.type = EZTree::LeafTypeFromString(s, o.leaves.type); }
        else if (property == "leaves.billboard") { std::string s; ok = ParseString(value, s, outError); if (ok) o.leaves.billboard = EZTree::BillboardModeFromString(s, o.leaves.billboard); }
        else if (property == "leaves.angle") ok = ParseFloat(value, o.leaves.angle, outError);
        else if (property == "leaves.count") ok = ParseU32(value, o.leaves.count, outError);
        else if (property == "leaves.start") ok = ParseFloat(value, o.leaves.start, outError);
        else if (property == "leaves.size") ok = ParseFloat(value, o.leaves.size, outError);
        else if (property == "leaves.sizevariance") ok = ParseFloat(value, o.leaves.sizeVariance, outError);
        else if (property == "leaves.tint") ok = ParseU32(value, o.leaves.tint, outError);
        else if (property == "leaves.alphatest") { ok = ParseFloat(value, o.leaves.alphaTest, outError); o.leaves.alphaTest = std::clamp(o.leaves.alphaTest, 0.0f, 1.0f); }
        else if (property == "leaves.texturecolumns") { ok = ParseU32(value, o.leaves.textureColumns, outError); o.leaves.textureColumns = std::max(1u, o.leaves.textureColumns); }
        else if (property == "leaves.texturerows") { ok = ParseU32(value, o.leaves.textureRows, outError); o.leaves.textureRows = std::max(1u, o.leaves.textureRows); }
        else if (property == "leaves.texturetile") ok = ParseU32(value, o.leaves.textureTile, outError);
        else if (property == "leaves.texturescale") { Mathematics::Vector3 v; ok = ParseVec3(value, v, outError); if (ok) o.leaves.textureScale = {std::max(0.0001f, v.x), std::max(0.0001f, v.y)}; }
        else if (property == "leaves.textureoffset") { Mathematics::Vector3 v; ok = ParseVec3(value, v, outError); if (ok) o.leaves.textureOffset = {v.x, v.y}; }
        else if (property == "leaves.randomtexturetile") ok = ParseBool(value, o.leaves.randomTextureTile, outError);
        else if (property == "leaves.roundednormals") ok = ParseBool(value, o.leaves.roundedNormals, outError);
        else if (property == "trellis.enabled") ok = ParseBool(value, o.trellis.enabled, outError);
        else if (property == "trellis.position") ok = ParseVec3(value, o.trellis.position, outError);
        else if (property == "trellis.width") ok = ParseFloat(value, o.trellis.width, outError);
        else if (property == "trellis.height") ok = ParseFloat(value, o.trellis.height, outError);
        else if (property == "trellis.spacing") ok = ParseFloat(value, o.trellis.spacing, outError);
        else if (property == "trellis.force.strength") ok = ParseFloat(value, o.trellis.forceStrength, outError);
        else if (property == "trellis.force.maxdistance") ok = ParseFloat(value, o.trellis.forceMaxDistance, outError);
        else if (property == "trellis.force.falloff") ok = ParseFloat(value, o.trellis.forceFalloff, outError);
        else if (property == "trellis.cylinderradius") ok = ParseFloat(value, o.trellis.cylinderRadius, outError);
        else if (property == "trellis.visible") ok = ParseBool(value, o.trellis.visible, outError);
        else if (property == "trellis.color") ok = ParseU32(value, o.trellis.color, outError);
        else if (property == "wind.enabled") ok = ParseBool(value, o.wind.enabled, outError);
        else if (property == "wind.strength") ok = ParseVec3(value, o.wind.strength, outError);
        else if (property == "wind.frequency") ok = ParseFloat(value, o.wind.frequency, outError);
        else if (property == "wind.scale") ok = ParseFloat(value, o.wind.scale, outError);
        else if (property == "branchoverrides.count") { ok = ParseU32(value, o.branchOverrideCount, outError); o.branchOverrideCount = std::min<uint32>(o.branchOverrideCount, static_cast<uint32>(o.branchOverrides.size())); }
        else if (property == "materials.bark") ok = ParseMaterialRef(ctx, value, o.barkMaterialGuid, outError);
        else if (property == "materials.leaf") ok = ParseMaterialRef(ctx, value, o.leafMaterialGuid, outError);
        else if (property == "materials.trellis") ok = ParseMaterialRef(ctx, value, o.trellisMaterialGuid, outError);
        else if (property == "textures.bark.color") ok = ParseTextureRef(ctx, value, o.barkColorTextureGuid, outError);
        else if (property == "textures.bark.normal") ok = ParseTextureRef(ctx, value, o.barkNormalTextureGuid, outError);
        else if (property == "textures.bark.roughness") ok = ParseTextureRef(ctx, value, o.barkRoughnessTextureGuid, outError);
        else if (property == "textures.bark.ao") ok = ParseTextureRef(ctx, value, o.barkAoTextureGuid, outError);
        else if (property == "textures.leaf.color") ok = ParseTextureRef(ctx, value, o.leafColorTextureGuid, outError);
        else if (property == "textures.trellis") ok = ParseTextureRef(ctx, value, o.trellisTextureGuid, outError);
        else if (property.starts_with("branchoverrides."))
        {
            const std::string s(property);
            const size_t dot = s.find('.', std::string("branchoverrides.").size());
            if (dot == std::string::npos)
                ok = false;
            else
            {
                const uint32 ovIdx = static_cast<uint32>(std::stoul(s.substr(std::string("branchoverrides.").size(), dot - std::string("branchoverrides.").size())));
                if (ovIdx >= o.branchOverrides.size())
                    ok = false;
                else
                {
                    o.branchOverrideCount = std::max(o.branchOverrideCount, ovIdx + 1u);
                    auto& ov = o.branchOverrides[ovIdx];
                    const std::string field = s.substr(dot + 1u);
                    if (field == "enabled") ok = ParseBool(value, ov.enabled, outError);
                    else if (field == "branchid") ok = ParseU32(value, ov.branchId, outError);
                    else if (field == "level") ok = ParseU32(value, ov.level, outError);
                    else if (field == "lengthscale") ok = ParseFloat(value, ov.lengthScale, outError);
                    else if (field == "radiusscale") ok = ParseFloat(value, ov.radiusScale, outError);
                    else if (field == "angleoffsetdegrees") ok = ParseFloat(value, ov.angleOffsetDegrees, outError);
                    else if (field == "twistoffset") ok = ParseFloat(value, ov.twistOffset, outError);
                    else ok = false;
                }
            }
        }
        else if (property == "upstreamversion" || property == "upstreamcommit")
        {
            ok = true;
        }
        else
        {
            if (outError)
                *outError = "Unknown EZTree property";
            return false;
        }

        if (!ok)
            return false;
        comp.RuntimeOptionsHash = 0;
        comp.RuntimeMeshHandleId = 0;
        world.AddComponentImmediate(entity, comp);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string*) const override
    {
        world.AddComponentImmediate(entity, Components::EZTree{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::EZTree>(entity);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* tree = world.GetComponent<Components::EZTree>(entity);
        if (!tree || !visitor)
            return;
        const GUID bark = ReadGuid(tree->Options.barkMaterialGuid);
        const GUID leaf = ReadGuid(tree->Options.leafMaterialGuid);
        const GUID trellis = ReadGuid(tree->Options.trellisMaterialGuid);
        if (!bark.IsNull())
            visitor(bark, AssetType::Material, {}, "materials.bark");
        if (!leaf.IsNull())
            visitor(leaf, AssetType::Material, {}, "materials.leaf");
        if (!trellis.IsNull())
            visitor(trellis, AssetType::Material, {}, "materials.trellis");
        const GUID barkColor = ReadGuid(tree->Options.barkColorTextureGuid);
        const GUID barkNormal = ReadGuid(tree->Options.barkNormalTextureGuid);
        const GUID barkRoughness = ReadGuid(tree->Options.barkRoughnessTextureGuid);
        const GUID barkAo = ReadGuid(tree->Options.barkAoTextureGuid);
        const GUID leafColor = ReadGuid(tree->Options.leafColorTextureGuid);
        const GUID trellisTexture = ReadGuid(tree->Options.trellisTextureGuid);
        if (!barkColor.IsNull())
            visitor(barkColor, AssetType::Texture, {}, "textures.bark.color");
        if (!barkNormal.IsNull())
            visitor(barkNormal, AssetType::Texture, {}, "textures.bark.normal");
        if (!barkRoughness.IsNull())
            visitor(barkRoughness, AssetType::Texture, {}, "textures.bark.roughness");
        if (!barkAo.IsNull())
            visitor(barkAo, AssetType::Texture, {}, "textures.bark.ao");
        if (!leafColor.IsNull())
            visitor(leafColor, AssetType::Texture, {}, "textures.leaf.color");
        if (!trellisTexture.IsNull())
            visitor(trellisTexture, AssetType::Texture, {}, "textures.trellis");
    }
};

} // namespace

void RegisterEZTreeSceneSchemas()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    Scene::SceneSchemaRegistry::Register(std::make_unique<EZTreeSchema>());
}

} // namespace GameEngine::EZTreeECS
