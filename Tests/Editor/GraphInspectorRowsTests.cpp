#include <gtest/gtest.h>

#include "Graph/GraphInspectorRows.h"
#include "Graph/GraphValue.h"

#include <string>
#include <unordered_map>
#include <vector>

using namespace GameEngine;

namespace {

const GraphInspectorRow* FindRow(const std::vector<GraphInspectorRow>& rows, GraphInspectorRowKind kind)
{
    for (const GraphInspectorRow& row : rows)
    {
        if (row.Kind == kind)
            return &row;
    }
    return nullptr;
}

int CountKind(const std::vector<GraphInspectorRow>& rows, GraphInspectorRowKind kind)
{
    int n = 0;
    for (const GraphInspectorRow& row : rows)
    {
        if (row.Kind == kind)
            ++n;
    }
    return n;
}

bool HasKeyKind(const std::vector<GraphInspectorRow>& rows, std::string_view key, GraphInspectorRowKind kind)
{
    for (const GraphInspectorRow& row : rows)
    {
        if (row.Key == key && row.Kind == kind)
            return true;
    }
    return false;
}

} // namespace

TEST(GraphInspectorRowsTests, ColorConstantRgbBecomesOneSwatch)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"r", "0.5"}, {"g", "0.25"}, {"b", "0.125"}};
    NodeTypeMeta schema;
    schema.TypeId = "ColorConstant";
    schema.Parameters = {{"r", 1.0f}, {"g", 1.0f}, {"b", 1.0f}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Kind, GraphInspectorRowKind::ColorSwatch);
    EXPECT_EQ(rows[0].Label, "Color");
}

TEST(GraphInspectorRowsTests, UnknownKindRgbStillGetsASwatch)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"r", "1"}, {"g", "0"}, {"b", "0"}};
    const auto rows = PlanGraphInspectorRows(parameters, nullptr);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Kind, GraphInspectorRowKind::ColorSwatch);
}

TEST(GraphInspectorRowsTests, FloatParameterLinkedUsesVariableWidgets)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"slot", "0"}, {"component", "x"}, {"variableName", "uRoughness"}, {"value", "0.4"}};
    NodeTypeMeta schema;
    schema.TypeId = "FloatParameter";
    schema.Parameters = {{"slot", 0}, {"component", "x"}, {"variableName", "param"}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableName), 1);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableFloat), 1);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::NodeFloat), 0);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::NodeInt), 0);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::NodeText), 0);
}

TEST(GraphInspectorRowsTests, FloatParameterUnlinkedHidesInternalsAndValue)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"slot", "0"}, {"component", "x"}, {"variableName", ""}, {"value", "0.4"}};
    NodeTypeMeta schema;
    schema.TypeId = "FloatParameter";
    schema.Parameters = {{"slot", 0}, {"component", "x"}, {"variableName", "param"}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_TRUE(rows.empty());
}

TEST(GraphInspectorRowsTests, ColorParameterLinkedUsesVariableColor)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"slot", "0"},
        {"swizzle", "xyz"},
        {"variableName", "uTint"},
        {"r", "1"},
        {"g", "0"},
        {"b", "0.5"}};
    NodeTypeMeta schema;
    schema.TypeId = "ColorParameter";
    schema.Parameters = {{"slot", 0}, {"swizzle", "xyz"}, {"variableName", "param"}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_NE(FindRow(rows, GraphInspectorRowKind::VariableName), nullptr);
    EXPECT_NE(FindRow(rows, GraphInspectorRowKind::VariableColor), nullptr);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::ColorSwatch), 0);
    EXPECT_FALSE(HasKeyKind(rows, "r", GraphInspectorRowKind::NodeFloat));
}

TEST(GraphInspectorRowsTests, Vec3ParameterLinkedEmitsThreeAxes)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"slot", "0"},
        {"swizzle", "xyz"},
        {"variableName", "uDir"},
        {"x", "1"},
        {"y", "0"},
        {"z", "0"}};
    NodeTypeMeta schema;
    schema.TypeId = "Vec3Parameter";
    schema.Parameters = {{"slot", 0}, {"swizzle", "xyz"}, {"variableName", "param"}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableVec), 3);
    EXPECT_EQ(rows.back().VecCount, 3);
    EXPECT_EQ(rows.back().Label, "Z");
}

TEST(GraphInspectorRowsTests, SchemaFloatWinsOverIntShapedText)
{
    const std::unordered_map<std::string, std::string> parameters = {{"scale", "1"}};
    NodeTypeMeta schema;
    schema.TypeId = "Time";
    schema.Parameters = {{"scale", 1.0f}, {"offset", 0.0f}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Kind, GraphInspectorRowKind::NodeFloat);
    EXPECT_EQ(rows[0].Key, "scale");
}

TEST(GraphInspectorRowsTests, SchemaOrdersRemainingKeys)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"function", "greater"}, {"power", "5"}};
    NodeTypeMeta schema;
    schema.TypeId = "Probe";
    schema.Parameters = {{"power", 5.0f}, {"function", "greater"}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].Key, "power");
    EXPECT_EQ(rows[0].Kind, GraphInspectorRowKind::NodeFloat);
    EXPECT_EQ(rows[1].Key, "function");
    EXPECT_EQ(rows[1].Kind, GraphInspectorRowKind::NodeText);
}

TEST(GraphInspectorRowsTests, SchemaOptionsPlanAnEnumRow)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"comparison", "greater"}};
    NodeTypeMeta schema;
    schema.TypeId = "CompareFloat";
    schema.Parameters = {{"comparison", std::string("greater"),
                          {{"greater", "Greater"}, {"less", "Less"}}}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Kind, GraphInspectorRowKind::NodeEnum);
    EXPECT_EQ(rows[0].Key, "comparison");
    // The renderer reaches the choices back through the schema.
    const NodeParamSpec* spec = FindSchemaParam(&schema, "comparison");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->Options[1].Label, "Less");
}

TEST(GraphInspectorRowsTests, UnschemedIntAndTextUseParse)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"count", "3"}, {"title", "Hello"}};
    const auto rows = PlanGraphInspectorRows(parameters, nullptr);
    EXPECT_TRUE(HasKeyKind(rows, "count", GraphInspectorRowKind::NodeInt));
    EXPECT_TRUE(HasKeyKind(rows, "title", GraphInspectorRowKind::NodeText));
}

TEST(GraphInspectorRowsTests, FindUniqueResolvesMaterialColorConstant)
{
    const NodeTypeMeta* meta = FindUniqueNodeTypeMeta("ColorConstant");
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->TypeId, "ColorConstant");
    bool hasR = false;
    for (const NodeParamSpec& param : meta->Parameters)
    {
        if (param.Id == "r")
            hasR = true;
    }
    EXPECT_TRUE(hasR);
}

TEST(GraphInspectorRowsTests, FindUniqueMissesUnknownType)
{
    EXPECT_EQ(FindUniqueNodeTypeMeta("NotARegisteredGraphNode"), nullptr);
}

TEST(GraphInspectorRowsTests, RegistryColorConstantPlanMatchesSwatch)
{
    const NodeTypeMeta* meta = FindUniqueNodeTypeMeta("ColorConstant");
    ASSERT_NE(meta, nullptr);
    const std::unordered_map<std::string, std::string> parameters = {
        {"r", "1"}, {"g", "1"}, {"b", "1"}};
    const auto rows = PlanGraphInspectorRows(parameters, meta);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Kind, GraphInspectorRowKind::ColorSwatch);
}

TEST(GraphInspectorRowsTests, GetVariableIsNotAMaterialParameter)
{
    const NodeTypeMeta* meta = FindUniqueNodeTypeMeta("GetVariable");
    ASSERT_NE(meta, nullptr);
    const std::unordered_map<std::string, std::string> parameters = {{"variableName", "playerScore"}};
    const auto rows = PlanGraphInspectorRows(parameters, meta);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableName), 0);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableFloat), 0);
    EXPECT_TRUE(HasKeyKind(rows, "variableName", GraphInspectorRowKind::NodeText));
}

TEST(GraphInspectorRowsTests, SetVariableValueIsANodeParameter)
{
    const NodeTypeMeta* meta = FindUniqueNodeTypeMeta("SetVariable");
    ASSERT_NE(meta, nullptr);
    const std::unordered_map<std::string, std::string> parameters = {
        {"variableName", "playerScore"}, {"value", "0"}};
    const auto rows = PlanGraphInspectorRows(parameters, meta);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableName), 0);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableFloat), 0);
    EXPECT_TRUE(HasKeyKind(rows, "variableName", GraphInspectorRowKind::NodeText));
    EXPECT_TRUE(HasKeyKind(rows, "value", GraphInspectorRowKind::NodeInt));
}

TEST(GraphInspectorRowsTests, GetVariableLiveSlotDoesNotBecomeMaterial)
{
    const NodeTypeMeta* meta = FindUniqueNodeTypeMeta("GetVariable");
    ASSERT_NE(meta, nullptr);
    const std::unordered_map<std::string, std::string> parameters = {
        {"variableName", "playerScore"}, {"slot", "0"}};
    const auto rows = PlanGraphInspectorRows(parameters, meta);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableName), 0);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableFloat), 0);
    EXPECT_TRUE(HasKeyKind(rows, "variableName", GraphInspectorRowKind::NodeText));
    EXPECT_TRUE(HasKeyKind(rows, "slot", GraphInspectorRowKind::NodeInt));
}

TEST(GraphInspectorRowsTests, DelaySecondsUsesNumericWidget)
{
    const NodeTypeMeta* meta = FindUniqueNodeTypeMeta("Delay");
    ASSERT_NE(meta, nullptr);
    const std::unordered_map<std::string, std::string> parameters = {{"seconds", "1"}};
    const auto rows = PlanGraphInspectorRows(parameters, meta);
    EXPECT_TRUE(HasKeyKind(rows, "seconds", GraphInspectorRowKind::NodeInt) ||
                HasKeyKind(rows, "seconds", GraphInspectorRowKind::NodeFloat));
}

TEST(GraphInspectorRowsTests, FloatParameterLinkedWithoutValueStillShowsValue)
{
    const std::unordered_map<std::string, std::string> parameters = {
        {"slot", "0"}, {"component", "x"}, {"variableName", "uRoughness"}};
    NodeTypeMeta schema;
    schema.TypeId = "FloatParameter";
    schema.Parameters = {{"slot", 0}, {"component", "x"}, {"variableName", "param"}};

    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableName), 1);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::VariableFloat), 1);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::NodeFloat), 0);
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::NodeInt), 0);
}

TEST(GraphInspectorRowsTests, ClipGuidSchemaPlansAssetRow)
{
    NodeTypeMeta schema;
    schema.TypeId = "ClipPlayer";
    schema.Parameters = {
        {"clipGuid", Graph::GraphValue::FromGuid("")},
        {"speed", 1.0f},
        {"looping", true},
    };
    const std::unordered_map<std::string, std::string> parameters = {
        {"clipGuid", ""}, {"speed", "1"}, {"looping", "true"}};
    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_TRUE(HasKeyKind(rows, "clipGuid", GraphInspectorRowKind::NodeAsset));
    const GraphInspectorRow* clip = nullptr;
    for (const GraphInspectorRow& row : rows)
    {
        if (row.Key == "clipGuid")
            clip = &row;
    }
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->Label, "Clip");
    EXPECT_TRUE(HasKeyKind(rows, "speed", GraphInspectorRowKind::NodeFloat));
}

TEST(GraphInspectorRowsTests, OtherGuidStaysText)
{
    NodeTypeMeta schema;
    schema.Parameters = {{"otherGuid", Graph::GraphValue::FromGuid("")}};
    const std::unordered_map<std::string, std::string> parameters = {{"otherGuid", ""}};
    const auto rows = PlanGraphInspectorRows(parameters, &schema);
    EXPECT_TRUE(HasKeyKind(rows, "otherGuid", GraphInspectorRowKind::NodeText));
    EXPECT_EQ(CountKind(rows, GraphInspectorRowKind::NodeAsset), 0);
}

TEST(GraphInspectorRowsTests, RegistryClipPlayerPlansClipAssetRow)
{
    (void)GraphNodeRegistry::Get();
    const NodeTypeMeta* meta = FindNodeTypeMeta("animation", "ClipPlayer");
    ASSERT_NE(meta, nullptr);
    const auto parameters = GraphNodeRegistry::Get().GetDefaultParameters("animation", "ClipPlayer");
    const auto rows = PlanGraphInspectorRows(parameters, meta);
    EXPECT_TRUE(HasKeyKind(rows, "clipGuid", GraphInspectorRowKind::NodeAsset));
    const GraphInspectorRow* clip = nullptr;
    for (const GraphInspectorRow& row : rows)
    {
        if (row.Key == "clipGuid")
            clip = &row;
    }
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->Label, "Clip");
}

TEST(GraphInspectorRowsTests, FindUniqueReturnsNullOnKindCollision)
{
    NodeTypeMeta meta;
    meta.TypeId = "AdvInspectorCollisionUnique";
    GraphNodeRegistry::Get().Register("material", meta);
    GraphNodeRegistry::Get().Register("animation", meta);
    EXPECT_EQ(FindUniqueNodeTypeMeta("AdvInspectorCollisionUnique"), nullptr);
}

TEST(GraphInspectorRowsTests, FindNodeTypeMetaUsesKindIdOnCollision)
{
    NodeTypeMeta meta;
    meta.TypeId = "AdvInspectorKindIdLookup";
    meta.DisplayName = "Material Copy";
    GraphNodeRegistry::Get().Register("material", meta);
    meta.DisplayName = "Animation Copy";
    GraphNodeRegistry::Get().Register("animation", meta);
    EXPECT_EQ(FindUniqueNodeTypeMeta("AdvInspectorKindIdLookup"), nullptr);
    const NodeTypeMeta* material = FindNodeTypeMeta("material", "AdvInspectorKindIdLookup");
    const NodeTypeMeta* animation = FindNodeTypeMeta("animation", "AdvInspectorKindIdLookup");
    ASSERT_NE(material, nullptr);
    ASSERT_NE(animation, nullptr);
    EXPECT_EQ(material->DisplayName, "Material Copy");
    EXPECT_EQ(animation->DisplayName, "Animation Copy");
    EXPECT_EQ(FindNodeTypeMeta("", "AdvInspectorKindIdLookup"), nullptr);
}
