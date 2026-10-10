#include <gtest/gtest.h>

#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/GraphValue.h"

using namespace GameEngine;

TEST(GraphNodeRegistryTests, FloatConstantDefaultsComeFromParameterSchema)
{
    const auto defaults = GraphNodeRegistry::Get().GetDefaultParameters(
        Graph::kKindIdMaterial, "FloatConstant");
    ASSERT_EQ(defaults.count("value"), 1u);
    EXPECT_EQ(defaults.at("value"), "0");
}

TEST(GraphNodeRegistryTests, TimeDefaultsComeFromParameterSchema)
{
    const auto defaults = GraphNodeRegistry::Get().GetDefaultParameters(
        Graph::kKindIdMaterial, "Time");
    EXPECT_EQ(defaults.at("scale"), "1");
    EXPECT_EQ(defaults.at("offset"), "0");
}

TEST(GraphNodeRegistryTests, SetVariableDefaultsComeFromCatalogSchema)
{
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find(Graph::kKindIdGameLogic, "SetVariable");
    ASSERT_NE(meta, nullptr);
    ASSERT_FALSE(meta->Parameters.empty());
    const auto defaults = GraphNodeRegistry::Get().GetDefaultParameters(
        Graph::kKindIdGameLogic, "SetVariable");
    EXPECT_EQ(defaults.at("variableName"), "playerScore");
}

TEST(GraphNodeRegistryTests, CompareFloatComparisonIsTypedWithRuntimeValues)
{
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find(Graph::kKindIdGameLogic, "CompareFloat");
    ASSERT_NE(meta, nullptr);
    const NodeParamSpec* spec = nullptr;
    for (const NodeParamSpec& param : meta->Parameters)
    {
        if (param.Id == "comparison")
            spec = &param;
    }
    ASSERT_NE(spec, nullptr);
    ASSERT_EQ(spec->Options.size(), 6u);
    // Values are the wire strings GameLogicRuntimeContext::CompareFloat matches.
    EXPECT_EQ(spec->Options[0].Value, "greater");
    EXPECT_EQ(spec->Options[0].Label, "Greater");
    EXPECT_EQ(spec->Options[1].Value, "greater_or_equal");
    EXPECT_EQ(spec->Options[5].Value, "not_equal");
    EXPECT_EQ(spec->Options[5].Label, "Not Equal");
}

TEST(GraphNodeRegistryTests, MaterialCompareFunctionIsTypedWithShaderValues)
{
    /* Compare reaches the material palette through shader-graph reflection,
       which needs the staged nodes root. Register the bare type when the root
       was unavailable: the enum descriptors under test are attached by
       Register (AttachKnownMaterialParameters), not by reflection. */
    if (!GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "Compare"))
        GraphNodeRegistry::Get().Register(Graph::kKindIdMaterial, {"Compare", "Compare", "Utility"});
    const NodeTypeMeta* meta =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "Compare");
    ASSERT_NE(meta, nullptr);
    const NodeParamSpec* spec = nullptr;
    for (const NodeParamSpec& param : meta->Parameters)
    {
        if (param.Id == "function")
            spec = &param;
    }
    ASSERT_NE(spec, nullptr);
    ASSERT_EQ(spec->Options.size(), 6u);
    /* Values are the GLSL selector constants SG_Compare branches on: the graph
       compiler splices a parameter value verbatim into the emitted call. */
    EXPECT_EQ(spec->Options[0].Value, "SG_COMPARE_GREATER");
    EXPECT_EQ(spec->Options[3].Value, "SG_COMPARE_NOT_EQUAL");
    EXPECT_EQ(spec->Options[3].Label, "Not Equal");
    EXPECT_EQ(spec->Default.ToString(), "SG_COMPARE_GREATER");
}

TEST(GraphNodeRegistryTests, CreateNodeAppliesSchemaDefaults)
{
    const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
        Graph::kKindIdMaterial, "ColorConstant", "n0", 0.f, 0.f);
    auto it = node.Parameters.find("r");
    ASSERT_NE(it, node.Parameters.end());
    EXPECT_TRUE(it->second.IsFloat());
    EXPECT_DOUBLE_EQ(node.Parameters.GetFloat("r"), 1.0);
    EXPECT_DOUBLE_EQ(node.Parameters.GetFloat("g"), 1.0);
    EXPECT_DOUBLE_EQ(node.Parameters.GetFloat("b"), 1.0);
}

TEST(GraphNodeRegistryTests, FloatConstantCreateNodeStoresAFloatValue)
{
    const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
        Graph::kKindIdMaterial, "FloatConstant", "n1", 0.f, 0.f);
    auto it = node.Parameters.find("value");
    ASSERT_NE(it, node.Parameters.end());
    EXPECT_TRUE(it->second.IsFloat());
    EXPECT_DOUBLE_EQ(node.Parameters.GetFloat("value"), 0.0);
}

TEST(GraphNodeRegistryTests, AnimationKindIsRegisteredWithoutAnEnumerator)
{
    (void)GraphNodeRegistry::Get();
    Graph::Kind parsed = Graph::Kind::GameLogic;
    EXPECT_FALSE(Graph::GraphTypeRegistry::TryParseKind("animation", parsed));
    const Graph::GraphTypeDesc* desc = Graph::GraphTypeRegistry::Get().Find("animation");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->DisplayName, "Animation");
    EXPECT_EQ(desc->FileExtension, ".animgraph");
}

TEST(GraphNodeRegistryTests, GetAllTypesByKindIdIncludesAnimationClipPlayer)
{
    const auto animation = GraphNodeRegistry::Get().GetAllTypes("animation");
    bool found = false;
    for (const NodeTypeMeta& meta : animation)
    {
        if (meta.TypeId == "ClipPlayer")
            found = true;
    }
    EXPECT_TRUE(found);
    EXPECT_FALSE(GraphNodeRegistry::Get().GetAllTypes(Graph::kKindIdMaterial).empty());
}

TEST(GraphNodeRegistryTests, AnimationClipPlayerIsFindableByKindId)
{
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find("animation", "ClipPlayer");
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->DisplayName, "Clip Player");
    ASSERT_FALSE(meta->Ports.empty());
    EXPECT_EQ(meta->Ports.front().DataType, "pose");
    EXPECT_EQ(meta->Ports.front().Id, "poseOut");
}

TEST(GraphNodeRegistryTests, AnimationClipPlayerHasClipGuidParameter)
{
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find("animation", "ClipPlayer");
    ASSERT_NE(meta, nullptr);
    const NodeParamSpec* clipGuid = nullptr;
    for (const NodeParamSpec& param : meta->Parameters)
    {
        if (param.Id == "clipGuid")
            clipGuid = &param;
    }
    ASSERT_NE(clipGuid, nullptr);
    EXPECT_TRUE(clipGuid->Default.IsGuid());
    EXPECT_TRUE(clipGuid->Default.empty());

    const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
        "animation", "ClipPlayer", "nClip", 0.f, 0.f);
    auto it = node.Parameters.find("clipGuid");
    ASSERT_NE(it, node.Parameters.end());
    EXPECT_TRUE(it->second.IsGuid());
    EXPECT_TRUE(node.Parameters.GetString("clipGuid").empty());
}

TEST(GraphNodeRegistryTests, AnimationIkNodesHaveDistinctPosePortIds)
{
    (void)GraphNodeRegistry::Get();
    const char* kIk[] = {"LookAt", "TwoBoneIK", "FABRIK"};
    for (const char* typeId : kIk)
    {
        const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find("animation", typeId);
        ASSERT_NE(meta, nullptr) << typeId;
        const NodePortTemplate* inPort = nullptr;
        const NodePortTemplate* outPort = nullptr;
        for (const NodePortTemplate& port : meta->Ports)
        {
            if (port.Direction == Graph::PortDirection::In && port.DataType == "pose")
                inPort = &port;
            if (port.Direction == Graph::PortDirection::Out && port.DataType == "pose")
                outPort = &port;
        }
        ASSERT_NE(inPort, nullptr) << typeId;
        ASSERT_NE(outPort, nullptr) << typeId;
        EXPECT_EQ(inPort->Id, "pose") << typeId;
        EXPECT_NE(inPort->Id, outPort->Id) << typeId;
        EXPECT_EQ(outPort->Id, "poseOut") << typeId;

        const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
            "animation", typeId, "nIk", 0.f, 0.f);
        for (size_t i = 0; i < node.Ports.size(); ++i)
        {
            for (size_t j = i + 1; j < node.Ports.size(); ++j)
                EXPECT_NE(node.Ports[i].Id, node.Ports[j].Id) << typeId;
        }
    }
}

TEST(GraphNodeRegistryTests, AnimationCreateNodeByKindIdHasPorts)
{
    const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
        "animation", "Blend2", "nBlend", 0.f, 0.f);
    EXPECT_EQ(node.TypeId, "Blend2");
    ASSERT_EQ(node.Ports.size(), 3u);
    EXPECT_EQ(node.Ports.back().Id, "poseOut");
    EXPECT_EQ(node.Ports.back().Direction, Graph::PortDirection::Out);
    const auto categories = GraphNodeRegistry::Get().GetCategories("animation");
    EXPECT_FALSE(categories.empty());
    bool hasPose = false;
    bool hasIk = false;
    for (const std::string& c : categories)
    {
        if (c == "Pose")
            hasPose = true;
        if (c == "IK")
            hasIk = true;
    }
    EXPECT_TRUE(hasPose);
    EXPECT_TRUE(hasIk);
}

TEST(GraphNodeRegistryTests, MaterialKindIdStillResolvesFloatConstant)
{
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find("material", "FloatConstant");
    ASSERT_NE(meta, nullptr);
    const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
        "material", "FloatConstant", "nMat", 0.f, 0.f);
    ASSERT_NE(node.Parameters.find("value"), node.Parameters.end());
}

TEST(GraphNodeRegistryTests, AnimationRegisteredTypesAreFindableByKindId)
{
    GraphNodeRegistry& registry = GraphNodeRegistry::Get();
    const char* kTypes[] = {
        "ClipPlayer", "Blend2", "BlendSpace1D", "BlendSpace2D", "StateMachine",
        "State", "Entry",
        "LayeredBlend", "AdditiveBlend", "TwoBoneIK", "FABRIK", "LookAt", "MontageSlot",
        "OutputPose"};
    for (const char* typeId : kTypes)
    {
        const NodeTypeMeta* meta = registry.Find("animation", typeId);
        ASSERT_NE(meta, nullptr) << typeId;
        const Graph::Node node = registry.CreateNode("animation", typeId, "n", 0.f, 0.f);
        EXPECT_FALSE(node.Ports.empty()) << typeId;
    }
}

TEST(GraphNodeRegistryTests, AnimationStateAndEntryHaveStateCategoryAndTitle)
{
    GraphNodeRegistry& registry = GraphNodeRegistry::Get();
    const NodeTypeMeta* state = registry.Find("animation", "State");
    const NodeTypeMeta* entry = registry.Find("animation", "Entry");
    ASSERT_NE(state, nullptr);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(state->Category, "State");
    EXPECT_EQ(entry->Category, "State");

    auto hasPosePort = [](const NodeTypeMeta& meta)
    {
        for (const NodePortTemplate& port : meta.Ports)
        {
            if (port.DataType == "pose")
                return true;
        }
        return false;
    };
    EXPECT_FALSE(hasPosePort(*state));
    EXPECT_FALSE(hasPosePort(*entry));

    auto hasTitle = [](const NodeTypeMeta& meta)
    {
        for (const NodeParamSpec& param : meta.Parameters)
        {
            if (param.Id == "title")
                return true;
        }
        return false;
    };
    EXPECT_TRUE(hasTitle(*state));
    EXPECT_TRUE(hasTitle(*entry));

    const Graph::Node stateNode = registry.CreateNode("animation", "State", "nState", 0.f, 0.f);
    EXPECT_EQ(stateNode.Parameters.GetString("title"), "");
    ASSERT_EQ(stateNode.Ports.size(), 2u);
    EXPECT_EQ(stateNode.Ports[0].Id, "in");
    EXPECT_EQ(stateNode.Ports[0].DataType, "transition");
    EXPECT_EQ(stateNode.Ports[1].Id, "out");
    EXPECT_EQ(stateNode.Ports[1].DataType, "transition");
    const Graph::Node entryNode = registry.CreateNode("animation", "Entry", "nEntry", 0.f, 0.f);
    EXPECT_EQ(entryNode.Parameters.GetString("title"), "Entry");
    ASSERT_EQ(entryNode.Ports.size(), 1u);
    EXPECT_EQ(entryNode.Ports[0].Id, "out");
    EXPECT_EQ(entryNode.Ports[0].DataType, "transition");
}

TEST(GraphNodeRegistryTests, OutputPoseIsAPoseSinkWithASinglePoseIn)
{
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find("animation", "OutputPose");
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->DisplayName, "Output Pose");
    EXPECT_EQ(meta->Category, "Pose");
    ASSERT_EQ(meta->Ports.size(), 1u);
    EXPECT_EQ(meta->Ports[0].Id, "pose");
    EXPECT_EQ(meta->Ports[0].Direction, Graph::PortDirection::In);
    EXPECT_EQ(meta->Ports[0].DataType, "pose");

    const Graph::Node node = GraphNodeRegistry::Get().CreateNode(
        "animation", "OutputPose", "nOut", 0.f, 0.f);
    ASSERT_EQ(node.Ports.size(), 1u);
    EXPECT_EQ(node.Ports[0].Id, "pose");
    EXPECT_EQ(node.Ports[0].Direction, Graph::PortDirection::In);
    EXPECT_TRUE(meta->Parameters.empty());
}

TEST(GraphNodeRegistryTests, GameLogicKindIdTransitionHasConditionDefault)
{
    const auto byId = GraphNodeRegistry::Get().GetDefaultParameters(
        Graph::kKindIdGameLogic, "Transition");
    EXPECT_EQ(byId.count("condition"), 1u);
}

TEST(GraphNodeRegistryTests, FindNodeMetaDoesNotCoerceUnknownKindToMaterial)
{
    EXPECT_NE(GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "FloatConstant"), nullptr);
    EXPECT_EQ(GraphNodeRegistry::Get().FindNodeMeta("animation", "FloatConstant"), nullptr);
    EXPECT_EQ(GraphNodeRegistry::Get().FindNodeMeta("animation", "Output"), nullptr);
    EXPECT_EQ(GraphNodeRegistry::Get().FindNodeMeta("not_a_kind", "Time"), nullptr);
}

TEST(GraphNodeRegistryTests, KindIdRegisterAttachesKnownMaterialParameters)
{
    const NodeTypeMeta* existing = GraphNodeRegistry::Get().Find(Graph::kKindIdMaterial, "Time");
    ASSERT_NE(existing, nullptr);
    NodeTypeMeta copy = *existing;
    copy.Parameters.clear();
    GraphNodeRegistry::Get().Register(Graph::kKindIdMaterial, copy);
    const NodeTypeMeta* found = GraphNodeRegistry::Get().Find(Graph::kKindIdMaterial, "Time");
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->Parameters.size(), 2u);
    EXPECT_EQ(found->Parameters[0].Id, "scale");
    EXPECT_TRUE(found->Parameters[0].Default.IsFloat());
    EXPECT_DOUBLE_EQ(found->Parameters[0].Default.AsFloat(), 1.0);
    EXPECT_EQ(found->Parameters[1].Id, "offset");
    EXPECT_TRUE(found->Parameters[1].Default.IsFloat());
    EXPECT_DOUBLE_EQ(found->Parameters[1].Default.AsFloat(), 0.0);
}

TEST(GraphNodeRegistryTests, FresnelPowerRangeComesFromParameterSchema)
{
    const NodeTypeMeta* meta =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "Fresnel");
    ASSERT_NE(meta, nullptr);
    const auto range = FindNodeValueRange(meta, "power");
    ASSERT_TRUE(range.has_value());
    EXPECT_FLOAT_EQ(range->Min, 0.01f);
    EXPECT_FLOAT_EQ(range->Max, 64.0f);
}

TEST(GraphNodeRegistryTests, LerpWeightPortRangeIsNormalized)
{
    const NodeTypeMeta* meta =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "Lerp");
    ASSERT_NE(meta, nullptr);
    const auto range = FindNodeValueRange(meta, "weight");
    ASSERT_TRUE(range.has_value());
    EXPECT_FLOAT_EQ(range->Min, 0.0f);
    EXPECT_FLOAT_EQ(range->Max, 1.0f);
    EXPECT_FALSE(FindNodeValueRange(meta, "a").has_value());
    EXPECT_FALSE(FindNodeValueRange(meta, "b").has_value());
}

TEST(GraphNodeRegistryTests, OutputMetallicAndRoughnessPortsAreNormalized)
{
    const NodeTypeMeta* meta =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "SurfaceOutput");
    ASSERT_NE(meta, nullptr);
    const auto metallic = FindNodeValueRange(meta, "Metallic");
    ASSERT_TRUE(metallic.has_value());
    EXPECT_FLOAT_EQ(metallic->Min, 0.0f);
    EXPECT_FLOAT_EQ(metallic->Max, 1.0f);
    const auto roughness = FindNodeValueRange(meta, "Roughness");
    ASSERT_TRUE(roughness.has_value());
    EXPECT_FLOAT_EQ(roughness->Min, 0.0f);
    EXPECT_FLOAT_EQ(roughness->Max, 1.0f);
    EXPECT_FALSE(FindNodeValueRange(meta, "BaseColor").has_value());
}

TEST(GraphNodeRegistryTests, FloatConstantValueHasNoRange)
{
    const NodeTypeMeta* meta =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "FloatConstant");
    ASSERT_NE(meta, nullptr);
    EXPECT_FALSE(FindNodeValueRange(meta, "value").has_value());
}

TEST(GraphNodeRegistryTests, ParameterNodeVariableValueHasNoRange)
{
    const NodeTypeMeta* meta =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "FloatParameter");
    ASSERT_NE(meta, nullptr);
    EXPECT_FALSE(FindNodeValueRange(meta, "value").has_value());
    EXPECT_FALSE(FindNodeValueRange(meta, "variableName").has_value());
}

TEST(GraphNodeRegistryTests, ValueRangePrefersParameterSchemaOverPort)
{
    NodeTypeMeta meta;
    meta.TypeId = "RangeProbe";
    meta.Ports.push_back({"amount", Graph::PortDirection::In, "float", "Amount", 0.0f, 10.0f});
    meta.Parameters.push_back({"amount", Graph::GraphValue(0.5f), {}, 0.0f, 1.0f});
    const auto range = FindNodeValueRange(&meta, "amount");
    ASSERT_TRUE(range.has_value());
    EXPECT_FLOAT_EQ(range->Min, 0.0f);
    EXPECT_FLOAT_EQ(range->Max, 1.0f);
}

TEST(GraphNodeRegistryTests, ValueRangeFallsBackToPortWhenSchemaParamHasNoRange)
{
    NodeTypeMeta meta;
    meta.TypeId = "RangeProbe";
    meta.Ports.push_back({"amount", Graph::PortDirection::In, "float", "Amount", -2.0f, 2.0f});
    meta.Parameters.push_back({"amount", Graph::GraphValue(0.5f)});
    const auto range = FindNodeValueRange(&meta, "amount");
    ASSERT_TRUE(range.has_value());
    EXPECT_FLOAT_EQ(range->Min, -2.0f);
    EXPECT_FLOAT_EQ(range->Max, 2.0f);
    EXPECT_FALSE(FindNodeValueRange(nullptr, "amount").has_value());
}

TEST(GraphNodeRegistryTests, ParameterSwizzleAndComponentParamsCarryEnumOptions)
{
    const NodeTypeMeta* floatParam =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "FloatParameter");
    ASSERT_NE(floatParam, nullptr);
    const NodeParamSpec* component = nullptr;
    for (const NodeParamSpec& param : floatParam->Parameters)
    {
        if (param.Id == "component")
            component = &param;
    }
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->Options.size(), 4u);
    EXPECT_EQ(component->Options[0].Value, "x");
    EXPECT_EQ(component->Options[0].Label, "X");
    EXPECT_EQ(component->Options[3].Value, "w");

    const NodeTypeMeta* vec3Param =
        GraphNodeRegistry::Get().FindNodeMeta(Graph::kKindIdMaterial, "Vec3Parameter");
    ASSERT_NE(vec3Param, nullptr);
    const NodeParamSpec* swizzle = nullptr;
    for (const NodeParamSpec& param : vec3Param->Parameters)
    {
        if (param.Id == "swizzle")
            swizzle = &param;
    }
    ASSERT_NE(swizzle, nullptr);
    ASSERT_EQ(swizzle->Options.size(), 4u);
    EXPECT_EQ(swizzle->Options[0].Value, "xyz");
    EXPECT_EQ(swizzle->Options[0].Label, "XYZ");
    // The wire value stays the lowercase swizzle; the label is display only.
    EXPECT_EQ(swizzle->Options[3].Value, "xzw");
    EXPECT_EQ(swizzle->Options[3].Label, "XZW");

    // variableName is free text: an options list there would forbid new names.
    for (const NodeParamSpec& param : vec3Param->Parameters)
    {
        if (param.Id == "variableName")
            EXPECT_TRUE(param.Options.empty());
    }
}

TEST(GraphNodeRegistryTests, LegacySampleTextureAliasIsUncategorizedSoThePaletteListsItOnce)
{
    GraphNodeRegistry& registry = GraphNodeRegistry::Get();
    registry.EnsureMaterialShaderGraphTypesRegistered();

    const NodeTypeMeta* alias = registry.Find(Graph::kKindIdMaterial, "SampleTexture");
    const NodeTypeMeta* canonical = registry.Find(Graph::kKindIdMaterial, "SampleTexture2D");
    if (!alias || !canonical)
        GTEST_SKIP() << "Shader-graph reflection did not register the texture sample nodes.";

    // The alias stays findable (old graphs reference it) but carries no
    // category, so no category listing can show it beside the canonical type.
    EXPECT_TRUE(alias->Category.empty());
    EXPECT_FALSE(canonical->Category.empty());

    int listedUnderCanonicalCategory = 0;
    for (const NodeTypeMeta& type :
         registry.GetTypesInCategory(Graph::kKindIdMaterial, canonical->Category))
    {
        if (type.TypeId == "SampleTexture2D" || type.TypeId == "SampleTexture")
            ++listedUnderCanonicalCategory;
    }
    EXPECT_EQ(listedUnderCanonicalCategory, 1);

    for (const std::string& category : registry.GetCategories(Graph::kKindIdMaterial))
        EXPECT_FALSE(category.empty());
}
