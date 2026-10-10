// What assigning an emissive map in the material inspector does to the rest of the
// document (Editor/Materials/EmissiveMapAssignment.h): the map must show without a
// second step, and an emitter's own color and luminance must survive.

#include "Editor/Materials/EmissiveMapAssignment.h"

#include "Rendering/Materials/MaterialDocument.h"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

using namespace GameEngine;

TEST(EmissiveMapAssignment, AFreshMaterialEmitsTheMapAtReferenceWhite)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Fresh");

    Editor::MaterialRows::TurnOnEmissionForAssignedMap(doc);

    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 203.0f);
    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("emissive")), (std::vector<float>{1.0f, 1.0f, 1.0f}));
}

TEST(EmissiveMapAssignment, ABlackColorBecomesWhite)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Black");
    doc.properties["emissive"] = std::vector<float>{0.0f, 0.0f, 0.0f};
    doc.properties["emissionLuminance"] = 400.0f;

    Editor::MaterialRows::TurnOnEmissionForAssignedMap(doc);

    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("emissive")), (std::vector<float>{1.0f, 1.0f, 1.0f}));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 400.0f);
}

TEST(EmissiveMapAssignment, AnEmitterKeepsItsColorAndLuminance)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Emitter");
    doc.properties["emissive"] = std::vector<float>{1.0f, 0.5f, 0.25f};
    doc.properties["emissionLuminance"] = 600.0f;

    Editor::MaterialRows::TurnOnEmissionForAssignedMap(doc);

    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("emissive")), (std::vector<float>{1.0f, 0.5f, 0.25f}));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties.at("emissionLuminance")), 600.0f);
}
