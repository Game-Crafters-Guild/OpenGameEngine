#include "Assets/Parsers/TerrainMaterialLibraryParser.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace GameEngine {

namespace {

// Emit a single GUID-or-path reference edge; drops empty refs.
void EmitTerrainMaterialRefEdge(DepEdgeSink& sink, const GUID& referrer,
                                std::string fieldLocator, const std::string& rawValue)
{
    if (rawValue.empty())
        return;

    DepEdge edge;
    edge.Referrer = referrer;
    edge.Kind = DepEdgeKind::Other;
    edge.FieldLocator = std::move(fieldLocator);

    if (ParserExtraction::LooksLikeGuid(rawValue))
        edge.Target = GUID(rawValue.c_str());
    else
        edge.TargetPath = ParserExtraction::NormalizeAuthoredPath(rawValue);

    if (!IsValidDepEdge(edge))
        return;

    sink.Emit(std::move(edge));
}

} // namespace

bool TerrainMaterialLibraryParser::ExtractDependencies(const GUID& referrer,
                                                       const AssetMetadata& metadata,
                                                       DepEdgeSink& sink) const
{
    const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
    if (body.empty())
        return true;

    nlohmann::json doc = nlohmann::json::parse(body, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return true;
    if (!doc.contains("materials") || !doc["materials"].is_array())
        return true;

    // Locators name the entry by its SLOT ID, not by array position: position is
    // display order and a reorder would otherwise rewrite every edge in the file.
    static constexpr const char* kTextureFields[] = {"albedoTexture", "normalTexture",
                                                     "ormTexture"};
    for (const auto& entry : doc["materials"])
    {
        if (!entry.is_object())
            continue;
        // Type-guarded, not coerced: value() throws on a type mismatch, and this
        // runs on registry scans over files nothing has vetted. An entry whose
        // slot cannot be named is skipped rather than failing the scan — unlike
        // the loader, whose wrong answer paints wrong pixels, this scan only
        // decides which files ship together, so losing one entry's edges beats
        // losing every later entry's. A MISSING slotId keeps the loader's
        // default of 0; only a present-but-wrong-typed one is unnameable.
        std::int64_t slotId = 0;
        if (entry.contains("slotId"))
        {
            if (!entry["slotId"].is_number_integer())
                continue;
            slotId = entry["slotId"].get<std::int64_t>();
        }
        const std::string slot = std::to_string(slotId);
        for (const char* field : kTextureFields)
        {
            if (!entry.contains(field) || !entry[field].is_string())
                continue;
            EmitTerrainMaterialRefEdge(sink, referrer,
                                       "materials[slot " + slot + "]." + field,
                                       entry[field].get<std::string>());
        }
    }
    return true;
}

} // namespace GameEngine
