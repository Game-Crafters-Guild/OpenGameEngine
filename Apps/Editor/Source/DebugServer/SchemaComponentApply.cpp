#include "DebugServer/SchemaComponentApply.h"

#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <vector>

namespace GameEngine::Editor
{

using json = nlohmann::json;

namespace
{

// Every component an entity carries, as bytes, so a refused request can put the entity back as it
// found it. A schema key may write a component other than the one it is named for (Terrain's grass
// keys write TerrainGrass, its planet relief keys TerrainPlanetRelief, grassEnabled the grass
// component's enable tag), so the whole entity is the unit of the request.
struct EntityComponentSnapshot
{
    std::vector<ECS::ComponentTypeId> Types;
    std::vector<std::vector<uint8_t>> Bytes; // parallel to Types; empty for a tag
};

std::vector<ECS::ComponentTypeId> ComponentTypesOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::Archetype* archetype = world.GetEntityArchetype(entity);
    return archetype ? archetype->GetSignature().GetComponents() : std::vector<ECS::ComponentTypeId>{};
}

EntityComponentSnapshot CaptureEntity(const ECS::World& world, ECS::EntityHandle entity)
{
    EntityComponentSnapshot snapshot;
    snapshot.Types = ComponentTypesOf(world, entity);
    snapshot.Bytes.resize(snapshot.Types.size());
    for (std::size_t i = 0; i < snapshot.Types.size(); ++i)
        world.CaptureComponentBytes(entity, snapshot.Types[i], snapshot.Bytes[i]);
    return snapshot;
}

// Removes what the request added, then writes every captured component back.
void RestoreEntity(ECS::World& world, ECS::EntityHandle entity, const EntityComponentSnapshot& snapshot)
{
    for (const ECS::ComponentTypeId type : ComponentTypesOf(world, entity))
    {
        if (std::find(snapshot.Types.begin(), snapshot.Types.end(), type) == snapshot.Types.end())
            world.RemoveComponentByTypeIdImmediate(entity, type);
    }
    for (std::size_t i = 0; i < snapshot.Types.size(); ++i)
        world.SetComponentBytesImmediate(entity, snapshot.Types[i], snapshot.Bytes[i].data(),
                                         snapshot.Bytes[i].size());
}

} // namespace

std::string ApplyComponentViaSchema(ECS::World& world, ECS::EntityHandle entity,
                                    const std::string& componentName,
                                    const Scene::ISceneComponentSchema& schema, const json& values)
{
    auto toLower = [](std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; };

    // Render a JSON scalar / vec into the string form the schema's parsers expect (ParseF32/ParseU32/
    // ParseBool/ParseFloat3). Enums (Tonemap, Shape) are passed as their int index.
    auto numToStr = [](const json& n) -> std::string {
        if (n.is_number_unsigned()) return std::to_string(n.get<unsigned long long>());
        if (n.is_number_integer()) return std::to_string(n.get<long long>());
        // Shortest round-trip float text, matching the scene serializer so the schema parser accepts
        // it (std::to_string would truncate small magnitudes to "0.000000").
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), n.get<double>());
        return std::string(buf, ptr);
    };
    auto allNumbers = [](const json& a, const json& b, const json& c) {
        return a.is_number() && b.is_number() && c.is_number();
    };
    auto toSchemaString = [&](const json& v, std::string& out) -> bool {
        if (v.is_boolean()) { out = v.get<bool>() ? "true" : "false"; return true; }
        if (v.is_number()) { out = numToStr(v); return true; }
        // The empty string is the quoted empty token: it is what the readback emits for an unset
        // reference and what every reference property reads as "clear", and bare empty text is no
        // value at all to the schema's parser.
        if (v.is_string()) { out = v.get<std::string>().empty() ? "\"\"" : v.get<std::string>(); return true; }
        if (v.is_array() && v.size() == 3 && allNumbers(v[0], v[1], v[2]))
        { out = "(" + numToStr(v[0]) + ", " + numToStr(v[1]) + ", " + numToStr(v[2]) + ")"; return true; }
        if (v.is_object() && v.contains("x") && v.contains("y") && v.contains("z") &&
            allNumbers(v["x"], v["y"], v["z"]))
        { out = "(" + numToStr(v["x"]) + ", " + numToStr(v["y"]) + ", " + numToStr(v["z"]) + ")"; return true; }
        return false;
    };

    // The schema applies one key at a time, so a refused key would leave the keys before it applied:
    // {"baseSource": 1, "terrainAsset": <refused>} switched a terrain to a heightmap base with no
    // heightmap. The request is all or nothing for the entity: a refusal restores it as it was.
    const EntityComponentSnapshot before = CaptureEntity(world, entity);

    Scene::SceneLoadContext loadCtx{};
    std::string err;
    bool anyApplied = false;
    for (auto it = values.begin(); it != values.end(); ++it)
    {
        const std::string key = toLower(it.key());
        std::string valStr;
        if (!toSchemaString(it.value(), valStr))
        {
            RestoreEntity(world, entity, before);
            return "Unsupported value type for " + componentName + "." + it.key();
        }
        if (!schema.ApplyProperty(world, entity, loadCtx, key, valStr, &err))
        {
            RestoreEntity(world, entity, before);
            return componentName + "." + it.key() + ": " + (err.empty() ? "invalid value" : err);
        }
        anyApplied = true;
    }
    if (!anyApplied && !schema.AddDefault(world, entity, &err))
        return "Could not add " + componentName + ": " + (err.empty() ? "AddDefault failed" : err);
    return {};
}

} // namespace GameEngine::Editor
