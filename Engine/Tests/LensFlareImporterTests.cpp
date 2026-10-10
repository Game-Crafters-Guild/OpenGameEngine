// Round-trip tests for the lens-flare importer: synthetic ProFlares-style
// documents (constructed here — no vendored content) go through
// ImportAtlas/ImportFlare/ImportFolder and the resulting engine JSON is
// asserted field by field. These lock in the data-format semantics that were
// reverse-engineered the hard way:
//   * the authored offset is the TYPO field "OffsetPostion" (the correctly
//     spelled one is runtime-baked and must be ignored),
//   * "position" maps to engine position + 1,
//   * Multi elements expand one engine element per baked sub-element,
//   * the numeric Dynamic*Boost fields are SIZE boosts,
//   * per-element Override* flags gate optional per-element coefficients,
//   * legacy elements resolve sprites via elementTextureID when SpriteName
//     is missing.
#include "Assets/LensFlareImporter.h"
#include "Assets/LensFlareDefinitionAsset.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
namespace fs = std::filesystem;
using nlohmann::json;
using namespace GameEngine;

class LensFlareImporterTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = fs::temp_directory_path() / "lensflare_importer_tests";
        fs::remove_all(m_Dir);
        fs::create_directories(m_Dir);
    }
    void TearDown() override { fs::remove_all(m_Dir); }

    fs::path Write(const char* name, const std::string& text)
    {
        const fs::path p = m_Dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }

    static json Load(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return json::parse(in);
    }

    fs::path m_Dir;
};

// A minimal TexturePacker atlas doc: two frames on a 512x256 sheet.
const char* kAtlasDoc = R"({"frames": {
"Glow_Big.psd": {"frame": {"x":0,"y":0,"w":256,"h":256}},
"Streak_Thin.psd": {"frame": {"x":256,"y":128,"w":256,"h":64}}
},
"meta": {"image":"TestAtlas.png","size": {"w":512,"h":256}}})";

// A flare doc exercising the semantics above. Deliberately near-JSON: the
// comma between "type" and "size" is omitted, as the authoring tool writes it.
const char* kFlareDoc = R"({
"meta": {
	"GlobalScale": 100,
	"GlobalBrightness": 0.5,
	"useDynamicEdgeBoost": 1,
	"DynamicEdgeBoost": 2.0,
	"DynamicEdgeBrightness": 0.25,
	"DynamicEdgeRange": 0.3,
	"Elements": {
		"Element0": {
			"Visible": 1,
			"elementTextureID": 1,
			"Brightness": 1,
			"Scale": 0.5,
			"position": -1,
			"OffsetPosition": {"r": 0.9, "g": 0.1, "b": 0},
			"OffsetPostion": {"r": 0.05, "g": -0.05, "b": 0},
			"Anamorphic": {"r": 0.5, "g": 1, "b": 0},
			"angle": 10,
			"OverrideDynamicEdgeBrightness": 1,
			"DynamicEdgeBrightnessOverride": 0.75,
			"ElementTint": {"r": 1, "g": 0.5, "b": 0.25, "a": 0.8},
			"type": "0"
			"size": {"x": 4, "y": 0.25}
		},
		"Element1": {
			"Visible": 1,
			"SpriteName": "Glow_Big",
			"Scale": 1,
			"position": 0.5,
			"useRangeOffset": 1,
			"type": "1",
			"subElements": {
				"subElement0": {"position": -0.25, "angle": 45, "scale": 2, "color": {"r":1,"g":1,"b":1,"a":0.5}},
				"subElement1": {"position": 0.75, "angle": 90, "scale": 0.5, "color": {"r":0,"g":1,"b":0,"a":1}}
			}
		}
	}
}
})";

TEST_F(LensFlareImporterTest, AtlasNormalizesUvsAndStripsExtensions)
{
    const fs::path src = Write("TestAtlas.txt", kAtlasDoc);
    const fs::path out = m_Dir / "TestAtlas.flareatlas";
    std::string err;
    ASSERT_TRUE(LensFlareImport::ImportAtlas(src, "Flares/TestAtlas.png", out, &err)) << err;

    const json atlas = Load(out);
    EXPECT_EQ(atlas["texture"], "Flares/TestAtlas.png");
    ASSERT_EQ(atlas["sprites"].size(), 2u);
    const json& glow = atlas["sprites"][0];
    EXPECT_EQ(glow["name"], "Glow_Big"); // ".psd" stripped
    EXPECT_FLOAT_EQ(glow["u"].get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(glow["w"].get<float>(), 256.0f / 512.0f);
    EXPECT_FLOAT_EQ(glow["h"].get<float>(), 256.0f / 256.0f);
    const json& streak = atlas["sprites"][1];
    EXPECT_FLOAT_EQ(streak["u"].get<float>(), 0.5f);
    EXPECT_FLOAT_EQ(streak["v"].get<float>(), 0.5f);
}

TEST_F(LensFlareImporterTest, FlareFieldSemantics)
{
    const fs::path src = Write("TestFlare.txt", kFlareDoc);
    const fs::path out = m_Dir / "TestFlare.lensflare";
    std::string err;
    const std::vector<std::string> atlasSprites{"Glow_Big", "Streak_Thin"};
    ASSERT_TRUE(
        LensFlareImport::ImportFlare(src, "Flares/TestAtlas.flareatlas", out, &err, &atlasSprites))
        << err;

    const json flare = Load(out);
    EXPECT_EQ(flare["atlas"], "Flares/TestAtlas.flareatlas");

    const json& g = flare["globals"];
    EXPECT_FLOAT_EQ(g["globalScale"].get<float>(), 100.0f);
    // The numeric DynamicEdgeBoost is a SIZE boost mapped onto the edge-scale ramp.
    EXPECT_TRUE(g["useDynamicEdgeScale"].get<bool>());
    EXPECT_FLOAT_EQ(g["dynamicEdgeScale"].get<float>(), 2.0f);
    EXPECT_FLOAT_EQ(g["dynamicEdgeBrightness"].get<float>(), 0.25f);

    // Element0 (Single) + Element1 expanded into its 2 sub-elements = 3 total.
    ASSERT_EQ(flare["elements"].size(), 3u);

    const json& e0 = flare["elements"][0];
    // Legacy sprite binding: no SpriteName -> elementTextureID indexes the atlas order.
    EXPECT_EQ(e0["sprite"], "Streak_Thin");
    // ProFlares position -1 (at source) -> engine 0.
    EXPECT_FLOAT_EQ(e0["position"].get<float>(), 0.0f);
    // Authored TYPO offset wins; the runtime-baked "OffsetPosition" is ignored.
    EXPECT_FLOAT_EQ(e0["offsetX"].get<float>(), 0.05f);
    EXPECT_FLOAT_EQ(e0["offsetY"].get<float>(), -0.05f);
    EXPECT_FLOAT_EQ(e0["anamorphicX"].get<float>(), 0.5f);
    EXPECT_FLOAT_EQ(e0["sizeX"].get<float>(), 4.0f);
    EXPECT_FLOAT_EQ(e0["sizeY"].get<float>(), 0.25f);
    // Override flag set -> per-element coefficient emitted; others absent.
    EXPECT_FLOAT_EQ(e0["edgeBrightnessBoost"].get<float>(), 0.75f);
    EXPECT_FALSE(e0.contains("centerBrightnessBoost"));
    EXPECT_FALSE(e0.contains("edgeScaleBoost"));

    // Multi expansion: per-sub position (+1), angle, scale multiplier, color.
    const json& s0 = flare["elements"][1];
    EXPECT_EQ(s0["sprite"], "Glow_Big");
    EXPECT_FLOAT_EQ(s0["position"].get<float>(), 0.75f); // -0.25 + 1
    EXPECT_FLOAT_EQ(s0["angle"].get<float>(), 45.0f);
    EXPECT_FLOAT_EQ(s0["scale"].get<float>(), 2.0f); // element Scale 1 x sub 2
    EXPECT_FLOAT_EQ(s0["tint"]["a"].get<float>(), 0.5f);
    const json& s1 = flare["elements"][2];
    EXPECT_FLOAT_EQ(s1["position"].get<float>(), 1.75f);
    EXPECT_FLOAT_EQ(s1["scale"].get<float>(), 0.5f);
    EXPECT_FLOAT_EQ(s1["tint"]["g"].get<float>(), 1.0f);
}

TEST_F(LensFlareImporterTest, ImportFolderBindsFlaresToCoveringAtlas)
{
    Write("TestAtlas.txt", kAtlasDoc);
    Write("TestFlare.txt", kFlareDoc);
    Write("ReadMe.txt", "Just a readme\n— with an em-dash, not JSON.\n");
    // The atlas texture the doc pairs with (content irrelevant to the importer).
    Write("TestAtlas.png", "png-bytes");

    const fs::path outDir = m_Dir / "out";
    std::vector<LensFlareImport::FolderImportEntry> report;
    const int written = LensFlareImport::ImportFolder(m_Dir, outDir, "Flares", report);

    EXPECT_EQ(written, 2); // one atlas + one flare
    EXPECT_TRUE(fs::exists(outDir / "TestAtlas.flareatlas"));
    EXPECT_TRUE(fs::exists(outDir / "TestAtlas.png"));
    EXPECT_TRUE(fs::exists(outDir / "TestFlare.lensflare"));

    const json flare = Load(outDir / "TestFlare.lensflare");
    EXPECT_EQ(flare["atlas"], "Flares/TestAtlas.flareatlas");

    // The readme classifies as skipped, not as an error or a crash.
    bool readmeSkipped = false;
    for (const auto& e : report)
        if (e.Source.filename() == "ReadMe.txt")
            readmeSkipped = !e.Ok && e.Note.find("skipped") != std::string::npos;
    EXPECT_TRUE(readmeSkipped);
}

TEST_F(LensFlareImporterTest, DefinitionSaveRoundTripsEditableSettings)
{
    const fs::path path = Write("Editable.lensflare", R"({"atlas":"Original.flareatlas"})");
    LensFlareDefinitionAsset authored(GUID::Generate(), path);
    ASSERT_TRUE(authored.Load());

    authored.SetAtlasRef("Flares/Edited.flareatlas");
    auto& globals = authored.EditGlobals();
    globals.GlobalScale = 2.5f;
    globals.GlobalBrightness = 0.75f;
    globals.GlobalTint = {0.8f, 0.6f, 0.4f, 0.9f};
    globals.UseAngleLimit = true;
    globals.UseAngleCurve = true;
    globals.AngleCurveKeys = {{0.0f, 0.0f, 0.1f, 0.2f}, {1.0f, 1.0f, 0.3f, 0.4f}};
    globals.UseDistanceFade = true;
    globals.UseMaxDistance = true;
    globals.MaxDistance = 275.0f;
    globals.UseDynamicEdgeBoost = true;
    globals.DynamicEdgeBrightness = 0.35f;
    globals.DynamicEdgeCurveKeys = {{0.0f, 0.0f, 0.0f, 1.0f},
                                    {0.5f, 1.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, -1.0f, 0.0f}};
    globals.UseDynamicCenterBoost = true;
    globals.DynamicCenterScale = 0.45f;
    globals.MultiplyScaleByTransformScale = true;
    globals.NeverCull = true;

    LensFlare::FlareElement element;
    element.SpriteName = "Glow_Large";
    element.Brightness = 1.4f;
    element.Scale = 0.8f;
    element.SizeX = 3.0f;
    element.SizeY = 0.25f;
    element.Position = 1.25f;
    element.OffsetX = -0.2f;
    element.AnamorphicY = 0.6f;
    element.Angle = 42.0f;
    element.RotateToFlare = true;
    element.RotationSpeed = 8.0f;
    element.EdgeBrightnessBoost = 0.65f;
    element.Tint = {0.25f, 0.5f, 1.0f, 0.7f};
    authored.EditElements().push_back(element);

    ASSERT_TRUE(authored.Save());

    LensFlareDefinitionAsset reloaded(GUID::Generate(), path);
    ASSERT_TRUE(reloaded.Load());
    EXPECT_EQ(reloaded.GetAtlasRef(), "Flares/Edited.flareatlas");
    const auto& savedGlobals = reloaded.GetGlobals();
    EXPECT_FLOAT_EQ(savedGlobals.GlobalScale, 2.5f);
    EXPECT_FLOAT_EQ(savedGlobals.GlobalTint.A, 0.9f);
    EXPECT_TRUE(savedGlobals.UseAngleCurve);
    ASSERT_EQ(savedGlobals.AngleCurveKeys.size(), 2u);
    EXPECT_FLOAT_EQ(savedGlobals.AngleCurveKeys[1].OutTangent, 0.4f);
    EXPECT_FLOAT_EQ(savedGlobals.MaxDistance, 275.0f);
    ASSERT_EQ(savedGlobals.DynamicEdgeCurveKeys.size(), 3u);
    EXPECT_FLOAT_EQ(savedGlobals.DynamicCenterScale, 0.45f);
    EXPECT_TRUE(savedGlobals.MultiplyScaleByTransformScale);
    EXPECT_TRUE(savedGlobals.NeverCull);

    ASSERT_EQ(reloaded.GetElements().size(), 1u);
    const auto& savedElement = reloaded.GetElements().front();
    EXPECT_EQ(savedElement.SpriteName, "Glow_Large");
    EXPECT_FLOAT_EQ(savedElement.SizeX, 3.0f);
    EXPECT_FLOAT_EQ(savedElement.Position, 1.25f);
    EXPECT_TRUE(savedElement.RotateToFlare);
    EXPECT_FLOAT_EQ(savedElement.EdgeBrightnessBoost, 0.65f);
    EXPECT_EQ(savedElement.CenterBrightnessBoost, LensFlare::kInheritGlobalBoost);
    EXPECT_FLOAT_EQ(savedElement.Tint.B, 1.0f);

    const json serialized = Load(path);
    EXPECT_FALSE(serialized["elements"][0].contains("centerBrightnessBoost"));
}

} // namespace
