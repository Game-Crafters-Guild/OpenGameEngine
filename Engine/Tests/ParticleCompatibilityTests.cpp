#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "GPUFogParticles/GPUFogParticlesMaterial.h"
#include "Particles/ParticleStackDocument.h"
#include "Particles/Processors/ParticleEmitRateProcessor.h"
#include "Particles/Processors/ParticlePropertyProcessor.h"
#include "Particles/Rendering/ParticleDrawSort.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Particles/Rendering/ParticleRenderData.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine;

TEST(ParticleCompatibility, RenderRecordPreservesShaderLaneOrder)
{
    Particles::ParticleRenderData record;
    record.PositionSize = {1, 2, 3, 4};
    record.VelocityAngle = {5, 6, 7, 8};
    record.Color = {9, 10, 11, 12};
    record.AxisX = {13, 14, 15, 16};
    record.AxisY = {17, 18, 19, 20};
    record.AxisZ = {21, 22, 23, 24};
    record.Animation = {25, 26, 27, 28};
    record.Metadata = {29, 30, 31, 32};
    std::array<float, 28> lanes{};
    std::memcpy(lanes.data(), &record, sizeof(lanes));
    for (size_t index = 0; index < lanes.size(); ++index)
        EXPECT_FLOAT_EQ(lanes[index], static_cast<float>(index + 1));
    std::array<uint32_t, 4> metadata{};
    std::memcpy(metadata.data(), reinterpret_cast<const unsigned char*>(&record) + sizeof(lanes), sizeof(metadata));
    EXPECT_EQ(metadata, (std::array<uint32_t, 4>{29, 30, 31, 32}));
}

TEST(ParticleCompatibility, RenderRecordDefaultsKeepIdentityBasisAndZeroPayload)
{
    const Particles::ParticleRenderData record;
    EXPECT_FLOAT_EQ(record.AxisX.x, 1.0f);
    EXPECT_FLOAT_EQ(record.AxisY.y, 1.0f);
    EXPECT_FLOAT_EQ(record.AxisZ.z, 1.0f);
    for (int lane = 0; lane < 4; ++lane)
    {
        EXPECT_FLOAT_EQ(record.PositionSize[lane], 0.0f);
        EXPECT_FLOAT_EQ(record.VelocityAngle[lane], 0.0f);
        EXPECT_FLOAT_EQ(record.Color[lane], 0.0f);
        EXPECT_FLOAT_EQ(record.Animation[lane], 0.0f);
        EXPECT_EQ(record.Metadata[lane], 0u);
        EXPECT_FLOAT_EQ(record.AxisX[lane], lane == 0 ? 1.0f : 0.0f);
        EXPECT_FLOAT_EQ(record.AxisY[lane], lane == 1 ? 1.0f : 0.0f);
        EXPECT_FLOAT_EQ(record.AxisZ[lane], lane == 2 ? 1.0f : 0.0f);
    }
}

TEST(ParticleCompatibility, CollisionEventBufferIsOptInAndBounded)
{
    Components::ParticleCollisionEventsBuffer buffer{};
    Components::ParticleCollisionEvent event{};
    event.Emitter = ECS::EntityHandle(42u);
    event.ParticleId = 7u;
    event.Position[1] = -4.0f;
    event.Normal[1] = 1.0f;
    event.Velocity[1] = -3.0f;
    event.Speed = 3.0f;

    EXPECT_TRUE(buffer.Push(event));
    ASSERT_EQ(buffer.Count, 1u);
    EXPECT_EQ(buffer.Events[0].Emitter.id, 42u);
    EXPECT_EQ(buffer.Events[0].ParticleId, 7u);
    EXPECT_FLOAT_EQ(buffer.Events[0].Position[1], -4.0f);

    buffer.Clear();
    EXPECT_EQ(buffer.Count, 0u);

    for (uint32 i = 0; i < Components::ParticleCollisionEventsBuffer::kMaxEvents; ++i)
        EXPECT_TRUE(buffer.Push(event));
    EXPECT_FALSE(buffer.Push(event));
    EXPECT_EQ(buffer.Count, Components::ParticleCollisionEventsBuffer::kMaxEvents);
}

// Without a material asset the particles draw the procedural smoke: the fog layers, untinted.
TEST(ParticleCompatibility, RenderMaterialWithoutASourceDrawsTheProceduralSmoke)
{
    const auto material = Particles::BuildParticleRenderMaterialDocument(Components::ParticleRenderer{}, nullptr);
    EXPECT_EQ(material.surfaceShader, "Surfaces/particle_surface.glsl");
    EXPECT_EQ(std::get<std::vector<float>>(material.properties.at("baseColor")), (std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f}));
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at(std::string(GPUFogParticles::kSimpleNoiseScale))), 24.0f);
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at(std::string(GPUFogParticles::kRadialMaskPower))), 1.35f);
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at(std::string(GPUFogParticles::kEdgeSoftness))), 0.22f);
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at(std::string(GPUFogParticles::kSurfaceDepthFade))), 0.66f);
}

namespace
{
// What decides the shader program a particle material compiles to.
std::string ProgramShape(const MaterialDocument& material)
{
    std::string shape = material.surfaceShader + "|" + material.vertexModifier + "|" + material.lightingModel + "|" +
                        std::to_string(static_cast<int>(material.alphaMode)) + "|" +
                        std::to_string(material.doubleSided) + "|" + std::to_string(material.zWrite.value_or(true));
    for (const auto& keyword : material.keywords)
        shape += "|" + keyword;
    return shape;
}
} // namespace

// The variant cook cooks ParticleRenderMaterialShapes; every material extraction builds, whatever the
// renderer's settings and source material, needs one of those programs.
TEST(ParticleCompatibility, EveryRenderMaterialCompilesToACookedShape)
{
    std::vector<std::string> cooked;
    for (const auto& shape : Particles::ParticleRenderMaterialShapes())
        cooked.push_back(ProgramShape(shape));
    ASSERT_EQ(cooked.size(), 20u);

    MaterialDocument source;
    source.keywords = {"SOURCE_KEYWORD"};
    source.lightingModel = "StandardPBR";
    source.textures["albedoMap"] = GUID::Generate().ToString();
    source.textures["wingMaskMap"] = GUID::Generate().ToString();
    const GUID map = GUID::Generate();
    const std::array<const MaterialDocument*, 2> sources = {nullptr, &source};
    uint32 checked = 0;
    for (const auto lighting : {Components::ParticleLightingMode::Unlit, Components::ParticleLightingMode::Lit,
                                Components::ParticleLightingMode::SixWay})
        for (const auto layout : {Components::ParticleSixWayLayout::SignedAxes,
                                  Components::ParticleSixWayLayout::RightTopBackRgba,
                                  Components::ParticleSixWayLayout::TopLeftRightBottomBackFront})
            for (uint32 maps = 0; maps < 16; ++maps)
                for (const MaterialDocument* from : sources)
                {
                    Components::ParticleRenderer renderer;
                    renderer.Lighting = lighting;
                    renderer.SixWayLayout = layout;
                    if (maps & 1u)
                        renderer.Texture.Set(map);
                    if (maps & 2u)
                        renderer.EmissionTexture.Set(map);
                    if (maps & 4u)
                        renderer.SixWayMapA.Set(map);
                    if (maps & 8u)
                        renderer.SixWayMapB.Set(map);
                    renderer.Columns = 1u + maps;
                    renderer.EmissionIntensity = static_cast<float>(maps);
                    const std::string shape = ProgramShape(Particles::BuildParticleRenderMaterialDocument(renderer, from));
                    EXPECT_NE(std::find(cooked.begin(), cooked.end(), shape), cooked.end()) << shape;
                    ++checked;
                }
    EXPECT_EQ(checked, 3u * 3u * 16u * 2u);
}

// The variant cook cooks each shape with ParticleRenderVariants only, so it must hold every variant the
// renderer asks for: the base registration, and the late transparent pass over the particle commands'
// keywords on the sprite's layout and a mesh particle's. The pass takes kLateTransparentKeywords of its
// pipeline's TransmissiveRender keywords; the shipped nodes carry ForwardPlus, Instanced and Shadows,
// with IBL (Assets ForwardPlus, the web library's Web) and without (the web smoke and DDGI projects).
TEST(ParticleCompatibility, TheRequestSetCoversEveryVariantTheRendererDraws)
{
    using Rendering::MaterialKeyword;
    using Rendering::VertexAttributeFlags;
    using Engine::Renderer::Pipeline::Nodes::TransmissivePassNode;
    const auto sprite = Engine::Renderer::PrimitiveGenerator::GeneratePlaneSpriteUv();
    for (const auto& vertex : sprite.Vertices)
        EXPECT_TRUE(vertex.Tangent[0] != 0.0f || vertex.Tangent[1] != 0.0f || vertex.Tangent[2] != 0.0f)
            << "the sprite's layout is StandardMeshWithTangent only while every vertex carries a tangent";

    std::vector<std::pair<MaterialKeyword, VertexAttributeFlags>> reached = {
        {MaterialKeyword::None, VertexAttributeFlags::StandardMesh}};
    const MaterialKeyword node = MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced | MaterialKeyword::Shadows;
    for (const MaterialKeyword transmissive : {node, node | MaterialKeyword::IBL})
        for (const VertexAttributeFlags layout :
             {VertexAttributeFlags::StandardMesh, VertexAttributeFlags::StandardMeshWithTangent})
            reached.push_back(
                {(transmissive & TransmissivePassNode::kLateTransparentKeywords) | Particles::kParticleDrawKeywords, layout});

    const auto variants = Particles::ParticleRenderVariants();
    for (const auto& [keywords, layout] : reached)
        EXPECT_TRUE(std::any_of(variants.begin(), variants.end(), [&](const Particles::ParticleRenderVariant& variant)
                                { return variant.PassKeywords == keywords && variant.VertexFlags == layout; }))
            << "keywords " << static_cast<uint64_t>(keywords) << ", layout " << static_cast<uint32_t>(layout);
    EXPECT_EQ(variants.size(), reached.size()) << "the set cooks a variant the renderer never draws";
}

TEST(ParticleCompatibility, RenderMaterialPreservesSourceValuesButOwnsParticlePipeline)
{
    const Components::ParticleRenderer renderer;
    MaterialDocument source;
    source.properties["customValue"] = 4.0f;
    source.textures["albedoMap"] = "source-texture";
    source.vertexModifier = "other.glsl";
    source.customVertexShader = true;
    source.surfaceGraph = "other-graph";
    const auto material = Particles::BuildParticleRenderMaterialDocument(renderer, &source);
    EXPECT_EQ(material.vertexModifier, "Particles/particle_vertex.glsl");
    EXPECT_EQ(material.surfaceShader, "Surfaces/particle_surface.glsl");
    EXPECT_TRUE(material.surfaceGraph.empty());
    EXPECT_FALSE(material.customVertexShader);
    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    ASSERT_TRUE(material.zWrite.has_value());
    EXPECT_FALSE(*material.zWrite);
    EXPECT_TRUE(material.doubleSided);
    EXPECT_EQ(material.lightingModel, "Unlit");
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at("customValue")), 4.0f);
    EXPECT_EQ(material.textures.at("albedoMap"), "source-texture");
    EXPECT_EQ(material.keywords, (std::vector<std::string>{"PARTICLE_BUFFER", "PARTICLE_TEXTURE"}));
}

TEST(ParticleCompatibility, RenderMaterialSelectsSixWayPackingAndClampsSheetBounds)
{
    Components::ParticleRenderer renderer;
    renderer.Lighting = Components::ParticleLightingMode::SixWay;
    renderer.SixWayLayout = Components::ParticleSixWayLayout::RightTopBackRgba;
    const auto positive = GUID::Generate();
    const auto negative = GUID::Generate();
    renderer.SixWayMapA.Set(positive);
    renderer.SixWayMapB.Set(negative);
    renderer.Columns = 0;
    renderer.Rows = 300;
    renderer.FrameCount = 1000;
    const auto material = Particles::BuildParticleRenderMaterialDocument(renderer, nullptr);
    EXPECT_EQ(material.keywords, (std::vector<std::string>{"PARTICLE_BUFFER", "PARTICLE_LIT", "SIX_WAY", "SIX_WAY_RGBA"}));
    EXPECT_EQ(material.textures.at("positiveAxesMap"), positive.ToString());
    EXPECT_EQ(material.textures.at("negativeAxesMap"), negative.ToString());
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at("sheetColumns")), 1.0f);
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at("sheetRows")), 256.0f);
    EXPECT_FLOAT_EQ(std::get<float>(material.properties.at("sheetFrames")), 256.0f);
}

// Blend Frames asks for a second, blending fetch per fragment; a sheet of one frame has nothing to
// blend, so its material turns the blend off and the surface takes one fetch.
TEST(ParticleCompatibility, AOneFrameSheetDoesNotBlendFrames)
{
    Components::ParticleRenderer renderer;
    renderer.Texture.Set(GUID::Generate());
    renderer.BlendFrames = true;
    const auto single = Particles::BuildParticleRenderMaterialDocument(renderer, nullptr);
    EXPECT_FLOAT_EQ(std::get<float>(single.properties.at("sheetBlend")), 0.0f);
    renderer.Columns = 2;
    renderer.Rows = 2;
    const auto sheet = Particles::BuildParticleRenderMaterialDocument(renderer, nullptr);
    EXPECT_FLOAT_EQ(std::get<float>(sheet.properties.at("sheetBlend")), 1.0f);
    renderer.FrameCount = 1;
    const auto oneFrameOfFour = Particles::BuildParticleRenderMaterialDocument(renderer, nullptr);
    EXPECT_FLOAT_EQ(std::get<float>(oneFrameOfFour.properties.at("sheetBlend")), 0.0f);
}

// Under the default straight-alpha blend a fragment of opacity 0 changes nothing and the surface
// discards it unshaded; an authored blend such as additive draws at opacity 0, so it keeps them.
TEST(ParticleCompatibility, OnlyAStraightAlphaBlendDiscardsTransparentFragments)
{
    const Components::ParticleRenderer renderer;
    const auto procedural = Particles::BuildParticleRenderMaterialDocument(renderer, nullptr);
    EXPECT_FLOAT_EQ(std::get<float>(procedural.properties.at("straightAlphaBlend")), 1.0f);
    MaterialDocument additive;
    additive.blend = MaterialBlendState{.SrcColorFactor = MaterialBlendFactor::One,
                                        .DstColorFactor = MaterialBlendFactor::One};
    const auto added = Particles::BuildParticleRenderMaterialDocument(renderer, &additive);
    EXPECT_FLOAT_EQ(std::get<float>(added.properties.at("straightAlphaBlend")), 0.0f);
}

// The material carries the power of the near fade its colour takes beyond what its alpha gives it, from
// the whole blend: 0 for straight alpha and for every blend that is not Add with a destination of One or
// OneMinusSrcAlpha (multiply among them), 1 for premultiplied and SrcAlpha / One, 2 for One / One.
static float NearFadeColorExponentFor(const MaterialBlendState& blend)
{
    const Components::ParticleRenderer renderer;
    MaterialDocument source;
    source.blend = blend;
    const auto material = Particles::BuildParticleRenderMaterialDocument(renderer, &source);
    return std::get<float>(material.properties.at("nearFadeColorExponent"));
}

TEST(ParticleCompatibility, TheMaterialDerivesTheColourFadeFromItsBlend)
{
    using Factor = MaterialBlendFactor;
    const auto exponent = NearFadeColorExponentFor;
    const Components::ParticleRenderer renderer;
    const auto straight = Particles::BuildParticleRenderMaterialDocument(renderer, nullptr);
    EXPECT_FLOAT_EQ(std::get<float>(straight.properties.at("nearFadeColorExponent")), 0.0f) << "straight alpha";
    EXPECT_FLOAT_EQ(exponent({.SrcColorFactor = Factor::One, .DstColorFactor = Factor::OneMinusSrcAlpha}), 1.0f)
        << "premultiplied";
    EXPECT_FLOAT_EQ(exponent({.SrcColorFactor = Factor::SrcAlpha, .DstColorFactor = Factor::One}), 1.0f)
        << "SrcAlpha / One";
    EXPECT_FLOAT_EQ(exponent({.SrcColorFactor = Factor::One, .DstColorFactor = Factor::One}), 2.0f) << "One / One";
    EXPECT_FLOAT_EQ(exponent({.SrcColorFactor = Factor::DstColor, .DstColorFactor = Factor::Zero}), 0.0f)
        << "multiply DstColor / Zero";
    EXPECT_FLOAT_EQ(exponent({.SrcColorFactor = Factor::Zero, .DstColorFactor = Factor::SrcColor}), 0.0f)
        << "multiply Zero / SrcColor";
    EXPECT_FLOAT_EQ(exponent({.SrcColorFactor = Factor::One, .DstColorFactor = Factor::One, .ColorOp = MaterialBlendOp::Min}), 0.0f)
        << "Min";
}

// Blended particles composite back to front: whatever the order, the farther draws first; one
// emitter's particles at one depth draw in the order named.
TEST(ParticleCompatibility, TheFartherParticleDrawsFirstInEveryOrder)
{
    using Components::ParticleDrawOrder;
    using Particles::ParticleSortKey;
    for (const auto order : {ParticleDrawOrder::Spawn, ParticleDrawOrder::Lifetime, ParticleDrawOrder::ReverseLifetime,
                             ParticleDrawOrder::ViewDepth})
    {
        const ParticleSortKey far{.Depth = 9.0f, .Emitter = 2, .Order = order, .Age = 0.1f, .SpawnIndex = 5};
        const ParticleSortKey near{.Depth = 3.0f, .Emitter = 1, .Order = order, .Age = 0.9f, .SpawnIndex = 1};
        EXPECT_TRUE(Particles::DrawsBefore(far, near)) << static_cast<int>(order);
        EXPECT_FALSE(Particles::DrawsBefore(near, far)) << static_cast<int>(order);
    }
    const ParticleSortKey older{.Depth = 5.0f, .Emitter = 1, .Order = ParticleDrawOrder::Spawn, .Age = 0.8f, .SpawnIndex = 1};
    const ParticleSortKey newer{.Depth = 5.0f, .Emitter = 1, .Order = ParticleDrawOrder::Spawn, .Age = 0.2f, .SpawnIndex = 2};
    EXPECT_TRUE(Particles::DrawsBefore(older, newer)) << "Spawn: the newest draws on top";
    ParticleSortKey olderByLifetime = older;
    ParticleSortKey newerByLifetime = newer;
    olderByLifetime.Order = ParticleDrawOrder::Lifetime;
    newerByLifetime.Order = ParticleDrawOrder::Lifetime;
    EXPECT_TRUE(Particles::DrawsBefore(newerByLifetime, olderByLifetime)) << "Lifetime: the oldest draws on top";
}

// A renderer left at its defaults, and an emitter with none, sort every particle by its own depth.
TEST(ParticleCompatibility, TheDefaultDrawOrderIsViewDepth)
{
    EXPECT_EQ(Components::ParticleRenderer{}.DrawOrder, Components::ParticleDrawOrder::ViewDepth);
}

// Particles face the camera's position by default: a particle close to a wide-angle camera then
// covers the part of the view it fills instead of the width a camera-plane quad spreads over, which
// drew about 1.7 times the fragments with the camera inside a smoke column.
TEST(ParticleCompatibility, TheDefaultBillboardFacesTheCameraPosition)
{
    EXPECT_EQ(Components::ParticleRenderer{}.Billboard, Components::ParticleBillboard::FaceCameraPosition);
}

// The seed smoke emits 80 a second and gives each particle up to 6.25 s: 500 alive at the peak, the
// Amount the inspector asks for.
TEST(ParticleCompatibility, TheContinuousPeakIsTheHighestRateTimesTheLongestLifetime)
{
    using namespace Particles;
    auto document = MakeDefaultStack();
    document.Lifetime = 2.0f;
    auto rate = MakeProcessorInstance(ParticleEmitRateProcessor());
    rate.Params<ParticleEmitRateParameters>().Rate = ParticleValue(40.0f, 80.0f);
    uint32 id = 0;
    std::string error;
    for (auto& phase : document.Phases)
        for (auto& processor : phase.Processors)
            processor.Enabled = processor.Descriptor != &ParticleEmitRateProcessor();
    ASSERT_TRUE(AddProcessor(document, document.EntryPhase, rate, id, error)) << error;
    EXPECT_FLOAT_EQ(ContinuousEmissionPeak(document), 160.0f) << "80 a second for the stack's 2 s";

    auto lifetime = MakeProcessorInstance(ParticlePropertyProcessor());
    lifetime.Stage = ParticleStage::Birth;
    auto& property = lifetime.Params<ParticlePropertyParameters>();
    property.Target = ParticleAttribute::Lifetime;
    property.Operation = ParticleOperation::Set;
    property.Value[0] = ParticleValue(3.75f, 6.25f);
    ASSERT_TRUE(AddProcessor(document, document.EntryPhase, lifetime, id, error)) << error;
    EXPECT_FLOAT_EQ(ContinuousEmissionPeak(document), 500.0f) << "80 a second for up to 6.25 s";
}
