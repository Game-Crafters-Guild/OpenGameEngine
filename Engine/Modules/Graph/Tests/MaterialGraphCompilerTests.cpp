#include "Graph/MaterialGraphCompiler.h"
#include "Graph/GameLogicActionCatalog.h"
#include "Graph/GameLogicGraphCompiler.h"
#include "Graph/GraphGlslAuthoring.h"
#include "Graph/GraphModel.h"
#include "Graph/SgGraphModelBridge.h"
#include "Graph/ShaderGraphTemplate.h"

#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <unordered_set>
#include "StagedTestPaths.h"

using namespace GameEngine;

TEST(GameLogicActionCatalogTest, IncludesPrecompiledEngineActionCategories)
{
    const auto& catalog = GetGameLogicActionCatalog();
    EXPECT_NE(FindGameLogicActionSpec("PlaySound"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("SetPosition"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("AddForce"), nullptr);
    EXPECT_EQ(FindGameLogicActionSpec("PlayAnimation"), nullptr);
    EXPECT_EQ(FindGameLogicActionSpec("SetAnimatorBool"), nullptr);
    EXPECT_EQ(FindGameLogicActionSpec("SetAnimatorFloat"), nullptr);
    EXPECT_EQ(FindGameLogicActionSpec("SetAnimatorTrigger"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("ShowPanel"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("CollisionEvent"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("TriggerEvent"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("FloatOperator"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("BoolOperator"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("MakeVector3"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("Vector3Distance"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("SetTimeScale"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("QuitApplication"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("SetSkyTimeOfDay"), nullptr);
    EXPECT_NE(FindGameLogicActionSpec("ThirdPersonCameraFollow"), nullptr);

    ASSERT_FALSE(catalog.empty());
    for (const GameLogicActionSpec& action : catalog)
        EXPECT_TRUE(action.Precompiled) << action.TypeId;
}

TEST(GameLogicGraphCompilerTest, CompilesFlowToNativeCppActionCalls)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdGameLogic);

    Graph::Node entry{};
    entry.Id = "entry";
    entry.TypeId = "Entry";
    entry.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    Graph::Node sound{};
    sound.Id = "sound";
    sound.TypeId = "PlaySound";
    sound.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    sound.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    sound.Parameters["clipGuid"] = "sound-guid";
    sound.Parameters["volume"] = "0.75";
    sound.Parameters["pitch"] = "1";
    sound.Parameters["loop"] = "false";
    sound.Parameters["spatialized"] = "true";
    sound.Parameters["position"] = "1, 2, 3";
    sound.Parameters["worldId"] = "0";
    sound.Parameters["bus"] = "2";

    Graph::Node log{};
    log.Id = "log";
    log.TypeId = "Log";
    log.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    log.Parameters["message"] = "done";

    Graph::Node sky{};
    sky.Id = "sky";
    sky.TypeId = "SetSkyTimeOfDay";
    sky.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    sky.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    sky.Parameters["hours"] = "18.5";
    sky.Parameters["animate"] = "true";
    sky.Parameters["cycleSeconds"] = "240";

    Graph::Node camera{};
    camera.Id = "camera";
    camera.TypeId = "ThirdPersonCameraFollow";
    camera.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    camera.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    camera.Parameters["camera"] = "Main Camera";
    camera.Parameters["target"] = "self";
    camera.Parameters["offset"] = "0, 2.6, -7";
    camera.Parameters["lookOffset"] = "0, 1.25, 0";
    camera.Parameters["positionSmoothing"] = "8";
    camera.Parameters["rotationSmoothing"] = "12";

    model.Nodes = {entry, sound, sky, camera, log};
    model.Links.push_back({"l0", "entry", "out", "sound", "in"});
    model.Links.push_back({"l1", "sound", "out", "sky", "in"});
    model.Links.push_back({"l2", "sky", "out", "camera", "in"});
    model.Links.push_back({"l3", "camera", "out", "log", "in"});

    const auto result = GameLogicGraphCompiler::CompileToCpp(model, "RunGraph");
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_NE(result.cppSource.find("void RunGraph(GameEngine::GameLogicRuntimeContext& ctx)"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.PlaySound(\"sound-guid\""), std::string::npos);
    EXPECT_NE(result.cppSource.find("GameLogicVec3{1.000000f, 2.000000f, 3.000000f}"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.SetSkyTimeOfDay(18.5f, true, 240.0f)"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.ThirdPersonCameraFollow(ctx.FindEntityByName(\"Main Camera\"), ctx.Self(), GameLogicVec3{0.000000f, 2.600000f, -7.000000f}, GameLogicVec3{0.000000f, 1.250000f, 0.000000f}, 8.0f, 12.0f)"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.Log(\"done\")"), std::string::npos);
}

TEST(GameLogicGraphCompilerTest, SequenceEmitsAllNumberedOutputs)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdGameLogic);

    Graph::Node entry{};
    entry.Id = "entry";
    entry.TypeId = "Entry";
    entry.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    Graph::Node sequence{};
    sequence.Id = "sequence";
    sequence.TypeId = "Sequence";
    sequence.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    sequence.Ports.push_back({"out0", Graph::PortDirection::Out, "flow", "Out 1"});
    sequence.Ports.push_back({"out1", Graph::PortDirection::Out, "flow", "Out 2"});
    sequence.Ports.push_back({"out2", Graph::PortDirection::Out, "flow", "Out 3"});
    sequence.Ports.push_back({"out3", Graph::PortDirection::Out, "flow", "Out 4"});
    sequence.Ports.push_back({"out4", Graph::PortDirection::Out, "flow", "Out 5"});

    Graph::Node log{};
    log.Id = "log";
    log.TypeId = "Log";
    log.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    log.Parameters["message"] = "fifth";

    model.Nodes = {entry, sequence, log};
    model.Links.push_back({"l0", "entry", "out", "sequence", "in"});
    model.Links.push_back({"l1", "sequence", "out4", "log", "in"});

    const auto result = GameLogicGraphCompiler::CompileToCpp(model, "RunSequenceGraph");
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_NE(result.cppSource.find("ctx.Log(\"fifth\")"), std::string::npos);
}

TEST(GameLogicGraphCompilerTest, CompilesMathAndVectorValueNodesToNativeCpp)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdGameLogic);

    Graph::Node entry{};
    entry.Id = "entry";
    entry.TypeId = "Entry";

    Graph::Node volume{};
    volume.Id = "volume";
    volume.TypeId = "FloatOperator";
    volume.Parameters["a"] = "0.25";
    volume.Parameters["b"] = "0.5";
    volume.Parameters["operation"] = "add";

    Graph::Node position{};
    position.Id = "position";
    position.TypeId = "MakeVector3";
    position.Parameters["x"] = "1";
    position.Parameters["y"] = "2";
    position.Parameters["z"] = "3";

    Graph::Node distance{};
    distance.Id = "distance";
    distance.TypeId = "Vector3Distance";
    distance.Parameters["a"] = "0, 0, 0";
    distance.Parameters["b"] = "0, 3, 4";

    Graph::Node compare{};
    compare.Id = "compare";
    compare.TypeId = "CompareFloat";
    compare.Parameters["b"] = "5";
    compare.Parameters["comparison"] = "greater_or_equal";

    Graph::Node sound{};
    sound.Id = "sound";
    sound.TypeId = "PlaySound";
    sound.Parameters["clipGuid"] = "math-sound";
    sound.Parameters["pitch"] = "1";
    sound.Parameters["loop"] = "false";
    sound.Parameters["spatialized"] = "true";

    Graph::Node finish{};
    finish.Id = "finish";
    finish.TypeId = "Finish";

    model.Nodes = {entry, volume, position, distance, compare, sound, finish};
    model.Links.push_back({"l0", "entry", "out", "compare", "in"});
    model.Links.push_back({"l1", "distance", "result", "compare", "a"});
    model.Links.push_back({"l2", "compare", "true", "sound", "in"});
    model.Links.push_back({"l3", "volume", "result", "sound", "volume"});
    model.Links.push_back({"l4", "position", "vector", "sound", "position"});
    model.Links.push_back({"l5", "sound", "out", "finish", "in"});

    const auto result = GameLogicGraphCompiler::CompileToCpp(model, "RunMathGraph");
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_NE(result.cppSource.find("ctx.Vector3Distance(GameLogicVec3{0.000000f, 0.000000f, 0.000000f}, GameLogicVec3{0.000000f, 3.000000f, 4.000000f})"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.FloatOperator(0.25f, 0.5f, \"add\")"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.MakeVector3(1.0f, 2.0f, 3.0f)"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.PlaySound(\"math-sound\""), std::string::npos);
    EXPECT_NE(result.cppSource.find("return;"), std::string::npos);
}

TEST(GameLogicGraphCompilerTest, CompilesBirdControllerTestGraphAsset)
{
    const std::filesystem::path repoRoot = GameEngine::TestPaths::StagedRoot();
    const std::filesystem::path graphPath = repoRoot / "Assets" / "Graphs" / "BirdControllerTest.graph";
    std::ifstream in(graphPath);
    ASSERT_TRUE(in.is_open()) << graphPath;

    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Graph::Model model{};
    ASSERT_TRUE(Graph::FromJson(text, model));
    ASSERT_TRUE(model.Validate());

    const auto result = GameLogicGraphCompiler::CompileToCpp(model, "RunBirdControllerTest");
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_NE(result.cppSource.find("ctx.GetInputAction(\"Bird.Flap\")"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.PlaySound(\"Assets/Audio/Bird/WingFlap.wav\""), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.CollisionEvent(ctx.Self(), \"enter\", \"Hazard\")"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.TriggerEvent(ctx.Self(), \"enter\", \"Perch\")"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.SetAnimatorTrigger(ctx.Self(), \"Flap\")"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.PlayAnimation(ctx.Self(), \"Assets/Animations/Bird_Dive.anim\", false)"), std::string::npos);
    EXPECT_EQ(result.cppSource.find("GameGraphActions::PlayAnimation"), std::string::npos);
    EXPECT_EQ(result.cppSource.find("GameGraphActions::SetAnimatorTrigger"), std::string::npos);
    EXPECT_EQ(result.cppSource.find("GameGraphActions::SetAnimatorBool"), std::string::npos);
    EXPECT_EQ(result.cppSource.find("GameGraphActions::SetAnimatorFloat"), std::string::npos);
}

TEST(GameLogicGraphCompilerTest, CompilesRetiredAnimatorActionsWithoutCatalogOrCustomDecls)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdGameLogic);

    Graph::Node entry{};
    entry.Id = "entry";
    entry.TypeId = "Entry";
    entry.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    Graph::Node play{};
    play.Id = "play";
    play.TypeId = "PlayAnimation";
    play.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    play.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    play.Parameters["entity"] = "self";
    play.Parameters["clipGuid"] = "clip-guid";
    play.Parameters["loop"] = "true";

    Graph::Node trig{};
    trig.Id = "trig";
    trig.TypeId = "SetAnimatorTrigger";
    trig.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    trig.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    trig.Parameters["entity"] = "self";
    trig.Parameters["name"] = "Jump";

    model.Nodes = {entry, play, trig};
    model.Links.push_back({"l0", "entry", "out", "play", "in"});
    model.Links.push_back({"l1", "play", "out", "trig", "in"});

    const auto result = GameLogicGraphCompiler::CompileToCpp(model, "RunRetiredAnim");
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_NE(result.cppSource.find("ctx.PlayAnimation(ctx.Self(), \"clip-guid\", true)"), std::string::npos);
    EXPECT_NE(result.cppSource.find("ctx.SetAnimatorTrigger(ctx.Self(), \"Jump\")"), std::string::npos);
    EXPECT_EQ(result.cppSource.find("namespace GameEngine::GameGraphActions"), std::string::npos);
}

static Graph::Model MakeSimpleColorGraph()
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node color{};
    color.Id = "node_color";
    color.TypeId = "ColorConstant";
    color.Parameters["r"] = "1.0";
    color.Parameters["g"] = "0.0";
    color.Parameters["b"] = "0.0";
    color.Ports.push_back({"value", Graph::PortDirection::Out, "float3", "Color"});

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports.push_back({"BaseColor", Graph::PortDirection::In, "float3", "Base Color"});
    output.Ports.push_back({"Metallic", Graph::PortDirection::In, "float", "Metallic"});
    output.Ports.push_back({"Roughness", Graph::PortDirection::In, "float", "Roughness"});

    model.Nodes = {color, output};
    model.Links.push_back(
        {"link_0", "node_color", "value", "node_output", "BaseColor"});

    return model;
}

TEST(MaterialGraphCompilerTest, RejectsGameLogicGraph)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdGameLogic);
    const auto result = MaterialGraphCompiler::Compile(model);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
}

TEST(MaterialGraphCompilerTest, RequiresOutputNode)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);
    Graph::Node color{};
    color.Id = "node_color";
    color.TypeId = "ColorConstant";
    model.Nodes = {color};

    const auto result = MaterialGraphCompiler::Compile(model);
    EXPECT_FALSE(result.success);
}

TEST(MaterialGraphCompilerTest, CompilesColorToEvaluateSurface)
{
    const Graph::Model model = MakeSimpleColorGraph();
    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success) << result.errors.front().Message;
    EXPECT_NE(result.glslSource.find("EvaluateSurface"), std::string::npos);
    EXPECT_NE(result.glslSource.find("SG_ColorConstant"), std::string::npos);
    EXPECT_NE(result.glslSource.find("SG_ColorConstant"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, PreviewDocumentUsesParameterNodeSlotForGenericVariableNames)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Variable tint{};
    tint.Name = "uTint";
    tint.Type = "float3";
    tint.Value = "1, 0.5, 0.2";
    model.Variables.push_back(tint);

    Graph::Node tintParam{};
    tintParam.Id = "node_tint";
    tintParam.TypeId = "ColorParameter";
    tintParam.Parameters["variableName"] = "uTint";
    tintParam.Parameters["slot"] = "0";

    model.Nodes.push_back(tintParam);

    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Preview");
    ApplyMaterialGraphVariablesToPreviewDocument(model, doc);

    const auto* baseColor = std::get_if<std::vector<float>>(&doc.properties["baseColor"]);
    ASSERT_NE(baseColor, nullptr);
    ASSERT_GE(baseColor->size(), 3u);
    EXPECT_FLOAT_EQ((*baseColor)[0], 1.0f);
    EXPECT_FLOAT_EQ((*baseColor)[1], 0.5f);
    EXPECT_FLOAT_EQ((*baseColor)[2], 0.2f);
}

TEST(MaterialGraphCompilerTest, PreviewDocumentMapsRimColorVariableToParams2WithoutExplicitSlot)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Variable rim{};
    rim.Name = "uRimColor";
    rim.Type = "float3";
    rim.Value = "0, 1, 0";
    rim.IsPublic = true;
    model.Variables.push_back(rim);

    Graph::Node rimParam{};
    rimParam.Id = "node_rim_color";
    rimParam.TypeId = "ColorParameter";
    rimParam.Parameters["variableName"] = "uRimColor";

    model.Nodes.push_back(rimParam);

    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Preview");
    ApplyMaterialGraphVariablesToPreviewDocument(model, doc);

    EXPECT_FLOAT_EQ(std::get<float>(doc.properties["flipbookColumns"]), 0.0f);
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties["flipbookRows"]), 1.0f);
    EXPECT_FLOAT_EQ(std::get<float>(doc.properties["flipbookFps"]), 0.0f);
    ASSERT_EQ(doc.shaderGraphPublicProperties.size(), 1u);
    EXPECT_EQ(doc.shaderGraphPublicProperties[0].GraphName, "uRimColor");
    EXPECT_EQ(doc.shaderGraphPublicProperties[0].Type, "vec3");
    ASSERT_EQ(doc.shaderGraphPublicProperties[0].MaterialPropertyKeys.size(), 3u);
    EXPECT_EQ(doc.shaderGraphPublicProperties[0].MaterialPropertyKeys[0], "flipbookColumns");
    EXPECT_EQ(doc.shaderGraphPublicProperties[0].MaterialPropertyKeys[1], "flipbookRows");
    EXPECT_EQ(doc.shaderGraphPublicProperties[0].MaterialPropertyKeys[2], "flipbookFps");
}

TEST(MaterialGraphCompilerTest, FresnelNodeGeneratesPowExpression)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node fresnel{};
    fresnel.Id = "node_fresnel";
    fresnel.TypeId = "Fresnel";
    fresnel.Parameters["power"] = "3.0";
    fresnel.Ports.push_back({"normal", Graph::PortDirection::In, "float3", "Normal"});
    fresnel.Ports.push_back({"view", Graph::PortDirection::In, "float3", "View"});
    fresnel.Ports.push_back({"out", Graph::PortDirection::Out, "float", "Out"});

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"}};

    model.Nodes = {fresnel, output};
    model.Links.push_back({"link_0", "node_fresnel", "out", "node_output", "Metallic"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_Fresnel"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, LerpNodeGeneratesMix)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node a{};
    a.Id = "node_a";
    a.TypeId = "FloatConstant";
    a.Parameters["value"] = "0.0";
    a.Ports.push_back({"value", Graph::PortDirection::Out, "float", "Value"});

    Graph::Node b{};
    b.Id = "node_b";
    b.TypeId = "FloatConstant";
    b.Parameters["value"] = "1.0";
    b.Ports.push_back({"value", Graph::PortDirection::Out, "float", "Value"});

    Graph::Node lerp{};
    lerp.Id = "node_lerp";
    lerp.TypeId = "Lerp";
    lerp.Ports = {{"a", Graph::PortDirection::In, "float", "A"},
                 {"b", Graph::PortDirection::In, "float", "B"},
                 {"weight", Graph::PortDirection::In, "float", "Weight"},
                 {"out", Graph::PortDirection::Out, "float", "Out"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"}};

    model.Nodes = {a, b, lerp, output};
    model.Links = {
        {"link_0", "node_a", "value", "node_lerp", "a"},
        {"link_1", "node_b", "value", "node_lerp", "b"},
        {"link_2", "node_lerp", "out", "node_output", "Metallic"},
    };

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_Lerp"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, SmoothStepNodeGeneratesSmoothstep)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node step{};
    step.Id = "node_step";
    step.TypeId = "SmoothStep";
    step.Ports = {{"edge0", Graph::PortDirection::In, "float", "Edge0"},
                 {"edge1", Graph::PortDirection::In, "float", "Edge1"},
                 {"value", Graph::PortDirection::In, "float", "Value"},
                 {"out", Graph::PortDirection::Out, "float", "Out"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"},
                   {"Normal", Graph::PortDirection::In, "float3", "Normal (TS)"}};

    model.Nodes = {step, output};
    model.Links.push_back({"link_0", "node_step", "out", "node_output", "Metallic"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_SmoothStep"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, VectorComposeBuildsVec3)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node compose{};
    compose.Id = "node_compose";
    compose.TypeId = "VectorCompose";
    compose.Ports = {{"x", Graph::PortDirection::In, "float", "X"},
                    {"y", Graph::PortDirection::In, "float", "Y"},
                    {"z", Graph::PortDirection::In, "float", "Z"},
                    {"out", Graph::PortDirection::Out, "float3", "Out"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"},
                   {"Normal", Graph::PortDirection::In, "float3", "Normal (TS)"}};

    model.Nodes = {compose, output};
    model.Links.push_back({"link_0", "node_compose", "out", "node_output", "Emissive"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_VectorCompose"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, RGB2HSVEmitsHelperFunction)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node rgb2hsv{};
    rgb2hsv.Id = "node_rgb2hsv";
    rgb2hsv.TypeId = "RGB2HSV";
    rgb2hsv.Ports = {{"color", Graph::PortDirection::In, "float3", "Color"},
                    {"out", Graph::PortDirection::Out, "float3", "Out"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"},
                   {"Normal", Graph::PortDirection::In, "float3", "Normal (TS)"}};

    model.Nodes = {rgb2hsv, output};
    model.Links.push_back({"link_0", "node_rgb2hsv", "out", "node_output", "BaseColor"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_RGB2HSV"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, TriplanarTextureEmitsHelperFunction)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node triplanar{};
    triplanar.Id = "node_triplanar";
    triplanar.TypeId = "TriplanarTexture";
    triplanar.Ports = {{"position", Graph::PortDirection::In, "float3", "Position"},
                      {"normal", Graph::PortDirection::In, "float3", "Normal"},
                      {"scale", Graph::PortDirection::In, "float", "Scale"},
                      {"sharpness", Graph::PortDirection::In, "float", "Sharpness"},
                      {"color", Graph::PortDirection::Out, "float4", "Color"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"},
                   {"Normal", Graph::PortDirection::In, "float3", "Normal (TS)"}};

    model.Nodes = {triplanar, output};
    model.Links.push_back({"link_0", "node_triplanar", "color", "node_output", "BaseColor"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_TriplanarSample"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, RotateByAxisEmitsHelperFunction)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node rotate{};
    rotate.Id = "node_rotate";
    rotate.TypeId = "RotateByAxis";
    rotate.Ports = {{"vector", Graph::PortDirection::In, "float3", "Vector"},
                   {"axis", Graph::PortDirection::In, "float3", "Axis"},
                   {"angle", Graph::PortDirection::In, "float", "Angle"},
                   {"out", Graph::PortDirection::Out, "float3", "Out"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"},
                   {"Normal", Graph::PortDirection::In, "float3", "Normal (TS)"}};

    model.Nodes = {rotate, output};
    model.Links.push_back({"link_0", "node_rotate", "out", "node_output", "Normal"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_RotateByAxis"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, RuntimeParameterReadsMaterialParamSlot)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node param{};
    param.Id = "node_param";
    param.TypeId = "FloatParameter";
    param.Parameters["slot"] = "2";
    param.Parameters["component"] = "z";
    param.Ports = {{"value", Graph::PortDirection::Out, "float", "Value"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"},
                   {"Emissive", Graph::PortDirection::In, "float3", "Emissive"},
                   {"Normal", Graph::PortDirection::In, "float3", "Normal (TS)"}};

    model.Nodes = {param, output};
    model.Links.push_back({"link_0", "node_param", "value", "node_output", "Metallic"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("Mat.uParams2.z"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, TextureArrayEmitsMaterialSamplerBinding)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node arraySample{};
    arraySample.Id = "node_array";
    arraySample.TypeId = "SampleTextureArray";
    arraySample.Ports = {{"uv", Graph::PortDirection::In, "float2", "UV"},
                        {"layer", Graph::PortDirection::In, "float", "Layer"},
                        {"color", Graph::PortDirection::Out, "float4", "Color"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"}};

    model.Nodes = {arraySample, output};
    model.Links.push_back({"link_0", "node_array", "color", "node_output", "BaseColor"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_SampleTextureArray"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, CubemapEmitsMaterialSamplerBinding)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node cubeSample{};
    cubeSample.Id = "node_cube";
    cubeSample.TypeId = "SampleCubemap";
    cubeSample.Parameters["texture"] = "skyCube";
    cubeSample.Ports = {{"direction", Graph::PortDirection::In, "float3", "Direction"},
                       {"color", Graph::PortDirection::Out, "float4", "Color"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"}};

    model.Nodes = {cubeSample, output};
    model.Links.push_back({"link_0", "node_cube", "color", "node_output", "BaseColor"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.glslSource.find("SG_SampleCubemap"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, BillboardEmitsVertexModifier)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node billboard{};
    billboard.Id = "node_billboard";
    billboard.TypeId = "Billboard";
    billboard.Ports = {{"position", Graph::PortDirection::In, "float3", "Position"},
                      {"out", Graph::PortDirection::Out, "float3", "Out"}};

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports = {{"BaseColor", Graph::PortDirection::In, "float3", "Base Color"},
                   {"Metallic", Graph::PortDirection::In, "float", "Metallic"},
                   {"Roughness", Graph::PortDirection::In, "float", "Roughness"}};

    Graph::Node vertexOutput{};
    vertexOutput.Id = "node_vertex_output";
    vertexOutput.TypeId = "VertexOutput";
    vertexOutput.Ports = {
        {"PositionOffset", Graph::PortDirection::In, "float3", "Position Offset"}};

    model.Nodes = {billboard, output, vertexOutput};
    model.Links.push_back(
        {"link_0", "node_billboard", "out", "node_vertex_output", "PositionOffset"});

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.vertexModifierSource.empty());
    EXPECT_NE(result.vertexModifierSource.find("vec3 ModifyVertex(vec3 position, InstanceData inst)"), std::string::npos);
    EXPECT_NE(result.vertexModifierSource.find("SG_Billboard(position, inst)"), std::string::npos);
}

TEST(SgGraphModelBridgeTest, LoadedShaderGraphGlslCompilesAfterRoundTrip)
{
    const std::filesystem::path repoRoot = GameEngine::TestPaths::StagedRoot();
    const std::filesystem::path graphPath = repoRoot / "Assets" / "Materials" / "Graph" / "GraphSampleRed.glsl";
    if (!std::filesystem::exists(graphPath))
        GTEST_SKIP() << "GraphSampleRed.glsl not present";

    const auto file = ShaderGraph::LoadGraphFile(graphPath);
    const Graph::Model model = SgDocumentToGraphModel(file.Document);
    ASSERT_FALSE(model.Nodes.empty());

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front().Message);
    EXPECT_FALSE(result.glslSource.empty());
    EXPECT_NE(result.glslSource.find("EvaluateSurface"), std::string::npos);
}

TEST(SgGraphModelBridgeTest, RoundTripsGraphVariablesThroughSgProperties)
{
    Graph::Model model;
    model.KindId.assign(Graph::kKindIdMaterial);
    Graph::Variable tiling;
    tiling.Name = "uTiling";
    tiling.Type = "float";
    tiling.Value = "0.25";
    tiling.IsPublic = true;
    tiling.CreatedOrder = 0;
    Graph::Variable tint;
    tint.Name = "uTint";
    tint.Type = "float3";
    tint.Value = "1, 0.5, 0.2";
    tint.CreatedOrder = 1;
    model.Variables = {tiling, tint};

    const auto doc = GraphModelToSgDocument(model, "VariableRoundTrip");
    ASSERT_EQ(doc.Properties.size(), 2u);
    EXPECT_EQ(doc.Properties[0].Name, "uTiling");
    EXPECT_EQ(doc.Properties[0].Type, "float");
    EXPECT_EQ(doc.Properties[0].DefaultValue, "0.25");
    EXPECT_TRUE(doc.Properties[0].IsPublic);
    EXPECT_EQ(doc.Properties[1].Name, "uTint");
    EXPECT_EQ(doc.Properties[1].Type, "vec3");

    const Graph::Model restored = SgDocumentToGraphModel(doc);
    ASSERT_EQ(restored.Variables.size(), 2u);
    EXPECT_EQ(restored.Variables[0].Name, "uTiling");
    EXPECT_EQ(restored.Variables[0].Type, "float");
    EXPECT_EQ(restored.Variables[0].Value, "0.25");
    EXPECT_TRUE(restored.Variables[0].IsPublic);
    EXPECT_EQ(restored.Variables[1].Name, "uTint");
    EXPECT_EQ(restored.Variables[1].Type, "float3");
    EXPECT_EQ(restored.Variables[1].Value, "1, 0.5, 0.2");
}

TEST(SgGraphModelBridgeTest, LoadsPreviewSphereTestPropertiesIntoVariables)
{
    const std::filesystem::path repoRoot = GameEngine::TestPaths::StagedRoot();
    const std::filesystem::path graphPath =
        repoRoot / "Assets" / "Materials" / "Graph" / "PreviewSphereTest.glsl";
    if (!std::filesystem::exists(graphPath))
        GTEST_SKIP() << "PreviewSphereTest.glsl not present";

    const auto file = ShaderGraph::LoadGraphFile(graphPath);
    const Graph::Model model = SgDocumentToGraphModel(file.Document);
    ASSERT_EQ(model.Variables.size(), 4u);
    EXPECT_TRUE(model.Variables[0].IsPublic);

    std::unordered_set<std::string> names;
    for (const Graph::Variable& variable : model.Variables)
        names.insert(variable.Name);
    EXPECT_TRUE(names.count("uBaseColor"));
    EXPECT_TRUE(names.count("uMetallic"));
    EXPECT_TRUE(names.count("uRoughness"));
    EXPECT_TRUE(names.count("uRimColor"));
}

TEST(SgGraphModelBridgeTest, ExpandsEngineInputNodesOnLoad)
{
    const std::filesystem::path repoRoot = GameEngine::TestPaths::StagedRoot();
    const std::filesystem::path graphPath =
        repoRoot / "Assets" / "Materials" / "Graph" / "PreviewSphereTest.glsl";
    if (!std::filesystem::exists(graphPath))
        GTEST_SKIP() << "PreviewSphereTest.glsl not present";

    const auto file = ShaderGraph::LoadGraphFile(graphPath);
    Graph::Model model = SgDocumentToGraphModel(file.Document);

    bool hasEngineInput = false;
    bool hasNormalVector = false;
    bool hasViewDirection = false;
    for (const Graph::Node& node : model.Nodes)
    {
        if (node.TypeId == "EngineInput")
            hasEngineInput = true;
        if (node.TypeId == "NormalVector")
            hasNormalVector = true;
        if (node.TypeId == "ViewDirection")
            hasViewDirection = true;
    }

    EXPECT_FALSE(hasEngineInput);
    EXPECT_TRUE(hasNormalVector);
    EXPECT_TRUE(hasViewDirection);

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front().Message);
}

TEST(SgGraphModelBridgeTest, LoadsVariablesTestGraphPropertiesIntoVariables)
{
    const std::filesystem::path repoRoot = GameEngine::TestPaths::StagedRoot();
    const std::filesystem::path graphPath = repoRoot / "Assets" / "Materials" / "Graph" / "VariablesTest.glsl";
    if (!std::filesystem::exists(graphPath))
        GTEST_SKIP() << "VariablesTest.glsl not present";

    const auto file = ShaderGraph::LoadGraphFile(graphPath);
    const Graph::Model model = SgDocumentToGraphModel(file.Document);
    ASSERT_EQ(model.Variables.size(), 2u);

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front().Message);
    EXPECT_NE(result.glslSource.find("EvaluateSurface"), std::string::npos);
}

// ---- Minimal walkthrough examples: smallest authoring JSON that parses and
// ---- compiles as a material graph, and one mutation that fails with a real
// ---- error message.

namespace {
const char* kMinimalGraphJson = R"JSON({
  "kind": "material",
  "nodes": [
    {
      "id": "node_color",
      "typeId": "ColorConstant",
      "parameters": { "r": "1.0", "g": "0.0", "b": "0.0" },
      "pins": [
        { "id": "value", "direction": "out", "dataType": "float3", "displayName": "Color" }
      ]
    },
    {
      "id": "node_output",
      "typeId": "SurfaceOutput",
      "pins": [
        { "id": "BaseColor", "direction": "in", "dataType": "float3", "displayName": "Base Color" }
      ]
    }
  ],
  "links": [
    {
      "id": "l0",
      "sourceNodeId": "node_color",
      "sourcePinId": "value",
      "targetNodeId": "node_output",
      "targetPinId": "BaseColor"
    }
  ]
})JSON";
}

TEST(MaterialGraphCompilerTest, MinimalExample_GraphJsonParsesAndCompiles)
{
    Graph::Model model{};
    ASSERT_TRUE(Graph::FromJson(kMinimalGraphJson, model));
    EXPECT_EQ(model.KindId, Graph::kKindIdMaterial);
    ASSERT_EQ(model.Nodes.size(), 2u);
    ASSERT_EQ(model.Links.size(), 1u);

    const auto result = MaterialGraphCompiler::Compile(model);
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front().Message);
    EXPECT_NE(result.glslSource.find("EvaluateSurface"), std::string::npos);
    std::cout << "[minimal graph JSON] compiled glsl:\n" << result.glslSource << "\n";
}

TEST(MaterialGraphCompilerTest, MinimalExample_WrongKind_FailsWithRealError)
{
    std::string mutated = kMinimalGraphJson;
    const std::string from = "\"kind\": \"material\"";
    const size_t pos = mutated.find(from);
    ASSERT_NE(pos, std::string::npos);
    mutated.replace(pos, from.size(), "\"kind\": \"game_logic\"");

    Graph::Model model{};
    ASSERT_TRUE(Graph::FromJson(mutated, model));
    const auto result = MaterialGraphCompiler::Compile(model);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    std::cout << "[mutated graph JSON] compiler error: " << result.errors.front().Message << "\n";
    EXPECT_NE(result.errors.front().Message.find("kind must be 'material'"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, MinimalExample_DanglingLink_FailsValidation)
{
    std::string mutated = kMinimalGraphJson;
    const std::string from = "\"targetNodeId\": \"node_output\"";
    const size_t pos = mutated.find(from);
    ASSERT_NE(pos, std::string::npos);
    mutated.replace(pos, from.size(), "\"targetNodeId\": \"missing_node\"");

    Graph::Model model{};
    EXPECT_FALSE(Graph::FromJson(mutated, model))
        << "a link to a nonexistent node must fail Graph::Model::Validate()";
}

TEST(MaterialGraphCompilerTest, BridgeAndJsonRoundTripsCarryVariantDefines)
{
    // The editor edits a shader graph as a Graph::Model and saves it back through
    // the bridge, so a define the model cannot hold is lost on the first save.
    ShaderGraph::SgGraphDocument doc;
    doc.GraphName = "VariantCarry";
    doc.LightingModel = "StandardPBR";
    doc.VariantDefines = {"GE_USER_SNOW", "GE_USER_WETNESS"};

    const Graph::Model model = SgDocumentToGraphModel(doc);
    ASSERT_EQ(model.VariantDefines.size(), 2u);

    Graph::Model reloaded{};
    ASSERT_TRUE(Graph::FromJson(Graph::ToJson(model), reloaded));
    EXPECT_EQ(reloaded.VariantDefines, model.VariantDefines);

    const ShaderGraph::SgGraphDocument back = GraphModelToSgDocument(reloaded, "VariantCarry");
    EXPECT_EQ(back.VariantDefines, doc.VariantDefines);
}

TEST(MaterialGraphCompilerTest, CompileFromFileUsesJsonFenceNotTags)
{
    const std::string tags = "// @sg-graph name=FenceOnly\n";
    const std::string fenced = Graph::AppendAuthoringJsonFence(tags, kMinimalGraphJson);
    const auto path = std::filesystem::temp_directory_path() / "ge-graph-fence-compile.glsl";
    {
        std::ofstream out(path);
        ASSERT_TRUE(static_cast<bool>(out));
        out << fenced;
    }
    const auto result = MaterialGraphCompiler::CompileFromFile(path);
    std::filesystem::remove(path);
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front().Message);
    EXPECT_NE(result.glslSource.find("EvaluateSurface"), std::string::npos);
}

TEST(MaterialGraphCompilerTest, CompileFromFileInvalidFenceFails)
{
    const char* broken =
        "// @sg-graph name=BrokenFence\n"
        "// GE-GRAPH-JSON-BEGIN\n"
        "// {not-json\n"
        "// GE-GRAPH-JSON-END\n";
    const auto path = std::filesystem::temp_directory_path() / "ge-graph-fence-invalid.glsl";
    {
        std::ofstream out(path);
        ASSERT_TRUE(static_cast<bool>(out));
        out << broken;
    }
    const auto result = MaterialGraphCompiler::CompileFromFile(path);
    std::filesystem::remove(path);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors.front().Message.find("invalid graph JSON fence"), std::string::npos);
}

TEST(ShaderGraphTemplateTest, NewGraphCarriesTheMasterNodeAndCompiles)
{
    const std::string source = Graph::MakeShaderGraphTemplateSource("TemplateProbe");
    EXPECT_NE(source.find("@sg-graph     TemplateProbe"), std::string::npos);
    EXPECT_NE(source.find(std::string(Graph::kGlslJsonFenceBegin)), std::string::npos);
    EXPECT_NE(source.find("SurfaceOutput EvaluateSurface(SurfaceInput sIn)"), std::string::npos);

    Graph::Model model{};
    ASSERT_TRUE(Graph::LoadModelFromShaderGraphComments(source, model));
    EXPECT_EQ(model.KindId, Graph::kKindIdMaterial);
    ASSERT_EQ(model.Nodes.size(), 1u);
    EXPECT_EQ(model.Nodes.front().TypeId, "SurfaceOutput");

    const auto result = MaterialGraphCompiler::Compile(model);
    EXPECT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front().Message);

    // Printed like the minimal-walkthrough examples above: the exact bytes
    // Create → Shader Graph writes, readable without launching an editor.
    std::cout << "[new shader graph template]\n" << source << "\n";
}

TEST(ShaderGraphTemplateTest, CompanionMaterialPointsAtTheSiblingGraph)
{
    const MaterialDocument doc = Graph::MakeShaderGraphTemplateMaterial("TemplateProbe");
    EXPECT_EQ(doc.surfaceShader, "TemplateProbe.glsl");
    EXPECT_TRUE(doc.surfaceShaderGuid.empty());
    EXPECT_TRUE(doc.surfaceGraph.empty());
}

TEST(SgGraphModelBridgeTest, SurfaceOutputSurvivesTheModelRoundTripUnrenamed)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);
    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    model.Nodes.push_back(output);

    const Graph::Model roundTripped = SgDocumentToGraphModel(GraphModelToSgDocument(model, "RoundTrip"));
    ASSERT_EQ(roundTripped.Nodes.size(), 1u);
    EXPECT_EQ(roundTripped.Nodes.front().TypeId, "SurfaceOutput");
}

TEST(SgGraphModelBridgeTest, VertexOutputNodeSelectsTheBothStage)
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);
    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    Graph::Node vertexOutput{};
    vertexOutput.Id = "node_vertex_output";
    vertexOutput.TypeId = "VertexOutput";
    model.Nodes = {output, vertexOutput};

    const auto doc = GraphModelToSgDocument(model, "VertexStage");
    EXPECT_EQ(doc.Stage, ShaderGraph::SgGraphStage::Both);
    EXPECT_EQ(doc.Nodes.size(), 2u);
}

namespace {

/* Two constants into a SurfaceOutput: enough shape for the digest to have wiring,
   parameters and a property to be sensitive to. */
Graph::Model MakeDigestFixtureGraph()
{
    Graph::Model model;
    model.KindId = std::string(Graph::kKindIdMaterial);

    Graph::Node color;
    color.Id = "node_color";
    color.TypeId = "ColorConstant";
    color.PositionX = 100.f;
    color.PositionY = 100.f;
    color.Parameters["r"] = Graph::GraphValue(0.5);
    color.Ports.push_back(Graph::Port{"value", Graph::PortDirection::Out, "vec3", "Value", {}});
    model.Nodes.push_back(std::move(color));

    Graph::Node output;
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.PositionX = 400.f;
    output.PositionY = 100.f;
    output.Ports.push_back(
        Graph::Port{"BaseColor", Graph::PortDirection::In, "vec3", "Base Color", {}});
    model.Nodes.push_back(std::move(output));

    Graph::Edge link;
    link.Id = "link_0";
    link.SourceNodeId = "node_color";
    link.SourcePortId = "value";
    link.TargetNodeId = "node_output";
    link.TargetPortId = "BaseColor";
    model.Links.push_back(std::move(link));

    Graph::Variable tint;
    tint.Name = "Tint";
    tint.Type = "color";
    tint.Value = "1,1,1,1";
    tint.IsPublic = true;
    model.Variables.push_back(std::move(tint));
    return model;
}

} // namespace

// The digest exists to answer one question: can this edit change the GLSL? Every
// caller that skips work on an unmoved digest is only correct if layout cannot
// move it, so that is what these pin.
TEST(MaterialGraphSurfaceDigestTest, LayoutIsNotPartOfTheDigest)
{
    const Graph::Model base = MakeDigestFixtureGraph();
    const std::uint64_t digest = MaterialGraphSurfaceDigest(base);

    Graph::Model moved = base;
    moved.Nodes[0].PositionX += 640.f;
    moved.Nodes[0].PositionY -= 220.f;
    EXPECT_EQ(MaterialGraphSurfaceDigest(moved), digest) << "dragging a node cannot change a shader";

    Graph::Model panned = base;
    panned.Viewport.PanX = 512.f;
    panned.Viewport.Zoom = 2.5f;
    EXPECT_EQ(MaterialGraphSurfaceDigest(panned), digest) << "panning the canvas cannot change a shader";
}

TEST(MaterialGraphSurfaceDigestTest, PropertyValuesAreNotPartOfTheDigestButDeclarationsAre)
{
    const Graph::Model base = MakeDigestFixtureGraph();
    const std::uint64_t digest = MaterialGraphSurfaceDigest(base);

    // A property compiles to a uniform reference; its value is pushed to the
    // runtime without touching the shader.
    Graph::Model scrubbed = base;
    scrubbed.Variables[0].Value = "0.2,0.4,0.9,1";
    EXPECT_EQ(MaterialGraphSurfaceDigest(scrubbed), digest);

    Graph::Model renamed = base;
    renamed.Variables[0].Name = "Tint2";
    EXPECT_NE(MaterialGraphSurfaceDigest(renamed), digest) << "the body references the name";

    Graph::Model retyped = base;
    retyped.Variables[0].Type = "float";
    EXPECT_NE(MaterialGraphSurfaceDigest(retyped), digest) << "the type picks the emitted expression";
}

TEST(MaterialGraphSurfaceDigestTest, WiringAndNodeParametersMoveTheDigest)
{
    const Graph::Model base = MakeDigestFixtureGraph();
    const std::uint64_t digest = MaterialGraphSurfaceDigest(base);

    // The stale-preview class: a node constant is baked into the materialized
    // body, so a digest blind to it would skip the regeneration that edit needs.
    Graph::Model tweaked = base;
    tweaked.Nodes[0].Parameters["r"] = Graph::GraphValue(0.75);
    EXPECT_NE(MaterialGraphSurfaceDigest(tweaked), digest);

    Graph::Model unwired = base;
    unwired.Links.clear();
    EXPECT_NE(MaterialGraphSurfaceDigest(unwired), digest);

    Graph::Model retypedNode = base;
    retypedNode.Nodes[0].TypeId = "FloatConstant";
    EXPECT_NE(MaterialGraphSurfaceDigest(retypedNode), digest);
}
