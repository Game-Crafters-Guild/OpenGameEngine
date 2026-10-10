#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
#include "Rendering/Core/NamedPushConstantWriter.h"

using namespace GameEngine::Rendering;

static float ReadF32(const std::vector<uint8_t>& buf, size_t off)
{
    float v = 0.0f; std::memcpy(&v, buf.data() + off, sizeof(float)); return v;
}
static int32_t ReadI32(const std::vector<uint8_t>& buf, size_t off)
{
    int32_t v = 0; std::memcpy(&v, buf.data() + off, sizeof(int32_t)); return v;
}

// Helper: build a 4-byte scalar Member with the modern Member layout.
static Member MakeScalarMember(const char* name, uint32_t offset, BaseType base = BaseType::Float)
{
    Member m{};
    m.Name = name;
    m.Type.Kind = TypeKind::Scalar;
    m.Type.Base = base;
    m.Offset = offset;
    m.Size = 4;
    return m;
}

TEST(NamedPushConstantWriter, BloomThreshold_PC_PacksMembersByName)
{
    ShaderMeta meta{};
    PushConstantRangeMeta range{};
    range.Id = 0;
    range.Name = "PC";
    range.Size = 16;
    range.StagesMask = (1u << 1); // fragment (abstract stage mask)
    range.Block.Size = 16;
    range.Block.Members = {
        MakeScalarMember("threshold", 0),
        MakeScalarMember("knee", 4),
        MakeScalarMember("intensity", 8),
    };
    meta.PushConstants.push_back(range);

    NamedPushConstantWriter w(meta, std::string("PC"));
    ASSERT_TRUE(w.IsValid());
    EXPECT_EQ(w.GetRangeSize(), 16u);

    // Schema: threshold (f32 @0), knee (f32 @4), intensity (f32 @8)
    EXPECT_TRUE(w.Add("threshold", 1.25f));
    EXPECT_TRUE(w.Add("knee", 0.5f));
    EXPECT_TRUE(w.Add("intensity", 0.75f));

    const auto& buf = w.GetBuffer();
    ASSERT_EQ(buf.size(), 16u);
    EXPECT_FLOAT_EQ(ReadF32(buf, 0), 1.25f);
    EXPECT_FLOAT_EQ(ReadF32(buf, 4), 0.5f);
    EXPECT_FLOAT_EQ(ReadF32(buf, 8), 0.75f);
}

TEST(NamedPushConstantWriter, TonemapPC_PacksMembersByName)
{
    ShaderMeta meta{};
    PushConstantRangeMeta range{};
    range.Id = 0;
    range.Name = "TonemapPC";
    range.Size = 16;
    range.StagesMask = (1u << 1); // fragment (abstract stage mask)
    range.Block.Size = 16;
    range.Block.Members = {
        MakeScalarMember("exposure", 0, BaseType::Float),
        MakeScalarMember("outEncoding", 4, BaseType::Int),
        MakeScalarMember("ditherMode", 8, BaseType::Int),
    };
    meta.PushConstants.push_back(range);

    NamedPushConstantWriter w(meta, std::string("TonemapPC"));
    ASSERT_TRUE(w.IsValid());
    // Block size is 16 in metadata (3 scalars with 4B alignment; last padded)
    EXPECT_EQ(w.GetRangeSize(), 16u);

    // Schema: exposure (f32 @0), outEncoding (i32 @4), ditherMode (i32 @8)
    EXPECT_TRUE(w.Add("exposure", 2.0f));
    EXPECT_TRUE(w.Add("outEncoding", 1));
    EXPECT_TRUE(w.Add("ditherMode", 0));

    const auto& buf = w.GetBuffer();
    ASSERT_EQ(buf.size(), 16u);
    EXPECT_FLOAT_EQ(ReadF32(buf, 0), 2.0f);
    EXPECT_EQ(ReadI32(buf, 4), 1);
    EXPECT_EQ(ReadI32(buf, 8), 0);
}

