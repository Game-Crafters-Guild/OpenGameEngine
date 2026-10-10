// The two panels that author one thing between them — the Terrain component's Material Layers
// section and the material library asset inspector — have to use ONE name for the terrain-level
// tiling scale. They drifted once already: the terrain offered an editable "Material Tiling" while
// both panels' per-material tooltips explained "Tiling" against a "global Material Tiling" the user
// could not tell apart from it, and a third string told them tiling belonged to the material.
//
// Source-level, because the terrain inspector cannot be built in this process: it is a component
// inspector over a live World and the terrain service, and the sibling guards in this target
// (PanelDefaultTabIconTests, DebugServerReadPurityTests) scan the tree for the same reason. What is
// locked is the vocabulary, not the sentences — reword freely, but not into two names.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

// Every place `Material Tiling` appears must be an undo entry in the per-material family
// ("Change Terrain Material Albedo", "... Roughness", "... Tiling"), which names a material's own
// field. Any other occurrence is a second name for the terrain-level scale.
size_t CountLooseMaterialTiling(const std::string& source)
{
    constexpr const char* kNeedle = "Material Tiling";
    constexpr const char* kUndoPrefix = "Change Terrain ";
    size_t loose = 0;
    for (size_t at = source.find(kNeedle); at != std::string::npos;
         at = source.find(kNeedle, at + 1))
    {
        const size_t prefix = std::string(kUndoPrefix).size();
        if (at >= prefix && source.compare(at - prefix, prefix, kUndoPrefix) == 0)
            continue;
        ++loose;
    }
    return loose;
}

std::string ReadEditorSource(const std::string& relativePath)
{
    const std::filesystem::path path = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr const char* kTerrainPanel = "Source/Terrain/TerrainInspector.cpp";
constexpr const char* kLibraryPanel = "Source/Inspectors/TerrainMaterialLibraryInspector.cpp";

} // namespace

TEST(TerrainMaterialPanelVocabulary, BothPanelsCallTheTerrainLevelScaleBaseTiling)
{
    const std::string terrain = ReadEditorSource(kTerrainPanel);
    const std::string library = ReadEditorSource(kLibraryPanel);
    ASSERT_FALSE(terrain.empty()) << "the terrain panel source did not read; this test is vacuous";
    ASSERT_FALSE(library.empty()) << "the library panel source did not read; this test is vacuous";

    EXPECT_NE(terrain.find("\"Base Tiling\""), std::string::npos)
        << "the terrain panel no longer labels its tiling field Base Tiling";
    EXPECT_NE(library.find("Base Tiling"), std::string::npos)
        << "the library's per-material Tiling no longer says what it scales against";

    // The name that made the two fields indistinguishable. It may not come back as a field name in
    // either panel — the component field it named is still Terrain::MaterialTiling, which is why
    // the check is on the panels rather than on the component.
    EXPECT_EQ(CountLooseMaterialTiling(terrain), 0u)
        << "the terrain panel is back to a second name for Base Tiling";
    EXPECT_EQ(CountLooseMaterialTiling(library), 0u)
        << "the library panel is back to a second name for Base Tiling";

    // Positive control: the rule can fire at all, and it accepts the undo-entry family it must.
    EXPECT_EQ(CountLooseMaterialTiling("tooltip: the global Material Tiling"), 1u);
    EXPECT_EQ(CountLooseMaterialTiling("\"Change Terrain Material Tiling\""), 0u);
}
