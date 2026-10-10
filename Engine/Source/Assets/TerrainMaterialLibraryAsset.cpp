#include "Assets/TerrainMaterialLibraryAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace GameEngine {

namespace {

constexpr int kSchemaVersion = 1;
constexpr const char* kAssetTypeName = "TerrainMaterialLibrary";

// The projection's spelling in the document. Triplanar is the default and is not written, so a
// library authored before the choice existed saves byte-identical.
constexpr const char* kProjectionTriplanar = "triplanar";
constexpr const char* kProjectionPlanar = "planar";

// A GUID field is written as its string form, or omitted when null. Reading is
// tolerant: a missing, empty or malformed value leaves the reference unbound,
// because a library with one unreadable texture reference must still load its
// other nineteen materials.
std::string GuidToJson(const GUID& guid)
{
    return guid.IsNull() ? std::string{} : guid.ToString();
}

GUID GuidFromJson(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || !j[key].is_string())
        return GUID{};
    const std::string text = j[key].get<std::string>();
    if (text.empty())
        return GUID{};
    try
    {
        return GUID(text);
    }
    catch (...)
    {
        return GUID{};
    }
}

nlohmann::json EntryToJson(const TerrainMaterialEntry& e)
{
    nlohmann::json j{
        {"name", e.Name},
        {"slotId", e.SlotId},
        {"albedo", {e.AlbedoR, e.AlbedoG, e.AlbedoB}},
        {"tiling", e.Tiling},
        {"roughness", e.Roughness},
        {"ao", e.Ao},
        {"normalStrength", e.NormalStrength},
        {"variationStrength", e.VariationStrength},
        {"variationHue", e.VariationHue},
        {"variationScale", e.VariationScale},
    };
    // Texture slots and the flags only appear when they carry information, so
    // a plain tinted material's entry stays readable by eye.
    if (const std::string albedoTex = GuidToJson(e.AlbedoTexture); !albedoTex.empty())
        j["albedoTexture"] = albedoTex;
    if (const std::string normalTex = GuidToJson(e.NormalTexture); !normalTex.empty())
        j["normalTexture"] = normalTex;
    if (const std::string ormTex = GuidToJson(e.OrmTexture); !ormTex.empty())
        j["ormTexture"] = ormTex;
    if (e.HexTiling)
        j["hexTiling"] = true;
    if (e.Retired)
        j["retired"] = true;
    if (e.OrmHasMetallic)
        j["ormHasMetallic"] = true;
    if (e.Projection == TerrainMaterialProjection::Planar)
        j["projection"] = kProjectionPlanar;
    return j;
}

// Field reads are STRICT about type, and a violation fails the whole document.
// nlohmann's value()/get<T>() coerce on type and THROW on a mismatch, and the
// load path they sit under has no enclosing try — it runs inline from
// Asset::Reload via AssetManager::CheckForReloads, so a throw here is an editor
// crash on hot-reload, not a failed load. Guarding is therefore mandatory; the
// remaining choice is granularity, and document-level is the honest one: a
// wrong-typed value has no valid interpretation, and dropping just its entry
// would leave the library reporting Loaded while a painted slot resolves to
// nothing. That is the same failure malformed text already gets.
//
// Texture references are the deliberate exception and stay tolerant — see
// GuidFromJson: an unresolvable reference is a normal runtime state the shading
// path already handles as "unbound".
//
// A missing key leaves the caller's default in place; only a PRESENT key of the
// wrong type is a failure.
bool ReadString(const nlohmann::json& j, const char* key, std::string& out)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_string())
        return false;
    out = j[key].get<std::string>();
    return true;
}

bool ReadFloat(const nlohmann::json& j, const char* key, float& out)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_number())
        return false;
    out = j[key].get<float>();
    return true;
}

bool ReadBool(const nlohmann::json& j, const char* key, bool& out)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_boolean())
        return false;
    out = j[key].get<bool>();
    return true;
}

// Range-checked, not truncated. A slot ID is the identity the painted splat stores, so a
// narrowing cast would silently rename the material an authored 256 or -1 refers to (they wrap to
// slots 0 and 255), and repaint whatever already held that slot.
bool ReadSlotId(const nlohmann::json& j, const char* key, std::uint8_t& out)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_number_integer())
        return false;
    const std::int64_t value = j[key].get<std::int64_t>();
    if (value < 0 || value > 255)
        return false;
    out = static_cast<std::uint8_t>(value);
    return true;
}

// Present ⇒ must be one of the two spellings. An unknown projection is a schema error rather than
// a Triplanar guess: guessing would render an orthophoto through the side projections silently.
bool ReadProjection(const nlohmann::json& j, TerrainMaterialProjection& out)
{
    if (!j.contains("projection"))
        return true;
    if (!j["projection"].is_string())
        return false;
    const std::string text = j["projection"].get<std::string>();
    if (text == kProjectionTriplanar)
        out = TerrainMaterialProjection::Triplanar;
    else if (text == kProjectionPlanar)
        out = TerrainMaterialProjection::Planar;
    else
        return false;
    return true;
}

// Present ⇒ must be three numbers. A tint of any other shape is a schema error,
// not a partially-authored colour to guess at.
bool ReadAlbedo(const nlohmann::json& j, TerrainMaterialEntry& e)
{
    if (!j.contains("albedo"))
        return true;
    const nlohmann::json& albedo = j["albedo"];
    if (!albedo.is_array() || albedo.size() != 3)
        return false;
    for (const auto& channel : albedo)
    {
        if (!channel.is_number())
            return false;
    }
    e.AlbedoR = albedo[0].get<float>();
    e.AlbedoG = albedo[1].get<float>();
    e.AlbedoB = albedo[2].get<float>();
    return true;
}

bool EntryFromJson(const nlohmann::json& j, TerrainMaterialEntry& out)
{
    TerrainMaterialEntry e{};
    if (!ReadString(j, "name", e.Name))
        return false;
    if (!ReadSlotId(j, "slotId", e.SlotId))
        return false;
    if (!ReadAlbedo(j, e))
        return false;
    if (!ReadFloat(j, "tiling", e.Tiling))
        return false;
    if (!ReadProjection(j, e.Projection))
        return false;
    if (!ReadFloat(j, "roughness", e.Roughness))
        return false;
    if (!ReadFloat(j, "ao", e.Ao))
        return false;
    if (!ReadFloat(j, "normalStrength", e.NormalStrength))
        return false;
    if (!ReadFloat(j, "variationStrength", e.VariationStrength))
        return false;
    if (!ReadFloat(j, "variationHue", e.VariationHue))
        return false;
    if (!ReadFloat(j, "variationScale", e.VariationScale))
        return false;

    e.AlbedoTexture = GuidFromJson(j, "albedoTexture");
    e.NormalTexture = GuidFromJson(j, "normalTexture");
    e.OrmTexture = GuidFromJson(j, "ormTexture");

    if (!ReadBool(j, "hexTiling", e.HexTiling))
        return false;
    if (!ReadBool(j, "retired", e.Retired))
        return false;
    if (!ReadBool(j, "ormHasMetallic", e.OrmHasMetallic))
        return false;

    out = std::move(e);
    return true;
}

} // namespace

const TerrainMaterialEntry* FindTerrainMaterialBySlotId(
    const std::vector<TerrainMaterialEntry>& entries, std::uint8_t slotId)
{
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [slotId](const TerrainMaterialEntry& e)
                                 { return e.SlotId == slotId; });
    return it != entries.end() ? &*it : nullptr;
}

TerrainMaterialEntry* FindTerrainMaterialBySlotIdMutable(std::vector<TerrainMaterialEntry>& entries,
                                                         std::uint8_t slotId)
{
    return const_cast<TerrainMaterialEntry*>(FindTerrainMaterialBySlotId(entries, slotId));
}

std::uint32_t NextFreeTerrainMaterialSlotId(const std::vector<TerrainMaterialEntry>& entries)
{
    bool taken[256] = {};
    for (const TerrainMaterialEntry& e : entries)
        taken[e.SlotId] = true;
    for (std::uint32_t id = 0; id < 256u; ++id)
    {
        if (!taken[id])
            return id;
    }
    return kInvalidTerrainMaterialSlotId;
}

std::string TerrainMaterialLibraryAsset::MakeDocumentText(
    const std::vector<TerrainMaterialEntry>& materials)
{
    nlohmann::json entries = nlohmann::json::array();
    for (const TerrainMaterialEntry& e : materials)
        entries.push_back(EntryToJson(e));

    const nlohmann::json document{
        {"schemaVersion", kSchemaVersion},
        {"assetType", kAssetTypeName},
        {"materials", std::move(entries)},
    };
    return document.dump(2) + "\n";
}

std::string TerrainMaterialLibraryAsset::MakeEmptyDocumentText()
{
    return MakeDocumentText({});
}

bool TerrainMaterialLibraryAsset::Save() const
{
    std::ofstream out(GetPath(), std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << MakeDocumentText(m_Materials);
    return out.good();
}

bool TerrainMaterialLibraryAsset::Load()
{
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        SetState(AssetState::Failed);
        return false;
    }
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

bool TerrainMaterialLibraryAsset::LoadFromData(const Vector<uint8>& data)
{
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void TerrainMaterialLibraryAsset::Unload()
{
    m_Materials.clear();
    SetState(AssetState::Unloaded);
}

bool TerrainMaterialLibraryAsset::ParseFromText(const std::string& text)
{
    m_Materials.clear();

    nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return false;

    // The header is checked, not assumed. Without this any JSON object with a `materials` array
    // loads as a terrain material library — including a future schema whose entries mean
    // something else, which would present as materials that quietly read wrong rather than as a
    // file this build cannot open.
    if (doc.contains("schemaVersion") &&
        (!doc["schemaVersion"].is_number_integer() ||
         doc["schemaVersion"].get<std::int64_t>() != kSchemaVersion))
        return false;
    if (doc.contains("assetType") &&
        (!doc["assetType"].is_string() || doc["assetType"].get<std::string>() != kAssetTypeName))
        return false;

    if (!doc.contains("materials"))
        return true;
    if (!doc["materials"].is_array())
        return false;

    for (const auto& entry : doc["materials"])
    {
        TerrainMaterialEntry parsed{};
        if (!entry.is_object() || !EntryFromJson(entry, parsed))
        {
            m_Materials.clear();
            return false;
        }
        m_Materials.push_back(std::move(parsed));
    }
    return true;
}

} // namespace GameEngine
