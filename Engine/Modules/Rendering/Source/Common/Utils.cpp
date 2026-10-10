#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace GameEngine::Rendering
{

namespace Utils
{

namespace
{
static ShaderFileLoaderFunc g_shaderFileLoader = nullptr;
static ShaderPathResolverFunc g_shaderPathResolver = nullptr;
}

void SetShaderFileLoader(ShaderFileLoaderFunc loader)
{
    g_shaderFileLoader = loader;
}

ShaderFileLoaderFunc GetShaderFileLoader()
{
    return g_shaderFileLoader;
}

ShaderPathResolverFunc GetShaderPathResolver()
{
    return g_shaderPathResolver;
}

void SetShaderPathResolver(ShaderPathResolverFunc resolver)
{
    g_shaderPathResolver = resolver;
}

bool HasShaderPathResolver()
{
    return g_shaderPathResolver != nullptr;
}

std::string FormatString(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(nullptr, 0, format, args);
    va_end(args);
    if (n <= 0)
        return {};
    std::vector<char> buf(static_cast<size_t>(n) + 1);
    va_start(args, format);
    vsnprintf(buf.data(), buf.size(), format, args);
    va_end(args);
    return std::string(buf.data(), static_cast<size_t>(n));
}

std::vector<std::string> SplitString(const std::string& str, char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;

    while (std::getline(ss, token, delimiter))
    {
        tokens.push_back(token);
    }

    return tokens;
}

std::string ToUpper(const std::string& str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c)
                   { return static_cast<char>(::toupper(c)); });
    return result;
}

bool FileExists(const std::string& path) noexcept
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return {};
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    return data;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

namespace
{
// Host loader first, then the path resolver — the order every shader load
// uses. std::nullopt means the name resolved nowhere; an empty vector means it
// resolved to an empty file, which callers already treat as "unavailable".
std::optional<std::vector<uint8_t>> TryLoadShaderBlob(const char* name)
{
    if (g_shaderFileLoader)
    {
        auto out = g_shaderFileLoader(name);
        if (!out.empty())
            return out;
    }

    const std::string resolved = ResolveShaderPath(name);
    if (!resolved.empty())
        return ReadFile(resolved);
    return std::nullopt;
}
} // namespace

static ShaderSourceKind g_preferredShaderSource = ShaderSourceKind::SpirV;

void SetPreferredShaderSource(ShaderSourceKind kind)
{
    g_preferredShaderSource = kind;
}

std::vector<uint8_t> LoadShaderFile(const char* name)
{
    if (!name || !name[0])
        return {};

    // A device that ingests WGSL never reads SPIR-V. Stage shaders outside the
    // .shaderpkg path — the UI SDF set, the editor's line and selection
    // overlays — are still asked for by their .spv name, so the cooked
    // sibling answers first and the backend's blob sniffer takes it from
    // there. Falling through to the .spv name keeps a desktop-shaped staging
    // tree resolving unchanged.
    if (g_preferredShaderSource == ShaderSourceKind::Wgsl)
    {
        const std::string_view requested(name);
        constexpr std::string_view kSpvSuffix = ".spv";
        if (requested.size() > kSpvSuffix.size() &&
            requested.compare(requested.size() - kSpvSuffix.size(), kSpvSuffix.size(), kSpvSuffix) == 0)
        {
            const std::string wgslName =
                std::string(requested.substr(0, requested.size() - kSpvSuffix.size())) + ".wgsl";
            auto wgsl = TryLoadShaderBlob(wgslName.c_str());
            if (wgsl && !wgsl->empty())
                return std::move(*wgsl);
        }
    }

    auto blob = TryLoadShaderBlob(name);
    if (blob)
        return std::move(*blob);

    // By value, not a throw: a runtime linked without exception catching would
    // abort here, and every caller already treats an empty blob as "this shader
    // is unavailable" and degrades (the GPU culling path disables itself, exactly
    // as it does when the package is missing).
    Logger::Log::Error("LoadShaderFile('{}'): shader not found. Ensure the file exists and a "
                       "resolver or file loader is configured.", name);
    return {};
}

std::string ResolveShaderPath(const char* name)
{
    namespace fs = std::filesystem;
    if (!name || !name[0])
        return {};

    fs::path in(name);

    if (in.is_absolute())
    {
        std::error_code ec;
        if (fs::exists(in, ec))
            return in.string();
        return {};
    }

    if (!g_shaderPathResolver)
    {
        Logger::Log::Error("ResolveShaderPath('{}'): no shader path resolver configured. Call "
                           "Utils::SetShaderPathResolver() before resolving shader paths.", name);
        return {};
    }

    fs::path resolved = g_shaderPathResolver(in);
    if (!resolved.empty())
    {
        std::error_code ec;
        if (fs::exists(resolved, ec))
            return resolved.string();
    }

    return {};
}

size_t HashString(const std::string& str) noexcept
{
    return HashBytes(str.data(), str.size());
}

size_t HashBytes(const void* data, size_t size) noexcept
{
    const uint8_t* b = static_cast<const uint8_t*>(data);
    if constexpr (sizeof(size_t) == 8)
    {
        size_t h = 14695981039346656037ull;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
        return h;
    }
    else
    {
        size_t h = 2166136261u;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= b[i];
            h *= 16777619u;
        }
        return h;
    }
}

double GetCurrentTimeSeconds() noexcept
{
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

uint64_t GetCurrentTimeMilliseconds() noexcept
{
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

uint64_t GetCurrentTimeMicroseconds() noexcept
{
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

} // namespace Utils

} // namespace GameEngine::Rendering
