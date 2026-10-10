#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Assets/RenderPipelineAsset.h"

using namespace GameEngine;

namespace
{
static Vector<uint8> Bytes(const std::string& s)
{
    Vector<uint8> out;
    out.resize(s.size());
    if (!s.empty())
        std::memcpy(out.data(), s.data(), s.size());
    return out;
}
} // namespace

TEST(RenderPipelineAsset, ParsesSchemaV2EnvelopeAndSummaries)
{
    RenderPipelineAsset a(GUID::Generate(), "Assets/RenderPipelines/Test.rendergraph");

    const std::string json = R"JSON(
{
  "schemaVersion": 2,
  "pipelineName": "TestPipe",
  "resources": {
    "ClusterBuffer": { "kind": "Buffer", "scope": "PerView" },
    "ViewColor": { "kind": "Texture", "scope": "PerView" }
  },
  "passes": [
    { "id": "DepthPrepass", "type": "DepthPrepass", "enabled": true },
    { "id": "Cull", "type": "ComputeShader", "enabled": false },
    { "id": "World", "type": "WorldRender" }
  ],
  "outputs": { "FinalColor": "View.Resolve" }
}
)JSON";

    EXPECT_TRUE(a.LoadFromData(Bytes(json)));
    EXPECT_TRUE(a.GetErrors().empty());
    EXPECT_EQ(a.GetDocument().schemaVersion, 2u);
    EXPECT_EQ(a.GetDocument().pipelineName, "TestPipe");
    ASSERT_EQ(a.GetDocument().passes.size(), 3u);
    EXPECT_EQ(a.GetDocument().passes[0].id, "DepthPrepass");
    EXPECT_EQ(a.GetDocument().passes[0].type, "DepthPrepass");
    EXPECT_TRUE(a.GetDocument().passes[0].enabled);
    EXPECT_EQ(a.GetDocument().passes[1].id, "Cull");
    EXPECT_FALSE(a.GetDocument().passes[1].enabled);
    ASSERT_EQ(a.GetDocument().resources.size(), 2u);
}

TEST(RenderPipelineAsset, RejectsWrongSchemaVersion)
{
    RenderPipelineAsset a(GUID::Generate(), "Assets/RenderPipelines/Test.rendergraph");
    const std::string json = R"JSON(
{
  "schemaVersion": 1,
  "passes": []
}
)JSON";
    EXPECT_FALSE(a.LoadFromData(Bytes(json)));
    EXPECT_FALSE(a.GetErrors().empty());
}

TEST(RenderPipelineAsset, ReportsJsonParseErrors)
{
    RenderPipelineAsset a(GUID::Generate(), "Assets/RenderPipelines/Test.rendergraph");
    EXPECT_FALSE(a.LoadFromData(Bytes("{ not json }")));
    EXPECT_FALSE(a.GetErrors().empty());
}

