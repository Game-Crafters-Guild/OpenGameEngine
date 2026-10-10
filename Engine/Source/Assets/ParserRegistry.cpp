#include "Assets/ParserRegistry.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/Parsers/AnimationAssetParser.h"
#include "Assets/Parsers/AnimationControllerAssetParser.h"
#include "Assets/Parsers/AnimationLibraryAssetParser.h"
#include "Assets/Parsers/AudioAssetParser.h"
#include "Assets/Parsers/CppSourceAssetParser.h"
#include "Assets/Parsers/JpegTextureParser.h"
#include "Assets/Parsers/Ktx2TextureParser.h"
#include "Assets/Parsers/ModelAssetParser.h"
#include "Assets/Parsers/OtherTextureParser.h"
#include "Assets/Parsers/PngTextureParser.h"
#include "Assets/Parsers/MaterialAssetParser.h"
#include "Assets/Parsers/LensFlareParsers.h"
#include "Assets/Parsers/TerrainMaterialLibraryParser.h"
#include "Particles/Assets/ParticleStackParser.h"
#include "Assets/Parsers/TimelineAssetParser.h"
#include "Assets/Parsers/AnimationGraphAssetParser.h"
#include "Assets/Parsers/ClipSetAssetParser.h"
#include "Assets/Parsers/NavGridAssetParser.h"
#include "Assets/Parsers/NavMeshAssetParser.h"
#include "Assets/Parsers/RenderPipelineAssetParser.h"
#include "Assets/Parsers/SceneAssetParser.h"
#include "Assets/Parsers/SpriteFramesAssetParser.h"
#include "Assets/Parsers/UILayoutAssetParser.h"
#include "Assets/Parsers/UIStyleAssetParser.h"
#include "Assets/Parsers/XmlAssetParser.h"
#include "Core/Application.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace GameEngine
{

// AssetParser base class implementation
bool AssetParser::CanParse(const std::filesystem::path& filePath) const
{
    std::string extension = filePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c)
                   { return static_cast<char>(std::tolower(c)); });

    auto supportedExtensions = GetSupportedExtensions();
    return std::find(supportedExtensions.begin(), supportedExtensions.end(), extension) != supportedExtensions.end();
}

// ParserRegistry implementation
ParserRegistry::ParserRegistry() = default;

bool ParserRegistry::Initialize()
{
    if (m_Initialized)
    {
        Logger::Log::Warning("ParserRegistry already initialized");
        return true;
    }

    Logger::Log::Info("Initializing Parser Registry");

    // Register default parsers
    RegisterDefaultParsers();

    // Optional: dynamic parser plugins.
    // Default location: <exe>/Plugins/AssetParsers
    LoadPluginsFromDirectory(PathUtils::GetExecutableDirectory() / "Plugins" / "AssetParsers");
    LoadPluginsFromEnvironment();

    m_Initialized = true;
    Logger::Log::Info("Parser Registry initialized with {} parsers", GetParserCount());
    return true;
}

void ParserRegistry::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    Logger::Log::Info("Shutting down Parser Registry");

    m_ExtensionParsers.clear();
    m_Registrations.clear();

    // Unload plugin libraries last.
    for (void* h : m_PluginHandles)
    {
        if (!h)
            continue;
#ifdef PLATFORM_WINDOWS
        FreeLibrary((HMODULE)h);
#else
        dlclose(h);
#endif
    }
    m_PluginHandles.clear();

    m_Initialized = false;
}

namespace
{
static std::vector<std::filesystem::path> SplitPluginDirs(const std::string& s)
{
    std::vector<std::filesystem::path> out;
    size_t start = 0;
    while (start < s.size())
    {
        size_t end = s.find(';', start);
        if (end == std::string::npos)
            end = s.size();
        std::string part = s.substr(start, end - start);
        // trim spaces
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
            part.erase(part.begin());
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
            part.pop_back();
        if (!part.empty())
            out.emplace_back(part);
        start = end + 1;
    }
    return out;
}

static bool IsPluginLibraryFile(const std::filesystem::path& p)
{
    const std::string ext = p.extension().string();
#ifdef PLATFORM_WINDOWS
    return ext == ".dll" || ext == ".DLL";
#elif defined(__APPLE__)
    return ext == ".dylib";
#else
    return ext == ".so";
#endif
}

static void* LoadNativeLibrary(const std::filesystem::path& p)
{
#ifdef PLATFORM_WINDOWS
    return (void*)LoadLibraryW(p.wstring().c_str());
#else
    return dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* GetExport(void* handle, const char* name)
{
#ifdef PLATFORM_WINDOWS
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}
} // namespace

bool ParserRegistry::LoadPluginsFromDirectory(const std::filesystem::path& directory)
{
    if (directory.empty())
    {
        return true;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
    {
        return true; // missing is fine
    }

    using RegisterFn = void (*)(GameEngine::ParserRegistry*);

    size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec))
            continue;
        const auto p = entry.path();
        if (!IsPluginLibraryFile(p))
            continue;

        void* lib = LoadNativeLibrary(p);
        if (!lib)
        {
            Logger::Log::Warning("ParserRegistry: failed to load parser plugin '{}'", p.string());
            continue;
        }

        void* sym = GetExport(lib, "GE_RegisterAssetParsers");
        if (!sym)
        {
            Logger::Log::Warning("ParserRegistry: '{}' does not export GE_RegisterAssetParsers; skipping", p.string());
#ifdef PLATFORM_WINDOWS
            FreeLibrary((HMODULE)lib);
#else
            dlclose(lib);
#endif
            continue;
        }

        auto fn = reinterpret_cast<RegisterFn>(sym);
        try
        {
            fn(this);
            m_PluginHandles.push_back(lib);
            ++loaded;
            Logger::Log::Info("ParserRegistry: loaded parser plugin '{}'", p.filename().string());
        }
        catch (...)
        {
            Logger::Log::Warning("ParserRegistry: exception while registering parsers from '{}'", p.string());
#ifdef PLATFORM_WINDOWS
            FreeLibrary((HMODULE)lib);
#else
            dlclose(lib);
#endif
        }
    }

    if (loaded > 0)
    {
        Logger::Log::Info("ParserRegistry: loaded {} parser plugin(s) from '{}'", loaded, directory.string());
    }
    return true;
}

void ParserRegistry::LoadPluginsFromEnvironment()
{
    const char* env = std::getenv("GE_ASSET_PARSER_PLUGINS_DIR");
    if (!env || !*env)
    {
        return;
    }

    const std::string s(env);
    const auto dirs = SplitPluginDirs(s);
    for (const auto& d : dirs)
    {
        (void)LoadPluginsFromDirectory(d);
    }
}

bool ParserRegistry::RegisterParser(std::shared_ptr<AssetParser> parser, int priority)
{
    if (!parser)
    {
        Logger::Log::Error("Cannot register null parser");
        return false;
    }

    const std::vector<std::string> extensions = parser->GetSupportedExtensions();
    if (extensions.empty())
    {
        Logger::Log::Error("Cannot register parser '{}': GetSupportedExtensions() returned no extensions",
                           parser->GetName());
        return false;
    }

    const int effectivePriority = (priority != 0) ? priority : parser->GetPriority();

    // Add to registrations
    m_Registrations.emplace_back(parser, extensions, effectivePriority);

    // Register for each extension
    for (const auto& ext : extensions)
    {
        std::string normalizedExt = NormalizeExtension(ext);
        m_ExtensionParsers[normalizedExt].push_back(ParserEntry{parser, effectivePriority});

        // Sort by priority (highest first)
        SortParsersByPriority(m_ExtensionParsers[normalizedExt]);
    }

    Logger::Log::Debug("Registered parser '{}' for extensions: {}",
                       parser->GetName(),
                       [&extensions]()
                       {
                           std::string result;
                           for (size_t i = 0; i < extensions.size(); ++i)
                           {
                               if (i > 0)
                                   result += ", ";
                               result += extensions[i];
                           }
                           return result;
                       }());

    return true;
}

void ParserRegistry::UnregisterParser(std::shared_ptr<AssetParser> parser)
{
    if (!parser)
    {
        return;
    }

    // Remove from registrations
    m_Registrations.erase(
        std::remove_if(m_Registrations.begin(), m_Registrations.end(),
                       [&parser](const ParserRegistration& reg)
                       {
                           return reg.Parser == parser;
                       }),
        m_Registrations.end());

    // Remove from extension mappings
    for (auto& [extension, parsers] : m_ExtensionParsers)
    {
        parsers.erase(std::remove_if(parsers.begin(), parsers.end(),
                                     [&parser](const ParserEntry& e)
                                     { return e.Parser == parser; }),
                      parsers.end());
    }

    Logger::Log::Debug("Unregistered parser '{}'", parser->GetName());
}

AssetType ParserRegistry::GetAssetTypeFromExtension(const std::string& extension) const
{
    std::string normalizedExt = NormalizeExtension(extension);

    auto it = m_ExtensionParsers.find(normalizedExt);
    if (it != m_ExtensionParsers.end() && !it->second.empty())
    {
        // Return the asset type of the highest priority parser (extension-only; no sniffing).
        return it->second[0].Parser ? it->second[0].Parser->GetAssetType() : AssetType::Unknown;
    }

    return AssetType::Unknown;
}

std::shared_ptr<AssetParser> ParserRegistry::FindParser(const std::filesystem::path& filePath) const
{
    std::string extension = NormalizeExtension(filePath.extension().string());

    auto it = m_ExtensionParsers.find(extension);
    if (it != m_ExtensionParsers.end() && !it->second.empty())
    {
        // Return the highest priority parser that can parse this file
        for (const auto& entry : it->second)
        {
            if (entry.Parser && entry.Parser->CanParse(filePath))
            {
                return entry.Parser;
            }
        }
    }

    return nullptr;
}

std::filesystem::path ParserRegistry::ResolveReadPath(const AssetMetadata& metadata, const AssetManager& assetManager) const
{
    const auto parser = FindParser(metadata.Path);
    return parser ? parser->ResolveReadPath(metadata, assetManager) : metadata.Path;
}

std::vector<std::shared_ptr<AssetParser>> ParserRegistry::FindParsers(const std::string& extension) const
{
    std::string normalizedExt = NormalizeExtension(extension);

    auto it = m_ExtensionParsers.find(normalizedExt);
    if (it != m_ExtensionParsers.end())
    {
        std::vector<std::shared_ptr<AssetParser>> out;
        out.reserve(it->second.size());
        for (const auto& e : it->second)
        {
            out.push_back(e.Parser);
        }
        return out; // Already sorted by priority
    }

    return {};
}

AssetParseResult ParserRegistry::ParseAsset(const AssetMetadata& metadata, AssetManager& assetManager) const
{
    const std::filesystem::path filePath = metadata.Path;
    const std::string extension = NormalizeExtension(filePath.extension().string());

    auto it = m_ExtensionParsers.find(extension);
    if (it == m_ExtensionParsers.end() || it->second.empty())
    {
        return AssetParseResult(false, "No parser found for file: " + filePath.string());
    }

    // Try parsers registered for this extension in priority order.
    // IMPORTANT: this only probes parsers mapped for this extension (optimized dispatch).
    for (const auto& entry : it->second)
    {
        if (!entry.Parser)
        {
            continue;
        }

        // Probe/classify: allow parsers to reject a file even when extension matches (sniffing).
        if (!entry.Parser->CanParse(filePath))
        {
            continue;
        }

        try
        {
            AssetParseResult result = entry.Parser->Parse(metadata, assetManager);
            if (result.Success || result.Status == AssetParseStatus::Success)
            {
                result.Status = AssetParseStatus::Success;
                result.Success = true;
                return result;
            }

            // Parser explicitly declined to handle; try the next parser for this extension.
            if (result.Status == AssetParseStatus::NotForMe)
            {
                continue;
            }

            // Error: stop on the first error by default (safer than silently falling back).
            result.Status = AssetParseStatus::Error;
            result.Success = false;
            return result;
        }
        catch (const std::exception& e)
        {
            AssetParseResult err(false, "Parser error (" + entry.Parser->GetName() + "): " + std::string(e.what()));
            err.Status = AssetParseStatus::Error;
            return err;
        }
    }

    // All parsers either failed probe (sniffing) or returned NotForMe.
    AssetParseResult out(false, "No parser could handle file: " + filePath.string());
    out.Status = AssetParseStatus::Error;
    return out;
}

std::vector<ParserRegistration> ParserRegistry::GetAllParsers() const
{
    return m_Registrations;
}

size_t ParserRegistry::GetParserCount() const
{
    return m_Registrations.size();
}

bool ParserRegistry::HasParserForExtension(const std::string& extension) const
{
    std::string normalizedExt = NormalizeExtension(extension);
    auto it = m_ExtensionParsers.find(normalizedExt);
    return it != m_ExtensionParsers.end() && !it->second.empty();
}

void ParserRegistry::RegisterDefaultParsers()
{
    // Register texture parsers (per-format to prove multi-parser/extension mapping)
    RegisterParser(std::make_shared<PngTextureParser>());
    RegisterParser(std::make_shared<JpegTextureParser>());
    RegisterParser(std::make_shared<Ktx2TextureParser>());
    RegisterParser(std::make_shared<OtherTextureParser>());

    // Register UI parsers (UI module assets)
    RegisterParser(std::make_shared<UILayoutAssetParser>());
    RegisterParser(std::make_shared<UIStyleAssetParser>());

    // Register generic XML parser (only used when UILayout sniffing rejects .xml)
    RegisterParser(std::make_shared<XmlAssetParser>());

    // Register model parsers (glTF/FBX/Blend). glTF decodes through cgltf, which
    // builds for wasm, so the web runtime imports models at load time.
    RegisterParser(std::make_shared<ModelAssetParser>());

    // Register audio parsers
    RegisterParser(std::make_shared<AudioAssetParser>());

    // Register animation parsers
    RegisterParser(std::make_shared<AnimationAssetParser>());
    RegisterParser(std::make_shared<AnimationLibraryAssetParser>());
    RegisterParser(std::make_shared<AnimationControllerAssetParser>());
    RegisterParser(std::make_shared<SpriteFramesAssetParser>());

    // Register render pipeline parser
    RegisterParser(std::make_shared<RenderPipelineAssetParser>());

    // Register navigation parsers
    RegisterParser(std::make_shared<NavGridAssetParser>());
    RegisterParser(std::make_shared<NavMeshAssetParser>());

    // Register material parser (.material -> Material v2 doc)
    RegisterParser(std::make_shared<MaterialAssetParser>());

    // Register scene parser (.scene / .blueprint -> SceneAsset, INI-style).
    RegisterParser(std::make_shared<SceneAssetParser>());

    // Register lens-flare parsers (.flareatlas / .lensflare JSON).
    RegisterParser(std::make_shared<FlareAtlasParser>());
    RegisterParser(std::make_shared<LensFlareDefinitionParser>());

    // Register the terrain material library parser (.terrainmatlib JSON).
    RegisterParser(std::make_shared<TerrainMaterialLibraryParser>());

    // Register the particle stack parser (.particlestack, JSON or cooked).
    RegisterParser(std::make_shared<Particles::ParticleStackParser>());

    // Register timeline + clipset parsers (.timeline / .clipset JSON,
    // composite multi-track timelines and lane-based clip arrangements).
    RegisterParser(std::make_shared<TimelineAssetParser>());
    RegisterParser(std::make_shared<AnimationGraphAssetParser>());
    RegisterParser(std::make_shared<ClipSetAssetParser>());

    // Native C++ user-script source (.cpp/.h/.hpp) — classifies as NativeSource.
    RegisterParser(std::make_shared<CppSourceAssetParser>());

    // Register binary asset parser (low priority fallback)
    RegisterParser(std::make_shared<BinaryAssetParser>());
}

std::string ParserRegistry::NormalizeExtension(const std::string& extension) const
{
    std::string normalized = extension;

    // Convert to lowercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](char c)
                   { return static_cast<char>(std::tolower(c)); });

    // Ensure it starts with '.'
    if (!normalized.empty() && normalized[0] != '.')
    {
        normalized = "." + normalized;
    }

    return normalized;
}

void ParserRegistry::SortParsersByPriority(std::vector<ParserEntry>& parsers) const
{
    // Stable sort to keep registration order deterministic among equal priorities.
    std::stable_sort(parsers.begin(), parsers.end(),
                     [](const ParserEntry& a, const ParserEntry& b)
                     {
                         return a.Priority > b.Priority;
                     });
}

} // namespace GameEngine
