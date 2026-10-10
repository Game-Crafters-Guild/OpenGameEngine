#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace GameEngine::Editor::MaterialRows
{

/// The undo snapshot of a material edit: the material file's bytes as they are on disk. Undo
/// writes them back unchanged, so it restores exactly what the author had, keys the editor fills
/// in at load (the StandardPBR defaults) left out and a MaterialX source left in its own format.
/// False when the file cannot be read.
bool ReadMaterialFileSnapshot(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes);

} // namespace GameEngine::Editor::MaterialRows
