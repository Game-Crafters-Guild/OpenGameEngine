#include "Assets/Parsers/ModelAssetParser.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"

#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace GameEngine
{

namespace
{

bool IsDataUri(std::string_view uri)
{
    return uri.size() >= 5 && uri.compare(0, 5, "data:") == 0;
}

std::string LowerExtension(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Try to extract the JSON body from a .glb file. .glb structure:
//   12-byte header: magic "glTF" (LE u32) + version (u32) + total length (u32)
//   chunk 1:        length (u32) + type (u32) + data[length]      // type = "JSON"
//   chunk 2 (opt):  length (u32) + type (u32) + data[length]      // type = "BIN"
// Returns empty string on any header / chunk-type mismatch.
std::string ExtractGlbJson(const std::string& fileBytes)
{
    static constexpr uint32_t kGlbMagic = 0x46546C67u;     // "glTF" little-endian
    static constexpr uint32_t kJsonChunkType = 0x4E4F534Au; // "JSON" little-endian

    if (fileBytes.size() < 20)
        return {};

    uint32_t magic = 0;
    std::memcpy(&magic, fileBytes.data(), 4);
    if (magic != kGlbMagic)
        return {};

    uint32_t chunkLength = 0;
    uint32_t chunkType = 0;
    std::memcpy(&chunkLength, fileBytes.data() + 12, 4);
    std::memcpy(&chunkType, fileBytes.data() + 16, 4);
    if (chunkType != kJsonChunkType)
        return {};
    if (fileBytes.size() < static_cast<size_t>(20) + chunkLength)
        return {};

    return std::string(fileBytes.data() + 20, chunkLength);
}

void EmitTextureFromUri(DepEdgeSink& sink,
                        const GUID& referrer,
                        std::string fieldLocator,
                        uint32_t ordinal,
                        const std::string& uri)
{
    if (uri.empty() || IsDataUri(uri))
        return;

    DepEdge edge;
    edge.Referrer = referrer;
    edge.Kind = DepEdgeKind::MaterialTexture;
    edge.FieldLocator = std::move(fieldLocator);
    edge.Ordinal = ordinal;
    // glTF image uri is always a path relative to the .gltf, never a GUID.
    edge.TargetPath = ParserExtraction::NormalizeAuthoredPath(uri);

    if (!IsValidDepEdge(edge))
        return;

    sink.Emit(std::move(edge));
}

// Walk images[] in a glTF document, emitting one MaterialTexture edge per
// non-empty, non-data-URI entry. Locator is "images[N].uri".
void EmitImageEdgesFromGltfJson(DepEdgeSink& sink,
                                const GUID& referrer,
                                const nlohmann::json& doc)
{
    auto imagesIt = doc.find("images");
    if (imagesIt == doc.end() || !imagesIt->is_array())
        return;

    uint32_t ordinal = 0;
    for (size_t i = 0; i < imagesIt->size(); ++i)
    {
        const auto& image = (*imagesIt)[i];
        if (!image.is_object())
            continue;
        std::string uri = image.value("uri", std::string{});
        std::string locator = "images[" + std::to_string(i) + "].uri";
        EmitTextureFromUri(sink, referrer, std::move(locator), ordinal++, uri);
    }
}

} // namespace

bool ModelAssetParser::ExtractDependencies(const GUID& referrer,
                                           const AssetMetadata& metadata,
                                           DepEdgeSink& sink) const
{
    const std::string ext = LowerExtension(metadata.Path);

    if (ext == ".gltf")
    {
        const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
        if (body.empty())
            return false;

        nlohmann::json doc;
        try
        {
            doc = nlohmann::json::parse(body);
        }
        catch (const std::exception& e)
        {
            Logger::Log::Debug("ModelAssetParser::ExtractDependencies: glTF parse error in {}: {}",
                               metadata.Path.string(), e.what());
            return false;
        }
        if (!doc.is_object())
            return false;

        EmitImageEdgesFromGltfJson(sink, referrer, doc);
        return true;
    }

    if (ext == ".glb")
    {
        const std::string body = ParserExtraction::ReadFileBody(metadata.Path);
        if (body.empty())
            return false;

        const std::string json = ExtractGlbJson(body);
        if (json.empty())
            return false;

        nlohmann::json doc;
        try
        {
            doc = nlohmann::json::parse(json);
        }
        catch (const std::exception& e)
        {
            Logger::Log::Debug("ModelAssetParser::ExtractDependencies: glb JSON-chunk parse error in {}: {}",
                               metadata.Path.string(), e.what());
            return false;
        }
        if (!doc.is_object())
            return false;

        EmitImageEdgesFromGltfJson(sink, referrer, doc);
        return true;
    }

    // Other formats (.obj, .fbx, .blend) — binary or .mtl-sidecar based.
    // Returning true means "parser handled it; no edges to emit." This
    // suppresses the syntactic fallback's misfire on binary content.
    // Future workstream: parse .mtl for .obj, ufbx for .fbx, fbtBlend for .blend.
    return true;
}

} // namespace GameEngine
