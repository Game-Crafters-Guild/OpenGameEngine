// Installs a shader-path resolver for Engine test harnesses that exercise the
// GPU-driven path (GPUScene / GPUDrawStreamBuilder / material + primitive
// registration). Without it, ResolveShaderPath has no resolver and the
// GPU-driven setup faults (SEH 0xc0000005). Mirrors UITestShaderSetup.cpp:
// resolves relative shader package paths against the build directory, where
// CompileShaderPkgs stages the .shaderpkg / .spv outputs.
#include "EngineTestShaderSetup.h"

#include "Rendering/Common/Utils.h"

#include <filesystem>

#ifndef ENGINE_TEST_BUILD_DIR
#error "ENGINE_TEST_BUILD_DIR must be defined by CMake (target_compile_definitions)"
#endif

using namespace GameEngine::Rendering;

std::filesystem::path GameEngine::Testing::EngineTestShaderPathResolver(
    const std::filesystem::path& relativePath)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    auto path = fs::path(ENGINE_TEST_BUILD_DIR) / relativePath;
    if (fs::exists(path, ec))
        return path;

    return {};
}

namespace
{
struct AutoInstallResolver
{
    AutoInstallResolver()
    {
        Utils::SetShaderPathResolver(&GameEngine::Testing::EngineTestShaderPathResolver);
    }
} g_autoInstallResolver;
} // namespace
