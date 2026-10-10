#include "Assets/Parsers/LensFlareParsers.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine {

namespace {

// Emit a single GUID-or-path reference edge; drops empty refs.
void EmitRefEdge(DepEdgeSink& sink, const GUID& referrer, std::string fieldLocator,
                 const std::string& rawValue)
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

std::string ReadJsonStringField(const std::filesystem::path& path, const char* field)
{
    const std::string body = ParserExtraction::ReadFileBody(path);
    if (body.empty())
        return {};
    nlohmann::json doc = nlohmann::json::parse(body, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return {};
    return doc.value(field, std::string{});
}

} // namespace

bool FlareAtlasParser::ExtractDependencies(const GUID& referrer, const AssetMetadata& metadata,
                                           DepEdgeSink& sink) const
{
    EmitRefEdge(sink, referrer, "texture", ReadJsonStringField(metadata.Path, "texture"));
    return true;
}

bool LensFlareDefinitionParser::ExtractDependencies(const GUID& referrer, const AssetMetadata& metadata,
                                                    DepEdgeSink& sink) const
{
    EmitRefEdge(sink, referrer, "atlas", ReadJsonStringField(metadata.Path, "atlas"));
    return true;
}

} // namespace GameEngine
