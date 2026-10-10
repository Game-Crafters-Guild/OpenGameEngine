// The material inspector's undo snapshot (Editor/Materials/MaterialUndoSnapshot.h): undo writes the
// file back exactly as the author had it, so a key the editor fills in at load is never written.

#include "Editor/Materials/MaterialUndoSnapshot.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(MaterialUndoSnapshot, IsTheFileAsTheAuthorWroteIt)
{
    const fs::path dir = fs::temp_directory_path() / "ge_material_undo_snapshot";
    fs::create_directories(dir);
    const fs::path file = dir / "Unauthored.material";
    const std::string authored = R"({"schemaVersion": 3, "materialName": "Unauthored", "lightingModel": "StandardPBR",
  "surfaceShader": "Surfaces/standard_pbr.glsl", "properties": {"emissionLuminance": 203}})";
    std::ofstream(file, std::ios::binary) << authored;

    std::vector<std::uint8_t> snapshot;
    ASSERT_TRUE(GameEngine::Editor::MaterialRows::ReadMaterialFileSnapshot(file, snapshot));
    EXPECT_EQ(std::string(snapshot.begin(), snapshot.end()), authored)
        << "undo must restore the file byte for byte, without the keys filled in at load";
    EXPECT_FALSE(GameEngine::Editor::MaterialRows::ReadMaterialFileSnapshot(dir / "Missing.material", snapshot));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
