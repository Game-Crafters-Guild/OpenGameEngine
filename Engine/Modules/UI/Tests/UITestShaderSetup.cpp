#include "Rendering/Common/Utils.h"

#include <filesystem>

#ifndef UI_TEST_BUILD_DIR
#error "UI_TEST_BUILD_DIR must be defined by CMake (target_compile_definitions)"
#endif

using namespace GameEngine::Rendering;

static std::filesystem::path UITestShaderPathResolver(const std::filesystem::path& relativePath)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    auto path = fs::path(UI_TEST_BUILD_DIR) / relativePath;
    if (fs::exists(path, ec))
        return path;

    return {};
}

namespace
{
struct AutoInstallResolver
{
    AutoInstallResolver() { Utils::SetShaderPathResolver(&UITestShaderPathResolver); }
} g_autoInstallResolver;
} // namespace
