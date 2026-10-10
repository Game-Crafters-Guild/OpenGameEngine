// Material graph variable vocabulary. MaterialGraphController itself cannot
// be reached from this target -- it owns the preview atlas, so linking it
// drags in the preview host, the canvas and the thumbnail handler.

#include <gtest/gtest.h>

#include "Graph/GraphModel.h"
#include "ShaderGraph/MaterialGraphVariables.h"

using namespace GameEngine;

TEST(MaterialGraphVariablesTests, MaterialVocabularyNamesOnlyParameterNodes)
{
    auto nodeOfType = [](const char* typeId)
    {
        Graph::Node node;
        node.TypeId = typeId;
        return node;
    };

    for (const char* typeId :
         {"FloatParameter", "Vec2Parameter", "Vec3Parameter", "Vec4Parameter", "ColorParameter"})
    {
        EXPECT_TRUE(MaterialGraphVariables::IsVariableNode(nodeOfType(typeId))) << typeId;
    }
    for (const char* typeId : {"Multiply", "Output", "FloatConstant", "Fresnel"})
    {
        EXPECT_FALSE(MaterialGraphVariables::IsVariableNode(nodeOfType(typeId))) << typeId;
    }
}

TEST(MaterialGraphVariablesTests, MaterialVariableTypesCarryTheirOwnDefaults)
{
    auto nodeOfType = [](const char* typeId)
    {
        Graph::Node node;
        node.TypeId = typeId;
        return node;
    };

    EXPECT_EQ(MaterialGraphVariables::TypeFromNode(nodeOfType("FloatParameter")), "float");
    EXPECT_EQ(MaterialGraphVariables::TypeFromNode(nodeOfType("Vec2Parameter")), "float2");
    EXPECT_EQ(MaterialGraphVariables::TypeFromNode(nodeOfType("Vec4Parameter")), "float4");
    EXPECT_EQ(MaterialGraphVariables::TypeFromNode(nodeOfType("Vec3Parameter")), "float3");
    EXPECT_EQ(MaterialGraphVariables::TypeFromNode(nodeOfType("ColorParameter")), "float3");

    EXPECT_EQ(MaterialGraphVariables::DefaultValueForType("float"), "0");
    EXPECT_EQ(MaterialGraphVariables::DefaultValueForType("float2"), "0, 0");
    EXPECT_EQ(MaterialGraphVariables::DefaultValueForType("float3"), "0, 0, 0");
    EXPECT_EQ(MaterialGraphVariables::DefaultValueForType("float4"), "0, 0, 0, 0");
}
