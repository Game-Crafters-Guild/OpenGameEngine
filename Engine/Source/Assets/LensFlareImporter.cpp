#include "Assets/LensFlareImporter.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/LensFlareTypes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>

namespace GameEngine::LensFlareImport {

namespace {

using nlohmann::json;

std::string ReadFile(const std::filesystem::path& path)
{
    std::string text;
    if (!ReadFileTextShared(path, text))
        return {};
    return text;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text, std::string* err)
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        if (err)
            *err = "Cannot open output for writing: " + path.string();
        return false;
    }
    out << text;
    return true;
}

void Fail(std::string* err, const std::string& msg)
{
    if (err)
        *err = msg;
}

// The external flare document is near-JSON: it omits commas between some
// adjacent members (e.g. `"type": "0"` directly followed by `"size": {...}`).
// Insert a comma wherever a value-terminating char is followed only by
// whitespace/newline and then a quoted key. Machine-generated input, so this
// targeted fix is sufficient; strings already separated by a comma or brace are
// untouched. Also strips a UTF-8 BOM if present.
std::string MakeStrictJson(std::string s)
{
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);

    static const std::regex missingComma(R"(([0-9"\}\]])(\s*\r?\n\s*)("))");
    return std::regex_replace(s, missingComma, "$1,$2$3");
}

// Read a bool from a field that may be authored as a JSON bool or 0/1 number.
bool ReadFlexBool(const json& j, const char* key, bool fallback)
{
    auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_number())
        return it->get<double>() != 0.0;
    return fallback;
}

float ReadFlexFloat(const json& j, const char* key, float fallback)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return fallback;
    return it->get<float>();
}

json ColorToJson(const LensFlare::Color& c)
{
    return json{{"r", c.R}, {"g", c.G}, {"b", c.B}, {"a", c.A}};
}

LensFlare::Color ReadColor(const json& j, const char* key, LensFlare::Color fallback)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        return fallback;
    LensFlare::Color c;
    c.R = ReadFlexFloat(*it, "r", fallback.R);
    c.G = ReadFlexFloat(*it, "g", fallback.G);
    c.B = ReadFlexFloat(*it, "b", fallback.B);
    c.A = ReadFlexFloat(*it, "a", fallback.A);
    return c;
}

// Transcode an authored curve ({"key0": {time,value,in,out}, ...}) into a flat
// key array. Returns an empty array when absent — consumers fall back to their
// defaults.
json ReadCurveKeys(const json& j, const char* name)
{
    json keys = json::array();
    auto it = j.find(name);
    if (it == j.end() || !it->is_object())
        return keys;
    for (int i = 0;; ++i)
    {
        auto keyIt = it->find("key" + std::to_string(i));
        if (keyIt == it->end())
            break;
        keys.push_back(json{
            {"time", ReadFlexFloat(*keyIt, "time", 0.0f)},
            {"value", ReadFlexFloat(*keyIt, "value", 0.0f)},
            {"inTangent", ReadFlexFloat(*keyIt, "in", 0.0f)},
            {"outTangent", ReadFlexFloat(*keyIt, "out", 0.0f)},
        });
    }
    return keys;
}

// Strip a trailing source extension (".psd", ".png", ...) from an atlas frame
// key so it matches the element sprite names ("Glow_LargeFalloff").
std::string StripSpriteExtension(const std::string& name)
{
    const size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

} // namespace

bool ImportAtlas(const std::filesystem::path& sourceTexturePackerJson,
                 const std::string& textureRef,
                 const std::filesystem::path& outFlareAtlasPath,
                 std::string* outError)
{
    const std::string body = ReadFile(sourceTexturePackerJson);
    if (body.empty())
    {
        Fail(outError, "Cannot read atlas source: " + sourceTexturePackerJson.string());
        return false;
    }

    json doc = json::parse(body, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("frames"))
    {
        Fail(outError, "Atlas source is not valid TexturePacker JSON: " + sourceTexturePackerJson.string());
        return false;
    }

    float atlasW = 0.0f;
    float atlasH = 0.0f;
    if (doc.contains("meta") && doc["meta"].is_object() && doc["meta"].contains("size"))
    {
        atlasW = ReadFlexFloat(doc["meta"]["size"], "w", 0.0f);
        atlasH = ReadFlexFloat(doc["meta"]["size"], "h", 0.0f);
    }
    if (atlasW <= 0.0f || atlasH <= 0.0f)
    {
        Fail(outError, "Atlas source missing meta.size");
        return false;
    }

    json sprites = json::array();
    const json& frames = doc["frames"];
    if (!frames.is_object())
    {
        Fail(outError, "Atlas 'frames' is not an object");
        return false;
    }
    for (auto it = frames.begin(); it != frames.end(); ++it)
    {
        const json& f = it.value();
        if (!f.is_object() || !f.contains("frame"))
            continue;
        const json& r = f["frame"];
        const float x = ReadFlexFloat(r, "x", 0.0f);
        const float y = ReadFlexFloat(r, "y", 0.0f);
        const float w = ReadFlexFloat(r, "w", 0.0f);
        const float h = ReadFlexFloat(r, "h", 0.0f);
        sprites.push_back(json{
            {"name", StripSpriteExtension(it.key())},
            {"u", x / atlasW},
            {"v", y / atlasH},
            {"w", w / atlasW},
            {"h", h / atlasH},
        });
    }

    json out{{"texture", textureRef}, {"sprites", std::move(sprites)}};
    return WriteFile(outFlareAtlasPath,
                     out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace),
                     outError);
}

bool ImportFlare(const std::filesystem::path& sourceFlareDocument,
                 const std::string& atlasRef,
                 const std::filesystem::path& outLensFlarePath,
                 std::string* outError,
                 const std::vector<std::string>* atlasSpriteNames)
{
    const std::string raw = ReadFile(sourceFlareDocument);
    if (raw.empty())
    {
        Fail(outError, "Cannot read flare source: " + sourceFlareDocument.string());
        return false;
    }

    json doc = json::parse(MakeStrictJson(raw), nullptr, /*allow_exceptions*/ false);
    // A flare document has a "meta" object carrying flare globals + an "Elements"
    // map. Reject anything else — notably a TexturePacker atlas, which also has a
    // "meta" object but a top-level "frames" map and no flare fields.
    const bool looksLikeFlare = !doc.is_discarded() && doc.is_object() && doc.contains("meta") &&
                                doc["meta"].is_object() && !doc.contains("frames") &&
                                (doc["meta"].contains("Elements") || doc["meta"].contains("GlobalScale"));
    if (!looksLikeFlare)
    {
        Fail(outError, "Flare source is not a recognized flare document: " + sourceFlareDocument.string());
        return false;
    }
    const json& meta = doc["meta"];

    // Globals.
    json globals{
        {"globalScale", ReadFlexFloat(meta, "GlobalScale", 1.0f)},
        {"globalBrightness", ReadFlexFloat(meta, "GlobalBrightness", 1.0f)},
        {"globalTint", ColorToJson(ReadColor(meta, "GlobalTintColor", {}))},
        {"useDistanceFade", ReadFlexBool(meta, "useDistanceFade", false)},
        {"useDistanceScale", ReadFlexBool(meta, "useDistanceScale", false)},
        {"useMaxDistance", ReadFlexBool(meta, "useMaxDistance", false)},
        {"maxDistance", ReadFlexFloat(meta, "GlobalMaxDistance", 150.0f)},
        {"useAngleLimit", ReadFlexBool(meta, "UseAngleLimit", false)},
        {"maxAngle", ReadFlexFloat(meta, "maxAngle", 90.0f)},
        {"useAngleScale", ReadFlexBool(meta, "UseAngleScale", false)},
        {"useAngleBrightness", ReadFlexBool(meta, "UseAngleBrightness", false)},
        {"useAngleCurve", ReadFlexBool(meta, "UseAngleCurve", false)},
        {"angleCurve", ReadCurveKeys(meta, "AngleCurve")},
        {"dynamicEdgeCurve", ReadCurveKeys(meta, "DynamicEdgeCurve")},
        {"multiplyScaleByTransformScale", ReadFlexBool(meta, "MultiplyScaleByTransformScale", false)},
        {"offScreenFadeDist", ReadFlexFloat(meta, "OffScreenFadeDist", 0.4f)},
        {"useDynamicEdgeBoost", ReadFlexBool(meta, "useDynamicEdgeBoost", false)},
        {"dynamicEdgeBrightness", ReadFlexFloat(meta, "DynamicEdgeBrightness", 0.1f)},
        {"dynamicEdgeRange", ReadFlexFloat(meta, "DynamicEdgeRange", 0.3f)},
        {"dynamicEdgeBias", ReadFlexFloat(meta, "DynamicEdgeBias", -0.1f)},
        // The source's numeric "DynamicEdgeBoost" is a SCALE boost (its
        // brightness ramp is the separate DynamicEdgeBrightness) — map it onto
        // the engine's edge-scale ramp, sharing the edge range/bias.
        {"useDynamicEdgeScale", ReadFlexBool(meta, "useDynamicEdgeBoost", false)},
        {"dynamicEdgeScale", ReadFlexFloat(meta, "DynamicEdgeBoost", 0.0f)},
        {"dynamicEdgeScaleRange", ReadFlexFloat(meta, "DynamicEdgeRange", 0.3f)},
        {"dynamicEdgeScaleBias", ReadFlexFloat(meta, "DynamicEdgeBias", 0.0f)},
        {"useDynamicCenterBoost", ReadFlexBool(meta, "useDynamicCenterBoost", false)},
        {"dynamicCenterBrightness", ReadFlexFloat(meta, "DynamicCenterBrightness", 0.0f)},
        // Numeric "DynamicCenterBoost" is a SIZE boost, like the edge one.
        {"dynamicCenterScale", ReadFlexFloat(meta, "DynamicCenterBoost", 0.0f)},
        {"dynamicCenterRange", ReadFlexFloat(meta, "DynamicCenterRange", 0.3f)},
        {"dynamicCenterBias", ReadFlexFloat(meta, "DynamicCenterBias", 0.0f)},
        {"neverCull", ReadFlexBool(meta, "neverCull", false)},
    };

    // Elements. The source stores them as an "Elements" object keyed
    // "Element0", "Element1", ... — preserve that order.
    json elements = json::array();
    if (meta.contains("Elements") && meta["Elements"].is_object())
    {
        const json& elems = meta["Elements"];
        for (int i = 0;; ++i)
        {
            const std::string key = "Element" + std::to_string(i);
            auto it = elems.find(key);
            if (it == elems.end())
                break;
            const json& e = *it;

            // The authored static offset is the TYPO field "OffsetPostion"; the
            // correctly-spelled "OffsetPosition" is a runtime scratch value the
            // source tool overwrites every frame with the element's computed
            // screen position — importing it would bake a stale camera framing
            // into the asset. Anamorphic is authored as {r,g,b}; take x,y.
            const LensFlare::Color off = ReadColor(e, "OffsetPostion", {0, 0, 0, 0});
            const LensFlare::Color ana = ReadColor(e, "Anamorphic", {0, 0, 0, 0});

            // Per-axis base size ("size" {x,y}): the sprite's authored aspect /
            // streak shape, multiplied into the quad extents alongside Scale.
            float sizeX = 1.0f;
            float sizeY = 1.0f;
            if (auto sz = e.find("size"); sz != e.end() && sz->is_object())
            {
                sizeX = ReadFlexFloat(*sz, "x", 1.0f);
                sizeY = ReadFlexFloat(*sz, "y", 1.0f);
            }

            // Sprite binding: by name, falling back to the legacy numeric
            // elementTextureID (an index into the atlas's frame order) for
            // flares exported before SpriteName existed.
            std::string spriteName = e.value("SpriteName", std::string{});
            if (spriteName.empty() && atlasSpriteNames)
            {
                const int texId = static_cast<int>(ReadFlexFloat(e, "elementTextureID", -1.0f));
                if (texId >= 0 && texId < static_cast<int>(atlasSpriteNames->size()))
                    spriteName = (*atlasSpriteNames)[static_cast<size_t>(texId)];
            }

            // Per-element dynamic-boost overrides: emit only when the authored
            // Override* flag is set — absent means "inherit the flare global".
            const auto overrideOrSkip = [&](json& outEl, const char* outKey, const char* flagKey,
                                            const char* valueKey) {
                if (ReadFlexBool(e, flagKey, false))
                    outEl[outKey] = ReadFlexFloat(e, valueKey, 0.0f);
            };

            // Shared element fields; per-emit overrides applied below.
            const auto makeElement = [&](float srcPosition, float angle, float scaleMul,
                                         const LensFlare::Color& tint) {
                json el{
                    {"sprite", spriteName},
                    {"visible", ReadFlexBool(e, "Visible", true)},
                    {"brightness", ReadFlexFloat(e, "Brightness", 1.0f)},
                    {"scale", ReadFlexFloat(e, "Scale", 1.0f) * scaleMul},
                    {"sizeX", sizeX},
                    {"sizeY", sizeY},
                    // Source convention: element pos = -position * sourceScreenPos
                    // (-1 = at the source, 0 = screen center, +1 = mirrored).
                    // Engine convention: pos = source + position * (center - source)
                    // (0 = at the source, 1 = center, 2 = mirrored) — shift by +1.
                    {"position", srcPosition + 1.0f},
                    {"offsetX", off.R},
                    {"offsetY", off.G},
                    {"anamorphicX", ana.R},
                    {"anamorphicY", ana.G},
                    {"angle", angle},
                    {"useStarRotation", ReadFlexBool(e, "useStarRotation", false)},
                    {"rotateToFlare", ReadFlexBool(e, "rotateToFlare", false)},
                    {"rotationSpeed", ReadFlexFloat(e, "rotationSpeed", 0.0f)},
                    {"tint", ColorToJson(tint)},
                };
                overrideOrSkip(el, "edgeBrightnessBoost", "OverrideDynamicEdgeBrightness",
                               "DynamicEdgeBrightnessOverride");
                overrideOrSkip(el, "centerBrightnessBoost", "OverrideDynamicCenterBrightness",
                               "DynamicCenterBrightnessOverride");
                overrideOrSkip(el, "edgeScaleBoost", "OverrideDynamicEdgeBoost",
                               "DynamicEdgeBoostOverride");
                overrideOrSkip(el, "centerScaleBoost", "OverrideDynamicCenterBoost",
                               "DynamicCenterBoostOverride");
                return el;
            };

            const float elementPosition = ReadFlexFloat(e, "position", 0.0f);
            const bool isMulti = e.value("type", std::string{"0"}) == "1";
            const auto subs = e.find("subElements");

            if (isMulti && subs != e.end() && subs->is_object())
            {
                // Multi elements draw ONLY their sub-sprites. The source doc
                // ships each sub's randomized position/angle/scale/color baked
                // in, so the expansion is deterministic — one engine element
                // per sub. `useRangeOffset` picks per-sub positions along the
                // axis; off means every sub stacks at the parent's position
                // (e.g. a burst of randomly rotated streaks at the source).
                const bool rangeOffset = ReadFlexBool(e, "useRangeOffset", false);
                for (int s = 0;; ++s)
                {
                    auto subIt = subs->find("subElement" + std::to_string(s));
                    if (subIt == subs->end())
                        break;
                    const json& sub = *subIt;
                    const float subPos = rangeOffset
                                             ? ReadFlexFloat(sub, "position", elementPosition)
                                             : elementPosition;
                    elements.push_back(makeElement(
                        subPos, ReadFlexFloat(sub, "angle", 0.0f),
                        ReadFlexFloat(sub, "scale", 1.0f), ReadColor(sub, "color", {})));
                }
            }
            else
            {
                elements.push_back(makeElement(elementPosition,
                                               ReadFlexFloat(e, "angle", 0.0f), 1.0f,
                                               ReadColor(e, "ElementTint", {})));
            }
        }
    }

    json out{{"atlas", atlasRef}, {"globals", std::move(globals)}, {"elements", std::move(elements)}};
    return WriteFile(outLensFlarePath,
                     out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace),
                     outError);
}

int ImportFolder(const std::filesystem::path& sourceDir, const std::filesystem::path& outputDir,
                 const std::string& outputMountPrefix, std::vector<FolderImportEntry>& outReport)
{
    namespace fs = std::filesystem;

    // Classify every .txt under the source: TexturePacker atlas docs have a
    // "frames" map; flare docs have a "meta" block with "Elements".
    struct AtlasSrc
    {
        fs::path Doc;
        fs::path Png;
        std::string Name; // whitespace-stripped stem
        std::vector<std::string> Sprites;
    };
    struct FlareSrc
    {
        fs::path Doc;
        std::string Name;
        std::vector<std::string> Sprites;
    };
    std::vector<AtlasSrc> atlases;
    std::vector<FlareSrc> flares;

    const auto stripName = [](const fs::path& p) {
        std::string n = p.stem().string();
        n.erase(std::remove_if(n.begin(), n.end(), [](unsigned char c) { return std::isspace(c); }),
                n.end());
        return n;
    };

    std::error_code ec;
    for (fs::recursive_directory_iterator it(sourceDir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().extension() != ".txt")
            continue;
        const fs::path& doc = it->path();
        json parsed = json::parse(MakeStrictJson(ReadFile(doc)), nullptr,
                                  /*allow_exceptions*/ false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            outReport.push_back({doc, {}, "skipped (not a data document)", false});
            continue; // readme / non-data text file
        }

        if (parsed.contains("frames") && parsed["frames"].is_object())
        {
            AtlasSrc a;
            a.Doc = doc;
            a.Name = stripName(doc);
            for (const auto& [key, unused] : parsed["frames"].items())
                a.Sprites.push_back(StripSpriteExtension(key));
            // The texture is the same-basename .png next to the doc.
            a.Png = doc;
            a.Png.replace_extension(".png");
            if (!fs::exists(a.Png, ec))
            {
                outReport.push_back({doc, {}, "atlas doc without a matching .png", false});
                continue;
            }
            atlases.push_back(std::move(a));
        }
        else if (parsed.contains("meta") && parsed["meta"].is_object() &&
                 parsed["meta"].contains("Elements"))
        {
            FlareSrc f;
            f.Doc = doc;
            f.Name = stripName(doc);
            for (const auto& [key, e] : parsed["meta"]["Elements"].items())
                if (e.is_object() && e.contains("SpriteName") && e["SpriteName"].is_string())
                    f.Sprites.push_back(e["SpriteName"].get<std::string>());
            flares.push_back(std::move(f));
        }
    }

    fs::create_directories(outputDir, ec);
    const std::string prefix =
        outputMountPrefix.empty() || outputMountPrefix.back() == '/' ? outputMountPrefix
                                                                     : outputMountPrefix + "/";

    int written = 0;

    // Atlases first (flares bind to them). Texture is copied next to the atlas.
    struct ImportedAtlas
    {
        std::string Ref; // mount-relative .flareatlas path
        std::vector<std::string> Sprites;
    };
    std::vector<ImportedAtlas> imported;
    for (const AtlasSrc& a : atlases)
    {
        const fs::path pngOut = outputDir / (a.Name + ".png");
        const fs::path atlasOut = outputDir / (a.Name + ".flareatlas");
        fs::copy_file(a.Png, pngOut, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            outReport.push_back({a.Doc, {}, "texture copy failed: " + ec.message(), false});
            ec.clear();
            continue;
        }
        std::string err;
        if (!ImportAtlas(a.Doc, prefix + a.Name + ".png", atlasOut, &err))
        {
            outReport.push_back({a.Doc, {}, err, false});
            continue;
        }
        imported.push_back({prefix + a.Name + ".flareatlas", a.Sprites});
        outReport.push_back({a.Doc, atlasOut, "atlas (" + std::to_string(a.Sprites.size()) + " sprites)", true});
        ++written;
    }

    // Flares bind to the imported atlas covering the most of their sprites.
    for (const FlareSrc& f : flares)
    {
        const ImportedAtlas* best = nullptr;
        size_t bestCovered = 0;
        for (const ImportedAtlas& a : imported)
        {
            size_t covered = 0;
            for (const std::string& s : f.Sprites)
                if (std::find(a.Sprites.begin(), a.Sprites.end(), s) != a.Sprites.end())
                    ++covered;
            if (covered > bestCovered || (!best && !imported.empty()))
            {
                best = &a;
                bestCovered = covered;
            }
        }
        if (!best)
        {
            outReport.push_back({f.Doc, {}, "no atlas imported to bind against", false});
            continue;
        }
        const fs::path flareOut = outputDir / (f.Name + ".lensflare");
        std::string err;
        if (!ImportFlare(f.Doc, best->Ref, flareOut, &err, &best->Sprites))
        {
            outReport.push_back({f.Doc, {}, err, false});
            continue;
        }
        std::string note = "flare -> " + best->Ref;
        if (bestCovered < f.Sprites.size())
            note += " (" + std::to_string(f.Sprites.size() - bestCovered) + " sprite(s) unresolved)";
        outReport.push_back({f.Doc, flareOut, note, true});
        ++written;
    }

    return written;
}

} // namespace GameEngine::LensFlareImport
