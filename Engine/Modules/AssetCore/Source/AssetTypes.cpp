#include "AssetCore/AssetTypes.h"
#include <algorithm>
#include <unordered_map>
#include <cctype>

namespace GameEngine {

namespace
{
static String NormalizeExtensionLower(String ext)
{
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    if (!ext.empty() && ext[0] != '.')
    {
        ext.insert(ext.begin(), '.');
    }

    return ext;
}

static const std::unordered_map<String, AssetType>& GetExtensionMap()
{
    static const std::unordered_map<String, AssetType> extensionMap = {
        // Texture formats
        {".png", AssetType::Texture},
        {".svg", AssetType::Texture},
        {".jpg", AssetType::Texture},
        {".jpeg", AssetType::Texture},
        {".bmp", AssetType::Texture},
        {".tga", AssetType::Texture},
        {".dds", AssetType::Texture},
        {".hdr", AssetType::Texture},
        {".exr", AssetType::Texture},
        {".ktx", AssetType::Texture},
        {".ktx2", AssetType::Texture},
        {".cube", AssetType::CubeLut},

        // Model formats
        {".obj", AssetType::Model},
        {".fbx", AssetType::Model},
        {".gltf", AssetType::Model},
        {".glb", AssetType::Model},
        {".blend", AssetType::Model},

        // Audio formats
        {".wav", AssetType::Audio},
        {".mp3", AssetType::Audio},
        {".ogg", AssetType::Audio},
        {".flac", AssetType::Audio},
        {".aac", AssetType::Audio},

        // Script formats
        {".cs", AssetType::Script},
        {".lua", AssetType::Script},
        {".js", AssetType::Script},

        // Native C/C++ user-script source (compiled into a hot-reloadable user DLL)
        {".cpp", AssetType::NativeSource},
        {".cc", AssetType::NativeSource},
        {".cxx", AssetType::NativeSource},
        {".c", AssetType::NativeSource},
        {".h", AssetType::NativeSource},
        {".hpp", AssetType::NativeSource},
        {".hxx", AssetType::NativeSource},
        {".hh", AssetType::NativeSource},

        // Scene formats
        {".scene", AssetType::Scene},
        {".unity", AssetType::Scene},

        // Material formats
        {".mat", AssetType::Material},
        {".material", AssetType::Material},
        {".mtlx", AssetType::Material}, // MaterialX (OpenPBR surface) -> imported StandardPBR material

        // Shader formats
        {".hlsl", AssetType::Shader},
        {".glsl", AssetType::Shader},
        {".vert", AssetType::Shader},
        {".frag", AssetType::Shader},
        {".geom", AssetType::Shader},
        {".comp", AssetType::Shader},
        {".shader", AssetType::Shader},

        // Render pipeline / render graph formats
        {".rendergraph", AssetType::RenderPipeline},
        {".renderpipeline", AssetType::RenderPipeline},

        // Font formats
        {".ttf", AssetType::Font},
        {".otf", AssetType::Font},
        {".woff", AssetType::Font},
        {".woff2", AssetType::Font},

        // Animation formats
        {".anim", AssetType::Animation},
        {".animation", AssetType::Animation},
        {".animlib", AssetType::AnimationLibrary},
        {".animationlib", AssetType::AnimationLibrary},
        {".animcontroller", AssetType::AnimationController},
        {".animationcontroller", AssetType::AnimationController},
        {".spriteframes", AssetType::SpriteFrames},
        {".timeline", AssetType::Timeline},
        {".animgraph", AssetType::AnimationGraph},
        {".clipset",  AssetType::ClipSet},

        // UI formats
        // Note: .xml is treated as generic XML by default. Content sniffing can
        // re-classify certain .xml files as UILayout (see ParserRegistry).
        {".xml", AssetType::XML},
        {".uxml", AssetType::UILayout},
        {".xaml", AssetType::UILayout},
        {".css", AssetType::UIStyle},
        {".uss", AssetType::UIStyle},

        // Lens flare formats
        {".flareatlas", AssetType::FlareAtlas},
        {".lensflare", AssetType::LensFlareDefinition},

        // Node graph (game logic / material)
        {".graph", AssetType::Graph},

        // Video formats
        {".mp4", AssetType::Video},
        {".mov", AssetType::Video},
        {".avi", AssetType::Video},
        {".mkv", AssetType::Video},
        {".m4v", AssetType::Video},
        {".webm", AssetType::Video},
        {".wmv", AssetType::Video},

        // Navigation formats
        {".navgrid", AssetType::NavigationGrid},
        {".navmesh", AssetType::NavigationMesh},

        // Terrain heightmap raw formats (16-bit PNG heightmaps stay Texture)
        {".r16", AssetType::TerrainHeightmap},
        {".r32", AssetType::TerrainHeightmap},

        // Terrain brush zone payload (sculpt R32F offsets / paint R8 mask)
        {".tzone", AssetType::TerrainZoneData},

        // Planet sphere sculpt page store (sparse dab + modifier pages)
        {".tsculpt", AssetType::TerrainSphereSculptData},

        // A terrain's ordered material list, keyed by stable slot ID
        {".terrainmatlib", AssetType::TerrainMaterialLibrary},

        // A particle emitter's processor stack
        {".particlestack", AssetType::ParticleStack},

        // Ocean formats
        {".oceandepth", AssetType::OceanDepthCache},
        {".oceanspectrum", AssetType::OceanWaveSpectrum},
        {".oceanfft", AssetType::OceanFFTCollision},
        {".oceansettings", AssetType::OceanSettings},
        {".oceanpreset", AssetType::OceanPreset},

        // Humanoid retargeting (Phase 1 of redesign).
        // path.extension() returns only the final segment (.json); use
        // GetCompoundExtensionFromPath() in metadata-extension producers
        // so these compound suffixes flow into the type lookup correctly.
        {".profile.json", AssetType::SkeletonProfile},
        {".humanoidrig.json", AssetType::HumanoidRig},
        {".retargetmap.json", AssetType::RetargetMap},

        // Animation clip sidecars (Phase B3 .blend Action extraction)
        {".anim.json", AssetType::Animation},
    };

    return extensionMap;
}
} // namespace

String AssetTypeToString(AssetType type) {
    switch (type) {
#define GE_ASSET_TYPE_TO_STRING_CASE(name) case AssetType::name: return #name;
        GE_ASSET_TYPE_LIST(GE_ASSET_TYPE_TO_STRING_CASE)
#undef GE_ASSET_TYPE_TO_STRING_CASE
    }
    // Only reachable for a value outside the enum (e.g. an out-of-range integer
    // cast in from a corrupt or newer asset database).
    return "Unknown";
}

AssetType AssetTypeFromString(const String& name) {
    static const std::unordered_map<String, AssetType> nameMap = {
#define GE_ASSET_TYPE_FROM_STRING_ENTRY(entry) {#entry, AssetType::entry},
        GE_ASSET_TYPE_LIST(GE_ASSET_TYPE_FROM_STRING_ENTRY)
#undef GE_ASSET_TYPE_FROM_STRING_ENTRY
    };

    auto it = nameMap.find(name);
    return (it != nameMap.end()) ? it->second : AssetType::Unknown;
}

bool IsRecognizedAssetType(AssetType type) {
    switch (type) {
#define GE_ASSET_TYPE_RECOGNIZED_CASE(name) case AssetType::name:
        GE_ASSET_TYPE_LIST(GE_ASSET_TYPE_RECOGNIZED_CASE)
#undef GE_ASSET_TYPE_RECOGNIZED_CASE
        return true;
    default:
        return false;
    }
}

bool IsClassificationKnown(AssetType type, std::string_view typeId) {
    if (IsRecognizedAssetType(type) && type != AssetType::Unknown) {
        return true;
    }
    // A programmable type id can name a type the enum does not declare, so an
    // empty AssetType alone does not settle it. The one string that never
    // carries an answer is the spelling of Unknown itself.
    return !typeId.empty() && typeId != AssetTypeToString(AssetType::Unknown);
}

AssetType GetAssetTypeFromExtension(const String& extension) {
    const String lowerExt = NormalizeExtensionLower(extension);
    const auto& extensionMap = GetExtensionMap();
    auto it = extensionMap.find(lowerExt);
    return (it != extensionMap.end()) ? it->second : AssetType::Unknown;
}

String GetCompoundExtensionFromPath(const String& path) {
    // Find the trailing filename component (no directory).
    size_t lastSlash = path.find_last_of("/\\");
    String filename = (lastSlash == String::npos) ? path : path.substr(lastSlash + 1);
    if (filename.empty()) return {};

    // Lowercase for case-insensitive map lookup.
    String lowerName = filename;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    const auto& extensionMap = GetExtensionMap();

    // Walk dot positions left-to-right. The first registered match (longest
    // suffix that exists in the map) wins. This allows e.g.
    // 'foo.humanoidrig.json' to resolve to '.humanoidrig.json' rather than
    // collapsing to '.json'.
    for (size_t pos = lowerName.find('.'); pos != String::npos;
         pos = lowerName.find('.', pos + 1))
    {
        String candidate = lowerName.substr(pos);
        if (extensionMap.find(candidate) != extensionMap.end())
            return candidate;
    }

    // No registered match; fall back to the final-segment extension. This
    // preserves prior behaviour for unknown / future extensions and keeps
    // the parser-registry lookup path working unchanged.
    size_t lastDot = lowerName.find_last_of('.');
    if (lastDot == String::npos) return {};
    return lowerName.substr(lastDot);
}

String AssetStateToString(AssetState state) {
    switch (state) {
        case AssetState::Unloaded:  return "Unloaded";
        case AssetState::Loading:   return "Loading";
        case AssetState::Loaded:    return "Loaded";
        case AssetState::Failed:    return "Failed";
        default:                    return "Unknown";
    }
}

String AssetLoadPriorityToString(AssetLoadPriority priority) {
    switch (priority) {
        case AssetLoadPriority::Low:        return "Low";
        case AssetLoadPriority::Normal:     return "Normal";
        case AssetLoadPriority::High:       return "High";
        case AssetLoadPriority::Critical:   return "Critical";
        default:                            return "Unknown";
    }
}

bool IsTextBasedAssetType(AssetType type) {
    switch (type) {
        case AssetType::Script:
        case AssetType::CubeLut:
        case AssetType::Scene:
        case AssetType::Material:
        case AssetType::Shader:
        case AssetType::RenderPipeline:
        case AssetType::UILayout:
        case AssetType::UIStyle:
        case AssetType::Graph:
        case AssetType::XML:
        case AssetType::AnimationLibrary:
        case AssetType::AnimationController:
        case AssetType::SpriteFrames:
        case AssetType::Timeline:
        case AssetType::AnimationGraph:
        case AssetType::ClipSet:
        case AssetType::NavigationMesh:
        case AssetType::OceanWaveSpectrum:
        case AssetType::OceanSettings:
        case AssetType::OceanPreset:
        case AssetType::SkeletonProfile:
        case AssetType::HumanoidRig:
        case AssetType::RetargetMap:
        case AssetType::NativeSource:
        case AssetType::FlareAtlas:
        case AssetType::LensFlareDefinition:
        case AssetType::TerrainMaterialLibrary:
        case AssetType::ParticleStack:
            return true;
        default:
            return false;
    }
}

bool IsBinaryAssetType(AssetType type) {
    return !IsTextBasedAssetType(type) && type != AssetType::Unknown;
}

Vector<String> GetDefaultExtensionsForAssetType(AssetType type) {
    Vector<String> out;
    const auto& extensionMap = GetExtensionMap();
    for (const auto& [ext, mappedType] : extensionMap)
    {
        if (mappedType == type)
        {
            out.push_back(ext);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool IsValidExtensionForAssetType(AssetType type, const String& extension) {
    return GetAssetTypeFromExtension(extension) == type;
}

} // namespace GameEngine
