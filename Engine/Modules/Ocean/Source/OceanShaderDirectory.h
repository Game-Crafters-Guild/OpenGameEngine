#pragma once

#include <filesystem>

namespace GameEngine::Ocean
{

/**
 * @brief The staged Ocean shader directory, `<install assets root>/Shaders/Ocean`.
 *
 * Every Ocean GPU stage compiles its GLSL from the copy the build stages with the executable
 * (PathUtils::GetInstallAssetsRoot()); the parent of the returned directory is the include root
 * for "Ocean/..." includes.
 *
 * @return The directory when @p requiredShaderFile is staged in it; empty otherwise, so the
 *         caller disables its stage with a message naming the missing file
 */
std::filesystem::path OceanShaderDirectory(const char* requiredShaderFile);

} // namespace GameEngine::Ocean
