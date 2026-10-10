#pragma once

#ifndef RENDERING_SOURCE_DIR
#error "RENDERING_SOURCE_DIR must be defined by CMake (target_compile_definitions)"
#endif

#include "Rendering/Common/Utils.h"
#include "StagedTestPaths.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Rendering::Tests
{

// The shader tree as authored, for tests whose subject is the GLSL text itself.
inline std::filesystem::path GetRenderingShadersDir()
{
    return std::filesystem::path(RENDERING_SOURCE_DIR) / "Shaders";
}

// The shader tree as staged beside the build output. A composed material is
// content the engine loads at runtime, so a compose test has to read the copy
// that actually ships — not the source tree, where it would pass on a build
// output that does not carry the shaders at all.
inline std::filesystem::path GetAdapterShaderDir()
{
    return TestPaths::StagedRenderingShadersDir();
}

// Resolve a shader file by name. Compiled SPIR-V is emitted to the build output
// (RENDERING_SHADER_OUTPUT_DIR = CMAKE_BINARY_DIR/Shaders) and reflects the
// CURRENT shaders; the source Shaders dir can hold stale .spv snapshots. Prefer
// the build output when the file exists there, and fall back to the source dir
// (e.g. GLSL/source-text reads that only live in the source tree).
inline std::filesystem::path ResolveShaderFile(const std::filesystem::path& name)
{
    std::error_code ec;
#ifdef RENDERING_SHADER_OUTPUT_DIR
    auto built = std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR) / name;
    if (std::filesystem::exists(built, ec))
        return built;
#endif
    return std::filesystem::path(RENDERING_SOURCE_DIR) / "Shaders" / name;
}

inline std::filesystem::path TestShaderPathResolver(const std::filesystem::path& relativePath)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    auto resolved = ResolveShaderFile(relativePath);
    if (fs::exists(resolved, ec))
        return resolved;

    auto underRoot = fs::path(RENDERING_SOURCE_DIR) / relativePath;
    if (fs::exists(underRoot, ec))
        return underRoot;

    return {};
}

inline void InstallTestShaderResolver()
{
    Utils::SetShaderPathResolver(&TestShaderPathResolver);
}

namespace Detail
{
inline struct AutoInstallResolver
{
    AutoInstallResolver() { InstallTestShaderResolver(); }
} g_autoInstallResolver;
} // namespace Detail

inline bool ReadSpirvBytes(const char* name, std::vector<uint8_t>& out)
{
    out = Utils::ReadFile((ResolveShaderFile(name)).string());
    return !out.empty();
}

inline std::vector<uint8_t> ReadSpirvBytes(const char* name)
{
    return Utils::ReadFile((ResolveShaderFile(name)).string());
}

inline bool ReadTextFromShaders(const char* name, std::string& out)
{
    auto bytes = Utils::ReadFile((ResolveShaderFile(name)).string());
    if (bytes.empty())
    {
        out.clear();
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

inline bool ResolveShaderPath(const char* name, std::filesystem::path& out)
{
    out = ResolveShaderFile(name);
    std::error_code ec;
    return std::filesystem::exists(out, ec);
}

inline std::string ResolveShaderPathStr(const char* name)
{
    return (ResolveShaderFile(name)).string();
}

inline bool ReadSpirvWords(const char* name, std::vector<uint32_t>& out)
{
    std::vector<uint8_t> bytes;
    if (!ReadSpirvBytes(name, bytes))
        return false;
    if (bytes.empty() || (bytes.size() % 4) != 0)
        return false;
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

} // namespace GameEngine::Rendering::Tests
