#include "Graph/GraphGlslAuthoring.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/SgGraphModelBridge.h"

#include "Rendering/ShaderGraph/SgTagParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using GameEngine::Graph::FromJson;
using GameEngine::Graph::GraphObject;
using GameEngine::Graph::GraphValue;
using GameEngine::Graph::Kind;
using GameEngine::Graph::Model;
using GameEngine::Graph::Node;
using GameEngine::Graph::PortDirection;
using GameEngine::Graph::ToJson;
using GameEngine::Graph::ValueKind;
using GameEngine::Graph::kKindIdGameLogic;
using GameEngine::Graph::kKindIdMaterial;
using GameEngine::Graph::kModelFormatVersion;

TEST(GraphValueTest, ScalarsRoundTripThroughAssignment)
{
    GraphValue boolean = true;
    EXPECT_TRUE(boolean.IsBool());
    EXPECT_TRUE(boolean.AsBool());
    EXPECT_EQ(boolean.ToString(), "true");

    GraphValue integer = 42;
    EXPECT_TRUE(integer.IsInt());
    EXPECT_EQ(integer.AsInt(), 42);

    GraphValue real = 0.75;
    EXPECT_TRUE(real.IsFloat());
    EXPECT_DOUBLE_EQ(real.AsFloat(), 0.75);

    GraphValue text = "clip";
    EXPECT_TRUE(text.IsString());
    EXPECT_EQ(text.ToString(), "clip");
    EXPECT_TRUE(text.EqualsString("clip"));

    GraphValue guid = GraphValue::FromGuid("00000000-0000-4000-8000-000000000001");
    EXPECT_TRUE(guid.IsGuid());
    EXPECT_EQ(guid.ToString(), "00000000-0000-4000-8000-000000000001");
}

TEST(GraphValueTest, StringEncodingsCoerceToNumbersAndBools)
{
    GraphValue volume = "0.75";
    EXPECT_DOUBLE_EQ(volume.AsFloat(0.0), 0.75);
    EXPECT_EQ(volume.AsInt(0), 0);

    GraphValue flag = "true";
    EXPECT_TRUE(flag.AsBool(false));
    GraphValue off = "0";
    EXPECT_FALSE(off.AsBool(true));
}

TEST(GraphValueTest, FloatParseTrimsAsciiSpaceAndRejectsJunk)
{
    EXPECT_DOUBLE_EQ(GraphValue("  0.75").AsFloat(-1.0), 0.75);
    EXPECT_DOUBLE_EQ(GraphValue("0.75  ").AsFloat(-1.0), 0.75);
    EXPECT_DOUBLE_EQ(GraphValue("\t1e-3\n").AsFloat(-1.0), 0.001);
    EXPECT_DOUBLE_EQ(GraphValue("+0.5").AsFloat(-1.0), 0.5);
    EXPECT_DOUBLE_EQ(GraphValue("+.5").AsFloat(-1.0), 0.5);
    EXPECT_DOUBLE_EQ(GraphValue("0.75 extra").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("0x1p0").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("+0x1p0").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("-0x1p0").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("+-0x1p0").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("++1").AsFloat(-1.0), -1.0);
    EXPECT_EQ(GraphValue("4.9").AsInt(-1), 4);
    EXPECT_EQ(GraphValue("1e20").AsInt(-1), -1);
    EXPECT_EQ(GraphValue("-1e20").AsInt(-1), -1);
    EXPECT_EQ(GraphValue("9223372036854775808").AsInt(-1), -1);
    EXPECT_DOUBLE_EQ(GraphValue("inf").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("+inf").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("-inf").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("nan").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("INFINITY").AsFloat(-1.0), -1.0);
    EXPECT_DOUBLE_EQ(GraphValue("").AsFloat(-1.0), -1.0);
    EXPECT_EQ(GraphValue("  42  ").AsInt(-1), 42);
    EXPECT_EQ(GraphValue("+42").AsInt(-1), 42);
    EXPECT_EQ(GraphValue("++42").AsInt(-1), -1);
    EXPECT_EQ(GraphValue("42x").AsInt(-1), -1);
    EXPECT_TRUE(GraphValue(" true ").AsBool(false));
    EXPECT_TRUE(GraphValue(" 1 ").AsBool(false));
}

TEST(GraphValueTest, AssignFromTextPreservesKind)
{
    GraphValue demoted = 1.0f;
    demoted = std::string("0.25");
    EXPECT_TRUE(demoted.IsString());

    GraphValue real = 1.0f;
    ASSERT_TRUE(real.AssignFromText("0.25"));
    EXPECT_TRUE(real.IsFloat());
    EXPECT_DOUBLE_EQ(real.AsFloat(), 0.25);
    ASSERT_TRUE(real.AssignFromText("+0.5"));
    EXPECT_TRUE(real.IsFloat());
    EXPECT_DOUBLE_EQ(real.AsFloat(), 0.5);
    EXPECT_FALSE(real.AssignFromText("0x1p0"));
    EXPECT_TRUE(real.IsFloat());
    EXPECT_DOUBLE_EQ(real.AsFloat(), 0.5);
    EXPECT_FALSE(real.AssignFromText("+-0x1p0"));
    EXPECT_FALSE(real.AssignFromText("++1"));
    EXPECT_FALSE(real.AssignFromText("abc"));
    EXPECT_TRUE(real.IsFloat());
    EXPECT_DOUBLE_EQ(real.AsFloat(), 0.5);
    EXPECT_FALSE(real.AssignFromText("inf"));
    EXPECT_FALSE(real.AssignFromText("nan"));
    EXPECT_TRUE(real.IsFloat());
    EXPECT_DOUBLE_EQ(real.AsFloat(), 0.5);

    GraphValue integer = 2;
    ASSERT_TRUE(integer.AssignFromText("3"));
    EXPECT_TRUE(integer.IsInt());
    EXPECT_EQ(integer.AsInt(), 3);
    ASSERT_TRUE(integer.AssignFromText("4.9"));
    EXPECT_TRUE(integer.IsInt());
    EXPECT_EQ(integer.AsInt(), 4);
    EXPECT_FALSE(integer.AssignFromText("1e20"));
    EXPECT_TRUE(integer.IsInt());
    EXPECT_EQ(integer.AsInt(), 4);

    GraphValue flag = false;
    ASSERT_TRUE(flag.AssignFromText("true"));
    EXPECT_TRUE(flag.IsBool());
    EXPECT_TRUE(flag.AsBool());
    EXPECT_FALSE(flag.AssignFromText("abc"));
    EXPECT_TRUE(flag.IsBool());
    EXPECT_TRUE(flag.AsBool());

    GraphValue text = "clip";
    ASSERT_TRUE(text.AssignFromText("idle"));
    EXPECT_TRUE(text.IsString());
    EXPECT_EQ(text.ToString(), "idle");

    GraphValue guid = GraphValue::FromGuid("old");
    ASSERT_TRUE(guid.AssignFromText("00000000-0000-4000-8000-000000000001"));
    EXPECT_TRUE(guid.IsGuid());
    EXPECT_EQ(guid.ToString(), "00000000-0000-4000-8000-000000000001");

    GraphValue list(std::vector<GraphValue>{GraphValue(1.0)});
    EXPECT_FALSE(list.AssignFromText("1"));
    EXPECT_TRUE(list.IsList());

    GraphValue unset;
    ASSERT_TRUE(unset.AssignFromText("new"));
    EXPECT_TRUE(unset.IsString());
    EXPECT_EQ(unset.ToString(), "new");
}

TEST(GraphValueTest, AssignFromTextKeepsTypedJsonDump)
{
    Node node;
    node.Id = "n0";
    node.TypeId = "ColorConstant";
    node.Parameters["r"] = 1.0;
    node.Parameters["g"] = 1.0;
    node.Parameters["b"] = 1.0;
    ASSERT_TRUE(node.Parameters.find("r")->second.AssignFromText("0.25"));
    ASSERT_TRUE(node.Parameters.find("g")->second.AssignFromText("0.5"));
    ASSERT_TRUE(node.Parameters.find("b")->second.AssignFromText("0.75"));

    Model model;
    model.KindId.assign(kKindIdMaterial);
    model.Nodes.push_back(std::move(node));
    const std::string json = ToJson(model);
    EXPECT_EQ(json.find("\"r\": \"0.25\""), std::string::npos);
    EXPECT_EQ(json.find("\"g\": \"0.5\""), std::string::npos);
    EXPECT_EQ(json.find("\"b\": \"0.75\""), std::string::npos);

    Model reloaded;
    ASSERT_TRUE(FromJson(json, reloaded));
    ASSERT_EQ(reloaded.Nodes.size(), 1u);
    const GraphObject& parameters = reloaded.Nodes[0].Parameters;
    auto r = parameters.find("r");
    auto g = parameters.find("g");
    auto b = parameters.find("b");
    ASSERT_NE(r, parameters.end());
    ASSERT_NE(g, parameters.end());
    ASSERT_NE(b, parameters.end());
    EXPECT_TRUE(r->second.IsFloat());
    EXPECT_TRUE(g->second.IsFloat());
    EXPECT_TRUE(b->second.IsFloat());
    EXPECT_DOUBLE_EQ(r->second.AsFloat(), 0.25);
    EXPECT_DOUBLE_EQ(g->second.AsFloat(), 0.5);
    EXPECT_DOUBLE_EQ(b->second.AsFloat(), 0.75);
}

TEST(GraphObjectTest, InsertionOrderAndEmplaceDoNotOverwrite)
{
    GraphObject object;
    object["b"] = "second";
    object["a"] = "first";
    auto [it, inserted] = object.emplace("b", GraphValue("ignored"));
    EXPECT_FALSE(inserted);
    EXPECT_EQ(it->second.ToString(), "second");
    ASSERT_EQ(object.size(), 2u);
    EXPECT_EQ(object.begin()->first, "b");
    EXPECT_EQ(std::next(object.begin())->first, "a");
}

TEST(GraphSerializationTest, WritesCurrentVersion)
{
    Model model;
    model.KindId.assign(kKindIdGameLogic);
    const std::string json = ToJson(model);
    EXPECT_NE(json.find("\"version\": 2"), std::string::npos);
    Model reloaded;
    ASSERT_TRUE(FromJson(json, reloaded));
    EXPECT_EQ(reloaded.Version, kModelFormatVersion);
}

TEST(GraphValueTest, AssignParameterFromTextPreservesKind)
{
    Node node;
    node.Parameters["r"] = 1.0;
    ASSERT_TRUE(node.AssignParameterFromText("r", "0.25"));
    EXPECT_TRUE(node.Parameters.find("r")->second.IsFloat());
    EXPECT_DOUBLE_EQ(node.Parameters.find("r")->second.AsFloat(), 0.25);
    EXPECT_FALSE(node.AssignParameterFromText("r", "inf"));
    EXPECT_TRUE(node.Parameters.find("r")->second.IsFloat());
    EXPECT_DOUBLE_EQ(node.Parameters.find("r")->second.AsFloat(), 0.25);

    ASSERT_TRUE(node.AssignParameterFromText("missing", "hello"));
    EXPECT_TRUE(node.Parameters.find("missing")->second.IsString());
    EXPECT_EQ(node.Parameters.find("missing")->second.ToString(), "hello");
}

TEST(GraphSerializationTest, UnknownKindIdRoundTripsAndDoesNotBecomeGameLogic)
{
    const char* json = R"JSON({
      "version": 2,
      "kind": "animation",
      "nodes": [],
      "links": []
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(json, model));
    EXPECT_EQ(model.KindId, "animation");
    Kind parsed = Kind::Material;
    EXPECT_FALSE(GameEngine::Graph::GraphTypeRegistry::TryParseKind(model.KindId, parsed));
    EXPECT_EQ(parsed, Kind::Material);

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded));
    EXPECT_EQ(reloaded.KindId, "animation");
}

TEST(GraphSerializationTest, EmptyKindIdRoundTripsAndDoesNotBecomeGameLogic)
{
    const char* json = R"JSON({
      "version": 2,
      "kind": "",
      "nodes": [],
      "links": []
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(json, model));
    EXPECT_EQ(model.KindId, "");
    EXPECT_EQ(ToJson(model).find("\"kind\": \"game_logic\""), std::string::npos);

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded));
    EXPECT_EQ(reloaded.KindId, "");
}

TEST(GraphSerializationTest, MissingKindKeyDefaultsToGameLogic)
{
    const char* json = R"JSON({
      "version": 2,
      "nodes": [],
      "links": []
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(json, model));
    EXPECT_EQ(model.KindId, kKindIdGameLogic);
}

TEST(GraphSerializationTest, V1AllStringParametersStillLoad)
{
    const char* v1 = R"JSON({
      "kind": "game_logic",
      "nodes": [
        {
          "id": "n0",
          "typeId": "PlaySound",
          "parameters": { "clipGuid": "sound-guid", "volume": "0.75", "loop": "false" },
          "pins": []
        }
      ],
      "links": []
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(v1, model));
    EXPECT_EQ(model.Version, 1);
    ASSERT_EQ(model.Nodes.size(), 1u);
    EXPECT_EQ(model.Nodes[0].Parameters.GetString("clipGuid"), "sound-guid");
    EXPECT_DOUBLE_EQ(model.Nodes[0].Parameters.GetFloat("volume", 0.0), 0.75);
    EXPECT_FALSE(model.Nodes[0].Parameters.GetBool("loop", true));
}

TEST(GraphSerializationTest, NonStringParametersAreNotDropped)
{
    const char* typed = R"JSON({
      "kind": "game_logic",
      "nodes": [
        {
          "id": "n0",
          "typeId": "FloatConstant",
          "parameters": {
            "value": 3.5,
            "enabled": true,
            "count": 2,
            "nested": { "x": 1, "y": [0, 1] }
          },
          "pins": []
        }
      ],
      "links": []
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(typed, model));
    ASSERT_EQ(model.Nodes.size(), 1u);
    const GraphObject& parameters = model.Nodes[0].Parameters;
    auto valueIt = parameters.find("value");
    ASSERT_NE(valueIt, parameters.end());
    EXPECT_TRUE(valueIt->second.IsFloat() || valueIt->second.IsInt());
    EXPECT_DOUBLE_EQ(valueIt->second.AsFloat(), 3.5);

    auto enabledIt = parameters.find("enabled");
    ASSERT_NE(enabledIt, parameters.end());
    EXPECT_TRUE(enabledIt->second.IsBool());
    EXPECT_TRUE(enabledIt->second.AsBool());

    auto countIt = parameters.find("count");
    ASSERT_NE(countIt, parameters.end());
    EXPECT_TRUE(countIt->second.IsInt());
    EXPECT_EQ(countIt->second.AsInt(), 2);

    auto nestedIt = parameters.find("nested");
    ASSERT_NE(nestedIt, parameters.end());
    ASSERT_TRUE(nestedIt->second.IsObject());
    const GraphObject* nested = nestedIt->second.TryObject();
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->GetInt("x"), 1);
    const GraphValue* y = nullptr;
    if (auto yIt = nested->find("y"); yIt != nested->end())
        y = &yIt->second;
    ASSERT_NE(y, nullptr);
    ASSERT_TRUE(y->IsList());
    ASSERT_EQ(y->TryList()->size(), 2u);
}

TEST(GraphSerializationTest, UnknownKeysRoundTripIncludingExampleDoc)
{
    const char* withExtra = R"JSON({
      "kind": "game_logic",
      "_example": "FSM that plays a sound then waits",
      "nodes": [
        {
          "id": "n0",
          "typeId": "Entry",
          "note": "start here",
          "pins": [ { "id": "out", "direction": "out", "dataType": "flow", "displayName": "Out" } ]
        }
      ],
      "links": []
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(withExtra, model));
    EXPECT_EQ(model.Passthrough.GetString("_example"), "FSM that plays a sound then waits");
    ASSERT_EQ(model.Nodes.size(), 1u);
    EXPECT_EQ(model.Nodes[0].Passthrough.GetString("note"), "start here");

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded));
    EXPECT_EQ(reloaded.Passthrough.GetString("_example"), "FSM that plays a sound then waits");
    ASSERT_EQ(reloaded.Nodes.size(), 1u);
    EXPECT_EQ(reloaded.Nodes[0].Passthrough.GetString("note"), "start here");
}

TEST(GraphSerializationTest, WriteReadWriteIsByteIdentical)
{
    Model model;
    model.KindId.assign(kKindIdGameLogic);
    model.Passthrough["_example"] = "docs";
    model.Extensions["vendor.x"] = 1;

    Node node;
    node.Id = "node_0";
    node.TypeId = "PlaySound";
    node.PositionX = 12.5f;
    node.PositionY = -4.0f;
    node.Ports.push_back({"in", PortDirection::In, "flow", "In"});
    node.Parameters["clipGuid"] = "sound-guid";
    node.Parameters["volume"] = 0.75;
    node.Parameters["loop"] = false;
    node.Extensions["ui.collapsed"] = true;
    node.Passthrough["author"] = "test";
    model.Nodes.push_back(std::move(node));

    const std::string first = ToJson(model);
    Model loaded;
    ASSERT_TRUE(FromJson(first, loaded));
    const std::string second = ToJson(loaded);
    EXPECT_EQ(first, second);

    Model third;
    ASSERT_TRUE(FromJson(second, third));
    EXPECT_EQ(ToJson(third), second);
}

TEST(GraphSerializationTest, InvalidJsonReturnsFalse)
{
    Model model;
    EXPECT_FALSE(FromJson("{", model));
    EXPECT_FALSE(FromJson("", model));
}

TEST(GraphSerializationTest, DanglingLinkFailsValidation)
{
    const char* dangling = R"JSON({
      "kind": "game_logic",
      "nodes": [ { "id": "n0", "typeId": "Entry", "pins": [] } ],
      "links": [ {
        "id": "l0",
        "sourceNodeId": "n0",
        "sourcePinId": "out",
        "targetNodeId": "missing",
        "targetPinId": "in"
      } ]
    })JSON";
    Model model;
    EXPECT_FALSE(FromJson(dangling, model));
}

TEST(GraphSerializationTest, GuidKindDemotesToStringOnLoad)
{
    Model model;
    Node node;
    node.Id = "n0";
    node.TypeId = "PlaySound";
    node.Parameters["clipGuid"] = GraphValue::FromGuid("00000000-0000-4000-8000-000000000001");
    model.Nodes.push_back(std::move(node));

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded));
    auto it = reloaded.Nodes[0].Parameters.find("clipGuid");
    ASSERT_NE(it, reloaded.Nodes[0].Parameters.end());
    EXPECT_TRUE(it->second.IsString());
    EXPECT_TRUE(it->second.EqualsString("00000000-0000-4000-8000-000000000001"));
}

TEST(GraphSerializationTest, PortAndEdgeUnknownKeysRoundTrip)
{
    const char* json = R"JSON({
      "kind": "game_logic",
      "nodes": [{
        "id": "n0",
        "typeId": "Entry",
        "pins": [{
          "id": "out", "direction": "out", "dataType": "flow", "displayName": "Out",
          "color": "#ff0",
          "extensions": { "ui.hidden": true }
        }]
      }],
      "links": [{
        "id": "l0",
        "sourceNodeId": "n0",
        "sourcePinId": "out",
        "targetNodeId": "n0",
        "targetPinId": "out",
        "curved": false
      }]
    })JSON";

    Model model;
    ASSERT_TRUE(FromJson(json, model));
    ASSERT_EQ(model.Nodes.size(), 1u);
    ASSERT_EQ(model.Nodes[0].Ports.size(), 1u);
    EXPECT_EQ(model.Nodes[0].Ports[0].Passthrough.GetString("color"), "#ff0");
    ASSERT_TRUE(model.Nodes[0].Ports[0].Passthrough.find("extensions") != model.Nodes[0].Ports[0].Passthrough.end());
    ASSERT_EQ(model.Links.size(), 1u);
    EXPECT_FALSE(model.Links[0].Passthrough.GetBool("curved", true));

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded));
    EXPECT_EQ(reloaded.Nodes[0].Ports[0].Passthrough.GetString("color"), "#ff0");
    EXPECT_FALSE(reloaded.Links[0].Passthrough.GetBool("curved", true));
}

TEST(GraphSerializationTest, GlslJsonFenceRoundTripsAndKeepsTagsFirst)
{
    using GameEngine::Graph::AppendAuthoringJsonFence;
    using GameEngine::Graph::ExtractAuthoringJson;
    using GameEngine::Graph::kGlslJsonFenceBegin;

    Model model;
    model.KindId.assign(kKindIdMaterial);
    Node node;
    node.Id = "node_color";
    node.TypeId = "ColorConstant";
    node.Parameters["r"] = 1.0;
    model.Nodes.push_back(std::move(node));

    const std::string tags = "// @sg-graph name=FenceTest\n// @sg-node id=node_color type=ColorConstant\n";
    const std::string fenced = AppendAuthoringJsonFence(tags, ToJson(model));
    EXPECT_EQ(fenced.find("// @sg-graph"), 0u);
    EXPECT_GT(fenced.find(kGlslJsonFenceBegin), fenced.find("// @sg-graph"));

    std::size_t commentLines = 0;
    {
        std::istringstream stream(fenced);
        std::string line;
        while (std::getline(stream, line) && commentLines < 64)
        {
            if (line.find("@sg-graph") != std::string::npos)
                break;
            if (!line.empty())
                ++commentLines;
        }
        EXPECT_LT(commentLines, 64u);
    }

    const auto extracted = ExtractAuthoringJson(fenced);
    ASSERT_TRUE(extracted.has_value());
    Model reloaded;
    ASSERT_TRUE(FromJson(*extracted, reloaded));
    ASSERT_EQ(reloaded.Nodes.size(), 1u);
    EXPECT_EQ(reloaded.Nodes[0].Id, "node_color");
    EXPECT_DOUBLE_EQ(reloaded.Nodes[0].Parameters.GetFloat("r"), 1.0);

    const std::string again = AppendAuthoringJsonFence(tags, ToJson(reloaded));
    EXPECT_EQ(ExtractAuthoringJson(again), extracted);
}

TEST(GraphSerializationTest, NamedExtensionsBucketSurvives)
{
    const char* json = R"JSON({
      "kind": "material",
      "extensions": { "sg.preview": { "spin": true } },
      "nodes": [],
      "links": []
    })JSON";
    Model model;
    ASSERT_TRUE(FromJson(json, model));
    const GraphValue* preview = nullptr;
    if (auto it = model.Extensions.find("sg.preview"); it != model.Extensions.end())
        preview = &it->second;
    ASSERT_NE(preview, nullptr);
    ASSERT_TRUE(preview->IsObject());
    EXPECT_TRUE(preview->TryObject()->GetBool("spin"));

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded));
    auto again = reloaded.Extensions.find("sg.preview");
    ASSERT_NE(again, reloaded.Extensions.end());
    EXPECT_TRUE(again->second.TryObject()->GetBool("spin"));
}

TEST(GraphSerializationTest, InvalidFenceJsonFailsClosed)
{
    using GameEngine::Graph::LoadModelFromShaderGraphComments;
    const char* comments =
        "// @sg-graph name=Broken\n"
        "// GE-GRAPH-JSON-BEGIN\n"
        "// {not-json\n"
        "// GE-GRAPH-JSON-END\n";
    Model model;
    EXPECT_FALSE(LoadModelFromShaderGraphComments(comments, model));
}

TEST(GraphSerializationTest, EmptyFenceFailsClosed)
{
    using GameEngine::Graph::LoadModelFromShaderGraphComments;
    const char* comments =
        "// @sg-graph name=Broken\n"
        "// GE-GRAPH-JSON-BEGIN\n"
        "// GE-GRAPH-JSON-END\n";
    Model model;
    EXPECT_FALSE(LoadModelFromShaderGraphComments(comments, model));
}

TEST(GraphSerializationTest, UnclosedFenceFailsClosed)
{
    using GameEngine::Graph::LoadModelFromShaderGraphComments;
    const char* comments =
        "// @sg-graph name=Broken\n"
        "// GE-GRAPH-JSON-BEGIN\n"
        "// {\"version\":2,\"kind\":\"material\"}\n";
    Model model;
    EXPECT_FALSE(LoadModelFromShaderGraphComments(comments, model));
}

// Editor-shaped save/reopen: load a tags-only graph, serialize it the way
// SaveGraphToPath does (tag block + authoring JSON fence), and load the result
// again. Lighting and unknown tags must survive; the reopen must succeed.
TEST(GraphSerializationTest, EditorSaveShapeRoundTripsTagsAndReopens)
{
    using GameEngine::Graph::AppendAuthoringJsonFence;
    using GameEngine::Graph::LoadModelFromShaderGraphComments;
    using GameEngine::GraphModelToSgDocument;
    using GameEngine::ShaderGraph::SerializeGraphDocumentTags;

    const char* source =
        "// @sg-graph     RoundTrip\n"
        "// @sg-version   1\n"
        "// @sg-stage     surface\n"
        "// @sg-lighting  StandardPBR\n"
        "// @sg-futuretag mode=fancy\n"
        "// @sg-node      c0 type=ColorConstant pos=(0,0) r=1.0\n"
        "// @sg-node      out type=SurfaceOutput pos=(200,0)\n"
        "// @sg-edge      c0.value -> out.BaseColor\n";

    Model model;
    ASSERT_TRUE(LoadModelFromShaderGraphComments(source, model));
    ASSERT_EQ(model.UnknownSgTags.size(), 1u);
    EXPECT_EQ(model.UnknownSgTags[0], "// @sg-futuretag mode=fancy");
    EXPECT_EQ(model.LightingModel, "StandardPBR");

    const auto doc = GraphModelToSgDocument(model, "RoundTrip");
    const std::string saved = AppendAuthoringJsonFence(SerializeGraphDocumentTags(doc), ToJson(model));
    EXPECT_NE(saved.find("// @sg-lighting  StandardPBR"), std::string::npos) << saved;
    EXPECT_NE(saved.find("// @sg-futuretag mode=fancy"), std::string::npos) << saved;

    Model reloaded;
    ASSERT_TRUE(LoadModelFromShaderGraphComments(saved, reloaded)) << saved;
    EXPECT_EQ(reloaded.LightingModel, "StandardPBR");
    ASSERT_EQ(reloaded.UnknownSgTags.size(), 1u);
    EXPECT_EQ(reloaded.UnknownSgTags[0], "// @sg-futuretag mode=fancy");
    ASSERT_EQ(reloaded.Nodes.size(), model.Nodes.size());
    ASSERT_EQ(reloaded.Links.size(), model.Links.size());
}

// A material graph saved by the editor stores links with shader-graph output
// aliases ("value"/"out") while ports carry registry ids ("result"). The fence
// must reopen: aliases resolve against a node's single output port.
TEST(GraphSerializationTest, SavedFenceWithSgAliasOutPortsReopens)
{
    Model model;
    model.KindId.assign(kKindIdMaterial);

    Node color;
    color.Id = "node_color";
    color.TypeId = "ColorConstant";
    color.Ports.push_back({"r", PortDirection::In, "float", "r"});
    color.Ports.push_back({"g", PortDirection::In, "float", "g"});
    color.Ports.push_back({"b", PortDirection::In, "float", "b"});
    color.Ports.push_back({"result", PortDirection::Out, "float3", "Result"});
    model.Nodes.push_back(std::move(color));

    Node output;
    output.Id = "node_output";
    output.TypeId = "Output";
    output.Ports.push_back({"color", PortDirection::In, "float4", "Color"});
    model.Nodes.push_back(std::move(output));

    GameEngine::Graph::Edge link;
    link.Id = "node_color_node_output_BaseColor";
    link.SourceNodeId = "node_color";
    link.SourcePortId = "value"; // sg alias for the single output port "result"
    link.TargetNodeId = "node_output";
    link.TargetPortId = "color";
    model.Links.push_back(std::move(link));

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded)) << ToJson(model);
    ASSERT_EQ(reloaded.Links.size(), 1u);
    EXPECT_EQ(reloaded.Links[0].SourcePortId, "value");
}

TEST(GraphSerializationTest, AliasDoesNotMatchMultiOutputNode)
{
    Model model;
    Node split;
    split.Id = "split";
    split.Ports.push_back({"x", PortDirection::Out, "float", "X"});
    split.Ports.push_back({"y", PortDirection::Out, "float", "Y"});
    Node sink;
    sink.Id = "sink";
    sink.Ports.push_back({"in", PortDirection::In, "float", "In"});
    model.Nodes.push_back(std::move(split));
    model.Nodes.push_back(std::move(sink));
    GameEngine::Graph::Edge link;
    link.Id = "l0";
    link.SourceNodeId = "split";
    link.SourcePortId = "out"; // ambiguous: two outputs, alias must not resolve
    link.TargetNodeId = "sink";
    link.TargetPortId = "in";
    model.Links.push_back(std::move(link));
    EXPECT_FALSE(model.Validate());
}

// A tag-parsed model has no hydrated ports; saving it writes empty port arrays.
// Reopening such a fence must not reject links against the empty lists.
TEST(GraphSerializationTest, LinksBetweenPinlessNodesPassValidate)
{
    Model model;
    model.KindId.assign(kKindIdMaterial);
    Node a;
    a.Id = "a";
    a.TypeId = "ColorConstant";
    Node b;
    b.Id = "b";
    b.TypeId = "Output";
    model.Nodes.push_back(std::move(a));
    model.Nodes.push_back(std::move(b));
    GameEngine::Graph::Edge link;
    link.Id = "l0";
    link.SourceNodeId = "a";
    link.SourcePortId = "value";
    link.TargetNodeId = "b";
    link.TargetPortId = "color";
    model.Links.push_back(std::move(link));

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(model), reloaded)) << ToJson(model);
}

// The surface output's ports carry a single vocabulary (BaseColor/Metallic/
// Roughness/...); a link into the sink must name the port exactly as the node
// declares it — the retired lowercase editor spellings are not aliased.
TEST(GraphSerializationTest, OutputTargetPortRequiresExactSpelling)
{
    auto makeModel = [](const char* portId, const char* linkTargetPortId) {
        Model model;
        model.KindId.assign(kKindIdMaterial);
        Node source;
        source.Id = "node_roughness";
        source.TypeId = "FloatParameter";
        source.Ports.push_back({"value", PortDirection::Out, "float", "Value"});
        Node output;
        output.Id = "node_output";
        output.TypeId = "SurfaceOutput";
        output.Ports.push_back({portId, PortDirection::In, "float", "Roughness"});
        model.Nodes.push_back(std::move(source));
        model.Nodes.push_back(std::move(output));
        GameEngine::Graph::Edge link;
        link.Id = "l0";
        link.SourceNodeId = "node_roughness";
        link.SourcePortId = "value";
        link.TargetNodeId = "node_output";
        link.TargetPortId = linkTargetPortId;
        model.Links.push_back(std::move(link));
        return model;
    };

    Model reloaded;
    ASSERT_TRUE(FromJson(ToJson(makeModel("Roughness", "Roughness")), reloaded));
    // A mixed spelling no longer validates: nothing translates it downstream.
    EXPECT_FALSE(FromJson(ToJson(makeModel("Roughness", "smoothness")), reloaded));
    EXPECT_FALSE(FromJson(ToJson(makeModel("smoothness", "Roughness")), reloaded));
}

TEST(GraphSerializationTest, FromJsonFailureLeavesModelUntouched)
{
    Model model;
    Node sentinel;
    sentinel.Id = "sentinel";
    sentinel.TypeId = "Entry";
    model.Nodes.push_back(std::move(sentinel));

    // Valid JSON, invalid graph: link to a missing node fails Validate.
    const char* dangling = R"JSON({
      "kind": "game_logic",
      "nodes": [ { "id": "n0", "typeId": "Entry", "pins": [] } ],
      "links": [ {
        "id": "l0",
        "sourceNodeId": "n0",
        "sourcePinId": "out",
        "targetNodeId": "missing",
        "targetPinId": "in"
      } ]
    })JSON";
    ASSERT_FALSE(FromJson(dangling, model));
    ASSERT_EQ(model.Nodes.size(), 1u);
    EXPECT_EQ(model.Nodes[0].Id, "sentinel");

    // Unparseable JSON must not touch the model either.
    ASSERT_FALSE(FromJson("{not-json", model));
    ASSERT_EQ(model.Nodes.size(), 1u);
    EXPECT_EQ(model.Nodes[0].Id, "sentinel");
}

// The undo snapshot path: ToJson of a hydrated material graph (including the
// EngineInput display-split nodes node_0/node_1) must load back through
// FromJson and re-serialize byte-identically.
TEST(GraphSerializationTest, HydratedEngineInputSplitGraphSnapshotRoundTrips)
{
    Model model;
    model.KindId.assign(kKindIdMaterial);

    Node normal;
    normal.Id = "node_0";
    normal.TypeId = "NormalVector";
    normal.Ports.push_back({"normal", PortDirection::Out, "float3", "Normal"});
    model.Nodes.push_back(std::move(normal));

    Node view;
    view.Id = "node_1";
    view.TypeId = "ViewDirection";
    view.Ports.push_back({"view", PortDirection::Out, "float3", "View"});
    model.Nodes.push_back(std::move(view));

    Node fresnel;
    fresnel.Id = "node_fresnel";
    fresnel.TypeId = "Fresnel";
    fresnel.Ports.push_back({"normal", PortDirection::In, "float3", "normal"});
    fresnel.Ports.push_back({"view", PortDirection::In, "float3", "view"});
    fresnel.Ports.push_back({"power", PortDirection::In, "float", "power"});
    fresnel.Ports.push_back({"result", PortDirection::Out, "float", "Result"});
    fresnel.Parameters["power"] = 4.0;
    model.Nodes.push_back(std::move(fresnel));

    Node roughness;
    roughness.Id = "node_roughness";
    roughness.TypeId = "FloatParameter";
    roughness.Ports.push_back({"value", PortDirection::Out, "float", "Value"});
    roughness.Parameters["variableName"] = "uRoughness";
    model.Nodes.push_back(std::move(roughness));

    Node output;
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports.push_back({"BaseColor", PortDirection::In, "float4", "Base Color"});
    output.Ports.push_back({"Metallic", PortDirection::In, "float", "Metallic"});
    output.Ports.push_back({"Roughness", PortDirection::In, "float", "Roughness"});
    output.Ports.push_back({"Emissive", PortDirection::In, "float3", "Emissive"});
    model.Nodes.push_back(std::move(output));

    auto addLink = [&](const char* id, const char* srcNode, const char* srcPort,
                       const char* dstNode, const char* dstPort) {
        GameEngine::Graph::Edge link;
        link.Id = id;
        link.SourceNodeId = srcNode;
        link.SourcePortId = srcPort;
        link.TargetNodeId = dstNode;
        link.TargetPortId = dstPort;
        model.Links.push_back(std::move(link));
    };
    addLink("l0", "node_0", "normal", "node_fresnel", "normal");
    addLink("l1", "node_1", "view", "node_fresnel", "view");
    addLink("l2", "node_roughness", "value", "node_output", "Roughness");
    addLink("l3", "node_fresnel", "result", "node_output", "Emissive");

    const std::string first = ToJson(model);
    Model reloaded;
    ASSERT_TRUE(FromJson(first, reloaded)) << first;
    EXPECT_EQ(ToJson(reloaded), first);
}

TEST(GraphSerializationTest, MissingPortOnLinkFailsValidate)
{
    Model model;
    Node source;
    source.Id = "a";
    source.Ports.push_back({"out", PortDirection::Out, "float", "Out"});
    Node target;
    target.Id = "b";
    target.Ports.push_back({"in", PortDirection::In, "float", "In"});
    model.Nodes.push_back(std::move(source));
    model.Nodes.push_back(std::move(target));
    GameEngine::Graph::Edge link;
    link.Id = "l0";
    link.SourceNodeId = "a";
    link.SourcePortId = "missing";
    link.TargetNodeId = "b";
    link.TargetPortId = "in";
    model.Links.push_back(std::move(link));
    EXPECT_FALSE(model.Validate());
}
