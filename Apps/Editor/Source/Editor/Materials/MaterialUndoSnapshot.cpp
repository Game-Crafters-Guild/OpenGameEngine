#include "Editor/Materials/MaterialUndoSnapshot.h"

#include <fstream>
#include <iterator>

namespace GameEngine::Editor::MaterialRows
{

bool ReadMaterialFileSnapshot(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    outBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

} // namespace GameEngine::Editor::MaterialRows
