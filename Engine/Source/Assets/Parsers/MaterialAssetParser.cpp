#include "Assets/Parsers/MaterialAssetParser.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"
#include "Assets/MaterialXImport.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace GameEngine
{

namespace
{

// Emit a single shader-or-texture reference. Decides GUID-form vs path-form
// from the value shape; drops empty refs silently.
void EmitRefEdge(DepEdgeSink& sink,
                 const GUID& referrer,
                 DepEdgeKind kind,
                 std::string fieldLocator,
                 uint32_t ordinal,
                 const std::string& rawValue)
{
    if (rawValue.empty())
        return;

    DepEdge edge;
    edge.Referrer = referrer;
    edge.Kind = kind;
    edge.FieldLocator = std::move(fieldLocator);
    edge.Ordinal = ordinal;

    if (ParserExtraction::LooksLikeGuid(rawValue))
        edge.Target = GUID(rawValue.c_str());
    else
        edge.TargetPath = ParserExtraction::NormalizeAuthoredPath(rawValue);

    if (!IsValidDepEdge(edge))
        return; // GUID parsed to null → drop

    sink.Emit(std::move(edge));
}

} // namespace

bool MaterialAssetParser::ExtractDependencies(const GUID& referrer,
                                              const AssetMetadata& metadata,
                                              DepEdgeSink& sink) const
{
    const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
    if (body.empty())
        return false;

    // MaterialX (.mtlx) is XML, not JSON. Trace its texture-driven inputs to their backing image
    // files and emit those as MaterialTexture deps (resolved relative to the .mtlx) so editing a
    // referenced texture propagates a material rebuild / hot-reload, mirroring native materials.
    const size_t firstNonWs = body.find_first_not_of(" \t\r\n");
    if (firstNonWs != std::string::npos && body[firstNonWs] == '<')
    {
        const std::filesystem::path baseDir = metadata.Path.parent_path();
        uint32_t ordinal = 0;
        for (const std::string& file : CollectMaterialXTextureFiles(body))
        {
            const std::string abs = (baseDir / file).lexically_normal().string();
            EmitRefEdge(sink, referrer, DepEdgeKind::MaterialTexture,
                        "textures[" + std::to_string(ordinal) + "]", ordinal, abs);
            ++ordinal;
        }
        return true;
    }

    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(body);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Debug("MaterialAssetParser::ExtractDependencies: JSON parse error in {}: {}",
                           metadata.Path.string(), e.what());
        return false;
    }
    if (!doc.is_object())
        return false;

    // Surface shader: prefer GUID field (v3) over path field (v2/legacy).
    // Schema v3 still emits both for self-contained docs; the GUID is the
    // authoritative reference, the path is a hint for the ShaderComposer.
    {
        std::string surfaceShader = doc.value("surfaceShaderGuid", std::string{});
        if (surfaceShader.empty())
            surfaceShader = doc.value("surfaceShader", std::string{});
        EmitRefEdge(sink, referrer, DepEdgeKind::MaterialShader,
                    "surfaceShader", 0, surfaceShader);
    }

    // Vertex modifier: same GUID-then-path precedence.
    {
        std::string vertexModifier = doc.value("vertexModifierGuid", std::string{});
        if (vertexModifier.empty())
            vertexModifier = doc.value("vertexModifier", std::string{});
        EmitRefEdge(sink, referrer, DepEdgeKind::MaterialShader,
                    "vertexModifier", 1, vertexModifier);
    }

    // Textures: { name: "<guid|path>", ... } or { name: { guid: "...", ... }, ... }.
    if (auto txIt = doc.find("textures"); txIt != doc.end() && txIt->is_object())
    {
        uint32_t ordinal = 0;
        for (const auto& [name, value] : txIt->items())
        {
            std::string raw;
            if (value.is_string())
            {
                raw = value.get<std::string>();
            }
            else if (value.is_object())
            {
                raw = value.value("guid", std::string{});
                if (raw.empty())
                    raw = value.value("path", std::string{});
            }
            else
            {
                continue;
            }
            EmitRefEdge(sink, referrer, DepEdgeKind::MaterialTexture,
                        "textures." + name, ordinal++, raw);
        }
    }

    return true; // I scanned and emitted; don't run the syntactic fallback.
}

} // namespace GameEngine
