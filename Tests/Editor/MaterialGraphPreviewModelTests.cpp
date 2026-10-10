// The preview projection is what makes a value edit realtime: a constant node's
// value stops being a literal in the shader and becomes a read from a material
// param lane. Everything downstream depends on two properties of that rewrite —
// the surface text no longer varies with the value (so the digest cannot move,
// so no recompile), and the value still arrives, in the lane the shader reads.

#include "ShaderGraph/MaterialGraphPreviewModel.h"

#include "Rendering/Materials/MaterialDocument.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using GameEngine::Editor::MakeMaterialGraphPreviewModel;
using GameEngine::Editor::MaterialGraphPreviewModel;

namespace
{

Graph::Node MakeColorConstant(const std::string& id, float r, float g, float b)
{
    Graph::Node node;
    node.Id = id;
    node.TypeId = "ColorConstant";
    // Authored as text, the way the editor writes them.
    node.Parameters["r"] = std::to_string(r);
    node.Parameters["g"] = std::to_string(g);
    node.Parameters["b"] = std::to_string(b);
    return node;
}

Graph::Node MakeFloatConstant(const std::string& id, float value)
{
    Graph::Node node;
    node.Id = id;
    node.TypeId = "FloatConstant";
    node.Parameters["value"] = std::to_string(value);
    return node;
}

std::string ParamText(const Graph::Model& model, const std::string& nodeId, const std::string& pin)
{
    const Graph::Node* node = model.FindNode(nodeId);
    if (!node)
        return {};
    const auto it = node->Parameters.find(pin);
    return it == node->Parameters.end() ? std::string{} : it->second.ToString();
}

} // namespace

TEST(MaterialGraphPreviewModelTest, ConstantValuesBecomeLaneReadsCarryingTheirValue)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    model.Nodes.push_back(MakeColorConstant("node_0", 0.08f, 0.28f, 1.0f));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);

    // Pins are taken in sorted key order, so b/g/r land on lanes 0/1/2. The order
    // itself does not matter — that it is fixed does, or a lane read would point
    // at a different value between two projections of the same graph.
    EXPECT_EQ(ParamText(preview.Model, "node_0", "b"), "Mat.uUser0.x");
    EXPECT_EQ(ParamText(preview.Model, "node_0", "g"), "Mat.uUser0.y");
    EXPECT_EQ(ParamText(preview.Model, "node_0", "r"), "Mat.uUser0.z");

    ASSERT_EQ(preview.LaneValues.size(), 3u);
    EXPECT_EQ(preview.LaneValues[0].first, "user0");
    EXPECT_FLOAT_EQ(preview.LaneValues[0].second, 1.0f); // b
    EXPECT_EQ(preview.LaneValues[2].first, "user2");
    EXPECT_FLOAT_EQ(preview.LaneValues[2].second, 0.08f); // r
    EXPECT_EQ(preview.UnboundScalars, 0u);
}

// The whole point: two graphs differing only in a constant's VALUE must project
// to the same surface text, or the digest moves and the edit costs a recompile.
TEST(MaterialGraphPreviewModelTest, ChangingAValueLeavesTheProjectedGraphIdentical)
{
    Graph::Model a;
    a.KindId = std::string(Graph::kKindIdMaterial);
    a.Nodes.push_back(MakeColorConstant("node_0", 0.08f, 0.28f, 1.0f));
    Graph::Model b = a;
    b.Nodes[0].Parameters["r"] = std::string("0.95");

    const MaterialGraphPreviewModel pa = MakeMaterialGraphPreviewModel(a);
    const MaterialGraphPreviewModel pb = MakeMaterialGraphPreviewModel(b);

    EXPECT_EQ(ParamText(pa.Model, "node_0", "r"), ParamText(pb.Model, "node_0", "r"));
    ASSERT_EQ(pa.LaneValues.size(), pb.LaneValues.size());
    EXPECT_NE(pa.LaneValues, pb.LaneValues)
        << "the value has to move somewhere — it moved into the lanes";
}

// Lanes are handed out in natural id order, so appending a node leaves the
// existing assignment alone. Model-order assignment would shift every later lane
// and rewrite every surface for one added node.
TEST(MaterialGraphPreviewModelTest, AppendingANodeDoesNotMoveExistingLanes)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    model.Nodes.push_back(MakeFloatConstant("node_9", 1.0f));
    const MaterialGraphPreviewModel before = MakeMaterialGraphPreviewModel(model);
    ASSERT_EQ(ParamText(before.Model, "node_9", "value"), "Mat.uUser0.x");

    // Inserted at the FRONT of the node list, with a higher generated id.
    model.Nodes.insert(model.Nodes.begin(), MakeFloatConstant("node_10", 2.0f));
    const MaterialGraphPreviewModel after = MakeMaterialGraphPreviewModel(model);

    EXPECT_EQ(ParamText(after.Model, "node_9", "value"), "Mat.uUser0.x")
        << "node_9 kept its lane; a lexical sort would have put node_10 first";
    // Lanes are scalars: the next one after uUser0.x is uUser0.y, not uUser1.x.
    EXPECT_EQ(ParamText(after.Model, "node_10", "value"), "Mat.uUser0.y");
}

TEST(MaterialGraphPreviewModelTest, PastTheLaneBudgetValuesKeepTheirLiterals)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    // 16 lanes exist; six colours is eighteen scalars.
    for (int i = 0; i < 6; ++i)
        model.Nodes.push_back(MakeColorConstant("node_" + std::to_string(i), 0.5f, 0.5f, 0.5f));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);

    EXPECT_EQ(preview.LaneValues.size(), 16u);
    EXPECT_EQ(preview.UnboundScalars, 2u);
    // The overflow keeps a literal rather than aliasing onto an occupied lane.
    // Sorted keys mean b and g fit; r is the one left over on the last node.
    EXPECT_EQ(ParamText(preview.Model, "node_5", "r"), "0.500000");
}

// A pin already holding an expression (or anything unparseable) must be left
// alone: rewriting it would splice a lane read over authored GLSL.
TEST(MaterialGraphPreviewModelTest, NonNumericParametersAreLeftUntouched)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    Graph::Node node = MakeFloatConstant("node_0", 1.0f);
    node.Parameters["value"] = std::string("sIn.uv0.x");
    model.Nodes.push_back(std::move(node));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);

    EXPECT_EQ(ParamText(preview.Model, "node_0", "value"), "sIn.uv0.x");
    EXPECT_TRUE(preview.LaneValues.empty());
}

TEST(MaterialGraphPreviewModelTest, LaneValuesReachTheMaterialDocument)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    model.Nodes.push_back(MakeColorConstant("node_0", 0.25f, 0.5f, 0.75f));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);
    MaterialDocument doc;
    Editor::ApplyPreviewLaneValues(preview, doc);

    ASSERT_TRUE(doc.properties.count("user1"));
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties["user1"]), 0.5f);
}

// Hostile / malformed authored values. A graph file is user-editable text, and
// before the projection an unparseable value was spliced into GLSL verbatim and
// failed the shader compile LOUDLY. The projection must not quietly turn that
// into a plausible-looking number.
TEST(MaterialGraphPreviewModelTest, MalformedValuesDoNotSilentlyBecomeNumbers)
{
    struct Case
    {
        const char* text;
        const char* what;
    };
    const Case cases[] = {
        {"", "empty"},
        {"1.0abc", "trailing garbage"},
        {"abc", "not a number"},
        {"--1", "double sign"},
        {"0x10", "hex"},
        {"1,5", "comma decimal"},
        {"  ", "whitespace only"},
    };

    for (const Case& c : cases)
    {
        Graph::Model model;
        model.KindId = std::string(Graph::kKindIdMaterial);
        Graph::Node node = MakeFloatConstant("node_0", 1.0f);
        node.Parameters["value"] = std::string(c.text);
        model.Nodes.push_back(std::move(node));

        const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);
        const std::string projected = ParamText(preview.Model, "node_0", "value");
        EXPECT_EQ(projected, std::string(c.text))
            << c.what << " (" << c.text << ") must be left for the compiler to reject, not "
            << "rewritten to a lane read";
        EXPECT_TRUE(preview.LaneValues.empty()) << c.what;
    }
}

// An out-of-range literal is rejected by the parser rather than folded to
// infinity, so it keeps its literal and stays the compiler's problem. Pinned
// because the alternative — an inf quietly reaching a uniform lane — would show
// as NaN pixels with nothing pointing at the cause.
TEST(MaterialGraphPreviewModelTest, OutOfRangeValuesKeepTheirLiteral)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    Graph::Node node = MakeFloatConstant("node_0", 1.0f);
    node.Parameters["value"] = std::string("1e400"); // overflows
    model.Nodes.push_back(std::move(node));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);

    EXPECT_TRUE(preview.LaneValues.empty());
    EXPECT_EQ(ParamText(preview.Model, "node_0", "value"), "1e400");
}

// The gap the first cut of this shipped with: only *Constant nodes were
// promoted, so a value typed into any other node — a Fresnel's power, a
// Multiply's operand — stayed a literal and still recompiled on every edit,
// while colours next to it updated live.
TEST(MaterialGraphPreviewModelTest, PortDefaultsOnOrdinaryNodesArePromotedToo)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    Graph::Node fresnel;
    fresnel.Id = "node_0";
    fresnel.TypeId = "Fresnel";
    fresnel.Parameters["power"] = std::string("2.6");
    model.Nodes.push_back(std::move(fresnel));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);

    EXPECT_EQ(ParamText(preview.Model, "node_0", "power"), "Mat.uUser0.x");
    ASSERT_EQ(preview.LaneValues.size(), 1u);
    EXPECT_FLOAT_EQ(preview.LaneValues[0].second, 2.6f);
}

// A wired pin ignores its default, so promoting it would spend a lane the shader
// never reads — and there are only sixteen.
TEST(MaterialGraphPreviewModelTest, WiredPinsDoNotConsumeLanes)
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);
    model.Nodes.push_back(MakeFloatConstant("node_0", 1.0f));

    Graph::Node multiply;
    multiply.Id = "node_1";
    multiply.TypeId = "VectorMultiply";
    multiply.Parameters["a"] = std::string("3.0");
    multiply.Parameters["b"] = std::string("4.0");
    model.Nodes.push_back(std::move(multiply));

    Graph::Edge wire;
    wire.Id = "link_0";
    wire.SourceNodeId = "node_0";
    wire.SourcePortId = "value";
    wire.TargetNodeId = "node_1";
    wire.TargetPortId = "a";
    model.Links.push_back(std::move(wire));

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);

    EXPECT_EQ(ParamText(preview.Model, "node_1", "a"), "3.0") << "wired: left alone";
    EXPECT_EQ(ParamText(preview.Model, "node_1", "b"), "Mat.uUser0.y") << "unwired: promoted";
    EXPECT_EQ(preview.LaneValues.size(), 2u) << "node_0's value and node_1's b, not a";
}
