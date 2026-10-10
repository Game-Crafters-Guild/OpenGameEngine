// The shader package stores reflection metadata as a positional binary
// encoding, so a field written and a field read must stay in lockstep: a round trip that silently drops or shifts one would feed
// the pipeline builder wrong descriptor layouts.
//
// The decoder also reads a file off disk, so the truncation and corruption
// cases matter as much as the round trip: a damaged package must fail, not read
// past its buffer or allocate on a length it invented.
#include <gtest/gtest.h>

#include "Rendering/ShaderCache/ShaderMetaBinary.h"
#include "Rendering/Materials/ShaderMetaJson.h"
#include "Types/Fnv1a.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

std::vector<uint8_t> Encode(const ShaderMeta& meta)
{
    std::vector<uint8_t> bytes;
    std::string err;
    EXPECT_TRUE(EncodeShaderMetaBinary(meta, bytes, &err)) << err;
    return bytes;
}

// A meta that exercises every field the encoder writes: nested struct members,
// arrays, both optional strides, all three optional-bearing types, and a
// property with enum values.
ShaderMeta MakePopulatedMeta()
{
    ShaderMeta m;
    m.Version = 7;
    m.EntryPoints["vs"] = "VertexMain";
    m.EntryPoints["fs"] = "FragmentMain";

    Member inner;
    inner.Name = "innerScalar";
    inner.Type.Kind = TypeKind::Scalar;
    inner.Type.Base = BaseType::Float;
    inner.Offset = 8;
    inner.Size = 4;
    inner.RowMajor = true;

    Member matrix;
    matrix.Name = "rowsNotCols";
    matrix.Type.Kind = TypeKind::Matrix;
    matrix.Type.Base = BaseType::Float;
    matrix.Type.Rows = 3;
    matrix.Type.Cols = 4;
    matrix.Type.VecSize = 3;
    matrix.Offset = 32;
    matrix.Size = 48;
    matrix.MatrixStride = 16u;

    Member outer;
    outer.Name = "outerStruct";
    outer.Type.Kind = TypeKind::Struct;
    outer.Type.Base = BaseType::Unknown;
    outer.Type.ArrayDims = {4, kRuntimeArrayDim};
    outer.Type.StructMembers = {inner, matrix};
    outer.Offset = 16;
    outer.Size = 32;
    outer.ArrayStride = 48u;
    outer.MatrixStride = 16u;

    PushConstantRangeMeta pc;
    pc.Id = 3;
    pc.Name = "Push";
    pc.Size = 64;
    pc.StagesMask = 0x3;
    pc.Block.Size = 64;
    pc.Block.Members = {outer};
    m.PushConstants = {pc};

    DescriptorBindingMeta uniform;
    uniform.Binding = 0;
    uniform.Name = "GE_Frame";
    uniform.Type = ShaderMetaBindingType::kUniformBuffer;
    uniform.Count = 1;
    uniform.StagesMask = 0x1;
    uniform.ReadOnly = true;
    BlockLayout block;
    block.Size = 128;
    block.Members = {inner};
    uniform.Block = block;

    DescriptorBindingMeta image;
    image.Binding = 4;
    image.Name = "GE_ShadowMap";
    image.Type = ShaderMetaBindingType::kSampledImage;
    image.Count = 8;
    image.StagesMask = 0x2;
    DescriptorBindingMeta::ImageInfo info;
    info.Dim = 3;
    info.Arrayed = true;
    info.Multisample = true;
    info.Cube = true;
    info.Depth = true;
    info.Filtered = true;
    info.Format = 42;
    info.UnsignedInteger = true;
    image.Image = info;

    DescriptorSetMeta set;
    set.Set = 2;
    set.Bindings = {uniform, image};
    m.Sets = {set};

    StageIO in0;
    in0.Location = 1;
    in0.Component = 2;
    in0.Name = "inNormal";
    in0.Type.Kind = TypeKind::Vector;
    in0.Type.Base = BaseType::Float;
    in0.Type.VecSize = 3;
    StageIO out0;
    out0.Location = 0;
    out0.Name = "outColor";
    out0.Builtin = std::string("Position");

    StageMeta vs;
    vs.Inputs = {in0};
    vs.Outputs = {out0};
    vs.EntryPoint = "VertexMain";
    vs.ComputeLocalSize = StageMeta::LocalSize{8, 4, 2};
    StageMeta cs;
    cs.EntryPoint = "CSMain";
    cs.MeshLocalSize = StageMeta::LocalSize{32, 1, 1};
    m.Stages["vs"] = vs;
    m.Stages["cs"] = cs;

    SpecConstantMeta sc;
    sc.Id = 11;
    sc.Name = "kQuality";
    sc.Type.Kind = TypeKind::Scalar;
    sc.Type.Base = BaseType::UInt;
    sc.DefaultValue = std::string("2");
    m.SpecConstants = {sc};

    m.Requirements = {"VK_KHR_ray_query", "shaderInt64"};
    m.RequiredDefines = {"ALPHA_TEST", "FORWARD_PLUS"};

    ShaderProperty prop;
    prop.Name = "pulseSpeed";
    prop.DisplayName = "Pulse Speed";
    prop.Default = {1.0f, 2.0f, 3.0f, 4.0f};
    prop.HasRange = true;
    prop.RangeMin = -1.5f;
    prop.RangeMax = 9.25f;
    prop.Group = "Animation";
    prop.VisibleIf = "mode=1";
    prop.Tooltip = "How fast it pulses";
    prop.EnumValues = {"Off", "Slow", "Fast"};
    prop.Hdr = true;
    prop.Hidden = true;
    prop.HasAlpha = true;
    prop.HasLane = true;
    prop.Lane = 5;
    prop.Component = 2;
    prop.ByteOffset = 48;
    prop.ByteSize = 16;
    prop.SourceFile = "surface.glsl";
    prop.SourceLine = 120;
    m.DeclaredProperties = {prop};
    return m;
}

TEST(ShaderMetaBinaryTest, RoundTripPreservesEveryField)
{
    const ShaderMeta original = MakePopulatedMeta();
    const std::vector<uint8_t> encoded = Encode(original);
    ASSERT_FALSE(encoded.empty());

    ShaderMeta decoded;
    std::string err;
    ASSERT_TRUE(DecodeShaderMetaBinary(encoded.data(), encoded.size(), decoded, &err)) << err;

    // Every field the JSON form carries, in one comparison: a field dropped,
    // shifted or swapped with its neighbour shows up here even when no
    // targeted expectation below names it.
    EXPECT_EQ(nlohmann::json(decoded), nlohmann::json(original));

    EXPECT_EQ(decoded.Version, original.Version);
    EXPECT_EQ(decoded.EntryPoints, original.EntryPoints);
    EXPECT_EQ(decoded.Requirements, original.Requirements);
    EXPECT_EQ(decoded.RequiredDefines, original.RequiredDefines);

    ASSERT_EQ(decoded.PushConstants.size(), 1u);
    const PushConstantRangeMeta& pc = decoded.PushConstants[0];
    EXPECT_EQ(pc.Id, 3u);
    EXPECT_EQ(pc.Name, "Push");
    EXPECT_EQ(pc.Size, 64u);
    EXPECT_EQ(pc.StagesMask, 0x3u);
    ASSERT_EQ(pc.Block.Members.size(), 1u);
    const Member& outer = pc.Block.Members[0];
    EXPECT_EQ(outer.Name, "outerStruct");
    EXPECT_EQ(outer.Type.ArrayDims, (std::vector<uint32_t>{4, kRuntimeArrayDim}));
    ASSERT_TRUE(outer.ArrayStride.has_value());
    EXPECT_EQ(*outer.ArrayStride, 48u);
    ASSERT_TRUE(outer.MatrixStride.has_value());
    EXPECT_EQ(*outer.MatrixStride, 16u);
    // The nested struct member is the recursion this encoding has to survive.
    ASSERT_EQ(outer.Type.StructMembers.size(), 2u);
    EXPECT_EQ(outer.Type.StructMembers[0].Name, "innerScalar");
    EXPECT_TRUE(outer.Type.StructMembers[0].RowMajor);
    EXPECT_EQ(outer.Type.StructMembers[0].Offset, 8u);
    EXPECT_EQ(outer.Type.StructMembers[0].Size, 4u);
    EXPECT_EQ(outer.Type.StructMembers[1].Type.Rows, 3u);
    EXPECT_EQ(outer.Type.StructMembers[1].Type.Cols, 4u);

    ASSERT_EQ(decoded.Sets.size(), 1u);
    const DescriptorSetMeta& set = decoded.Sets[0];
    EXPECT_EQ(set.Set, 2u);
    ASSERT_EQ(set.Bindings.size(), 2u);
    EXPECT_EQ(set.Bindings[0].Name, "GE_Frame");
    EXPECT_TRUE(set.Bindings[0].ReadOnly);
    ASSERT_TRUE(set.Bindings[0].Block.has_value());
    EXPECT_EQ(set.Bindings[0].Block->Size, 128u);
    EXPECT_FALSE(set.Bindings[1].Block.has_value());
    ASSERT_TRUE(set.Bindings[1].Image.has_value());
    EXPECT_EQ(set.Bindings[1].Image->Dim, 3u);
    EXPECT_TRUE(set.Bindings[1].Image->Cube);
    EXPECT_TRUE(set.Bindings[1].Image->UnsignedInteger);
    EXPECT_EQ(set.Bindings[1].Image->Format, 42u);

    ASSERT_EQ(decoded.Stages.size(), 2u);
    const StageMeta& vs = decoded.Stages.at("vs");
    ASSERT_EQ(vs.Inputs.size(), 1u);
    EXPECT_EQ(vs.Inputs[0].Component, 2u);
    EXPECT_EQ(vs.Inputs[0].Type.VecSize, 3u);
    ASSERT_EQ(vs.Outputs.size(), 1u);
    ASSERT_TRUE(vs.Outputs[0].Builtin.has_value());
    EXPECT_EQ(*vs.Outputs[0].Builtin, "Position");
    ASSERT_TRUE(vs.ComputeLocalSize.has_value());
    EXPECT_EQ(vs.ComputeLocalSize->X, 8u);
    EXPECT_FALSE(vs.MeshLocalSize.has_value());
    ASSERT_TRUE(decoded.Stages.at("cs").MeshLocalSize.has_value());
    EXPECT_EQ(decoded.Stages.at("cs").MeshLocalSize->X, 32u);

    ASSERT_EQ(decoded.SpecConstants.size(), 1u);
    EXPECT_EQ(decoded.SpecConstants[0].Name, "kQuality");
    ASSERT_TRUE(decoded.SpecConstants[0].DefaultValue.has_value());
    EXPECT_EQ(*decoded.SpecConstants[0].DefaultValue, "2");

    ASSERT_EQ(decoded.DeclaredProperties.size(), 1u);
    const ShaderProperty& p = decoded.DeclaredProperties[0];
    EXPECT_EQ(p.Name, "pulseSpeed");
    EXPECT_EQ(p.DisplayName, "Pulse Speed");
    EXPECT_EQ(p.Default, (std::array<float, 4>{1.0f, 2.0f, 3.0f, 4.0f}));
    EXPECT_TRUE(p.HasRange);
    EXPECT_FLOAT_EQ(p.RangeMin, -1.5f);
    EXPECT_FLOAT_EQ(p.RangeMax, 9.25f);
    EXPECT_EQ(p.EnumValues, (std::vector<std::string>{"Off", "Slow", "Fast"}));
    EXPECT_TRUE(p.Hdr);
    EXPECT_TRUE(p.Hidden);
    EXPECT_TRUE(p.HasAlpha);
    EXPECT_TRUE(p.HasLane);
    EXPECT_EQ(p.ByteOffset, 48u);
    EXPECT_EQ(p.SourceLine, 120u);
}

TEST(ShaderMetaBinaryTest, DerivedBindingFieldsAreRecomputedNotStored)
{
    // NameId and BuiltLayoutHash are functions of the bindings. Storing them
    // would let a package assert a hash that disagrees with its own data;
    // recomputing on read is what makes that impossible.
    ShaderMeta original = MakePopulatedMeta();
    for (auto& b : original.Sets[0].Bindings)
        b.NameId = 0;
    original.Sets[0].BuiltLayoutHash = 0;

    const std::vector<uint8_t> encoded = Encode(original);
    ShaderMeta decoded;
    ASSERT_TRUE(DecodeShaderMetaBinary(encoded.data(), encoded.size(), decoded, nullptr));

    const DescriptorSetMeta& set = decoded.Sets[0];
    EXPECT_NE(set.BuiltLayoutHash, 0u) << "the set layout hash was not rebuilt";
    EXPECT_EQ(set.BuiltLayoutHash, ComputeDescriptorSetLayoutHash(set));
    for (const auto& b : set.Bindings)
    {
        EXPECT_NE(b.NameId, 0u) << "binding name id was not rebuilt";
        EXPECT_EQ(b.NameId, GameEngine::HashStringId(b.Name));
    }
}

TEST(ShaderMetaBinaryTest, EveryTruncationIsRejected)
{
    // A package is a file on disk and can be cut short by a failed write or a
    // half-finished copy. Every prefix of a valid encoding must be refused
    // rather than produce a half-built meta.
    const std::vector<uint8_t> encoded = Encode(MakePopulatedMeta());
    ASSERT_GT(encoded.size(), 64u);

    for (size_t len = 0; len < encoded.size(); ++len)
    {
        ShaderMeta decoded;
        std::string err;
        EXPECT_FALSE(DecodeShaderMetaBinary(encoded.data(), len, decoded, &err))
            << "a " << len << "-byte prefix decoded as if it were complete";
    }

    ShaderMeta whole;
    EXPECT_TRUE(DecodeShaderMetaBinary(encoded.data(), encoded.size(), whole, nullptr));
}

// The encoding's layout, for tests that craft a damaged payload which still
// passes its checksum: magic(4), checksum(8), then the payload -- version(u32),
// entry-point count(u32), then the first entry point's stage-name length(u32).
constexpr size_t kPayloadStart = 12;
constexpr size_t kEntryPointCountAt = kPayloadStart + 4;
constexpr size_t kFirstStringLengthAt = kPayloadStart + 8;

void PutU32(std::vector<uint8_t>& bytes, size_t at, uint32_t value)
{
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}

// Recompute the checksum over a payload the test has altered, so the decoder's
// structural checks are what has to refuse it rather than the checksum.
void Resign(std::vector<uint8_t>& bytes)
{
    const uint64_t checksum =
        GameEngine::Hashing::Fnv1a64(bytes.data() + kPayloadStart, bytes.size() - kPayloadStart);
    std::memcpy(bytes.data() + 4, &checksum, sizeof(checksum));
}

std::string DecodeError(const std::vector<uint8_t>& bytes)
{
    ShaderMeta decoded;
    std::string err;
    EXPECT_FALSE(DecodeShaderMetaBinary(bytes.data(), bytes.size(), decoded, &err));
    EXPECT_EQ(err.find("checksum"), std::string::npos) << "refused by the checksum, not the check: " << err;
    return err;
}

ShaderMeta OneEntryPoint()
{
    ShaderMeta m;
    m.EntryPoints["vs"] = "main";
    return m;
}

TEST(ShaderMetaBinaryTest, AnInventedCountIsRejectedRatherThanAllocated)
{
    // A count bigger than the bytes left is the shape of corruption that turns
    // a length into an allocation. Both a huge one and the smallest impossible
    // one must be refused by the count bound itself.
    const std::vector<uint8_t> valid = Encode(OneEntryPoint());
    const uint32_t bytesLeft = static_cast<uint32_t>(valid.size() - (kEntryPointCountAt + 4));
    for (const uint32_t count : {0xFFFFFFFFu, bytesLeft + 1})
    {
        std::vector<uint8_t> bytes = valid;
        PutU32(bytes, kEntryPointCountAt, count);
        Resign(bytes);
        EXPECT_NE(DecodeError(bytes).find("a count is larger than the bytes left"), std::string::npos)
            << "count " << count;
    }
}

TEST(ShaderMetaBinaryTest, AnInventedStringLengthIsRejectedRatherThanRead)
{
    const std::vector<uint8_t> valid = Encode(OneEntryPoint());
    const uint32_t bytesLeft = static_cast<uint32_t>(valid.size() - (kFirstStringLengthAt + 4));
    for (const uint32_t length : {0xFFFFFFFFu, bytesLeft + 1})
    {
        std::vector<uint8_t> bytes = valid;
        PutU32(bytes, kFirstStringLengthAt, length);
        Resign(bytes);
        EXPECT_NE(DecodeError(bytes).find("a string is longer than the bytes left"), std::string::npos)
            << "length " << length;
    }
}

TEST(ShaderMetaBinaryTest, ATruncatedFieldWithAValidChecksumIsRejected)
{
    std::vector<uint8_t> bytes = Encode(MakePopulatedMeta());
    bytes.resize(bytes.size() - 3);
    Resign(bytes);
    EXPECT_NE(DecodeError(bytes).find("ends before its last field"), std::string::npos);
}

// A struct type nested `depth` levels below the outermost one.
TypeDesc NestedStruct(uint32_t depth)
{
    TypeDesc type;
    type.Kind = TypeKind::Struct;
    if (depth > 0)
    {
        Member member;
        member.Name = "m";
        member.Type = NestedStruct(depth - 1);
        type.StructMembers.push_back(std::move(member));
    }
    return type;
}

ShaderMeta WithSpecConstantNested(uint32_t depth)
{
    ShaderMeta m;
    SpecConstantMeta sc;
    sc.Name = "nested";
    sc.Type = NestedStruct(depth);
    m.SpecConstants.push_back(sc);
    return m;
}

// A payload written by hand, field by field, for inputs the encoder refuses to
// produce. Anchored to the real encoder by NestingPayloadMatchesTheEncoder.
class PayloadWriter
{
  public:
    void U8(uint8_t v) { m_Bytes.push_back(v); }
    void U32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            m_Bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void Str(const std::string& text)
    {
        U32(static_cast<uint32_t>(text.size()));
        m_Bytes.insert(m_Bytes.end(), text.begin(), text.end());
    }

    // Magic, checksum, payload: the encoded form of what was written.
    std::vector<uint8_t> Signed() const
    {
        std::vector<uint8_t> bytes = {'G', 'E', 'S', 'M', 0, 0, 0, 0, 0, 0, 0, 0};
        bytes.insert(bytes.end(), m_Bytes.begin(), m_Bytes.end());
        Resign(bytes);
        return bytes;
    }

  private:
    std::vector<uint8_t> m_Bytes;
};

// NestedStruct(depth) as the encoding lays out a TypeDesc and its one member.
void WriteNestedStruct(PayloadWriter& w, uint32_t depth)
{
    w.U32(static_cast<uint32_t>(TypeKind::Struct));
    w.U32(static_cast<uint32_t>(BaseType::Unknown));
    w.U32(1); // Rows
    w.U32(1); // Cols
    w.U32(1); // VecSize
    w.U32(0); // ArrayDims
    w.U32(depth > 0 ? 1u : 0u);
    if (depth == 0)
        return;
    w.Str("m");
    WriteNestedStruct(w, depth - 1);
    w.U32(0); // Offset
    w.U32(0); // Size
    w.U8(0);  // no ArrayStride
    w.U8(0);  // no MatrixStride
    w.U8(0);  // RowMajor
}

// WithSpecConstantNested(depth), encoded by hand.
std::vector<uint8_t> NestedSpecConstantPayload(uint32_t depth)
{
    PayloadWriter w;
    w.U32(ShaderMeta{}.Version);
    for (int i = 0; i < 4; ++i)
        w.U32(0); // entry points, push constants, sets, stages
    w.U32(1);     // spec constants
    w.U32(0);     // Id
    w.Str("nested");
    WriteNestedStruct(w, depth);
    w.U8(0); // no DefaultValue
    for (int i = 0; i < 3; ++i)
        w.U32(0); // requirements, defines, declared properties
    return w.Signed();
}

TEST(ShaderMetaBinaryTest, NestingPayloadMatchesTheEncoder)
{
    EXPECT_EQ(NestedSpecConstantPayload(32), Encode(WithSpecConstantNested(32)))
        << "the hand-written payload no longer matches the encoding";
}

TEST(ShaderMetaBinaryTest, NestingIsDecodedUpToTheCapAndRefusedPastIt)
{
    // TypeDesc and Member recurse through each other, so a corrupt package
    // could otherwise describe nesting deep enough to run the decoder off the
    // stack.
    const std::vector<uint8_t> atCap = Encode(WithSpecConstantNested(32));
    ShaderMeta decoded;
    std::string err;
    EXPECT_TRUE(DecodeShaderMetaBinary(atCap.data(), atCap.size(), decoded, &err)) << err;

    EXPECT_NE(DecodeError(NestedSpecConstantPayload(33)).find("nests deeper"), std::string::npos);
}

TEST(ShaderMetaBinaryTest, TheEncoderRefusesNestingTheDecoderWouldRefuse)
{
    // Written anyway, such an entry could never load and would be recompiled
    // on every run.
    std::vector<uint8_t> bytes;
    std::string err;
    EXPECT_FALSE(EncodeShaderMetaBinary(WithSpecConstantNested(33), bytes, &err));
    EXPECT_NE(err.find("nests deeper"), std::string::npos) << err;
}

TEST(ShaderMetaBinaryTest, BytesAfterTheLastFieldAreRejected)
{
    std::vector<uint8_t> bytes = Encode(MakePopulatedMeta());
    bytes.push_back(0xAB);
    Resign(bytes);
    EXPECT_NE(DecodeError(bytes).find("bytes follow the last field"), std::string::npos);
}

TEST(ShaderMetaBinaryTest, AnEnumValueOutsideItsTypeIsRejected)
{
    // One spec constant with an empty name: its TypeDesc's Kind follows the
    // version, five empty counts, the id and the name length.
    ShaderMeta meta;
    meta.SpecConstants.push_back(SpecConstantMeta{});
    constexpr size_t kSpecTypeKindAt = kPayloadStart + 4 * 8;
    std::vector<uint8_t> bytes = Encode(meta);
    ASSERT_EQ(bytes[kSpecTypeKindAt], static_cast<uint8_t>(TypeKind::Scalar));
    PutU32(bytes, kSpecTypeKindAt, 0x77);
    Resign(bytes);
    EXPECT_NE(DecodeError(bytes).find("outside its type"), std::string::npos);
}

TEST(ShaderMetaBinaryTest, TheSameMetaAlwaysEncodesToTheSameBytes)
{
    // The stages live in unordered maps. Two metas holding the same stages,
    // inserted in opposite orders into maps grown differently, must encode
    // identically: a package's bytes may not depend on map history.
    const char* stages[] = {"vs", "fs", "cs", "ms", "gs"};
    ShaderMeta forward = MakePopulatedMeta();
    for (const char* stage : stages)
    {
        forward.EntryPoints[stage] = std::string(stage) + "Main";
        forward.Stages[stage].EntryPoint = std::string(stage) + "Main";
    }
    ShaderMeta backward = forward;
    backward.EntryPoints = {};
    backward.Stages = {};
    backward.EntryPoints.reserve(64);
    backward.Stages.reserve(64);
    for (auto it = std::rbegin(stages); it != std::rend(stages); ++it)
    {
        backward.EntryPoints[*it] = forward.EntryPoints.at(*it);
        backward.Stages[*it] = forward.Stages.at(*it);
    }
    EXPECT_EQ(Encode(forward), Encode(backward));
}

TEST(ShaderMetaBinaryTest, EverySingleByteCorruptionIsCaught)
{
    // A binary encoding has no syntax to violate: flip a byte and it decodes
    // into a DIFFERENT valid meta, which would go on to build a pipeline with
    // the wrong descriptor offsets. The magic and the checksum are what refuse
    // it. Every byte, not a sample.
    const std::vector<uint8_t> encoded = Encode(MakePopulatedMeta());
    for (size_t i = 0; i < encoded.size(); ++i)
    {
        std::vector<uint8_t> damaged = encoded;
        damaged[i] ^= 0xFF;
        ShaderMeta decoded;
        std::string err;
        EXPECT_FALSE(DecodeShaderMetaBinary(damaged.data(), damaged.size(), decoded, &err))
            << "a corrupted byte at offset " << i << " decoded as valid";
    }
}

TEST(ShaderMetaBinaryTest, AChunkThatIsNotThisEncodingIsRejected)
{
    // A JSON meta, as a version 1 package held it. It must be refused by the
    // magic rather than mistaken for a payload.
    const std::string json = R"({"Version":1,"Sets":[]})";
    ShaderMeta decoded;
    std::string err;
    EXPECT_FALSE(DecodeShaderMetaBinary(reinterpret_cast<const uint8_t*>(json.data()),
                                        json.size(), decoded, &err));
    EXPECT_FALSE(err.empty());
}

TEST(ShaderMetaBinaryTest, AnEmptyMetaRoundTrips)
{
    const ShaderMeta empty;
    const std::vector<uint8_t> encoded = Encode(empty);
    ShaderMeta decoded;
    std::string err;
    ASSERT_TRUE(DecodeShaderMetaBinary(encoded.data(), encoded.size(), decoded, &err)) << err;
    EXPECT_TRUE(decoded.Sets.empty());
    EXPECT_TRUE(decoded.Stages.empty());
    EXPECT_TRUE(decoded.DeclaredProperties.empty());
    EXPECT_EQ(decoded.Version, empty.Version);
}

} // namespace
