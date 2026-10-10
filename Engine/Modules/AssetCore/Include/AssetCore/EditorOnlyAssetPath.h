#pragma once

#include <filesystem>

namespace GameEngine
{

/**
 * @brief The Editor folder convention for asset roots.
 *
 * A directory named "Editor" (case-insensitive, at any depth under an asset
 * root) holds content that exists only while the editor runs: it is never
 * staged into a build, and source files under it compile into a package's
 * editor module rather than its runtime one.
 *
 * @param relativePath Path RELATIVE to the asset root. An absolute path whose
 *        ancestry happens to contain an Editor directory ABOVE the root would
 *        match wrongly.
 * @return True when any segment of the path is named "Editor".
 */
bool IsEditorOnlyAssetPath(const std::filesystem::path& relativePath);

} // namespace GameEngine
