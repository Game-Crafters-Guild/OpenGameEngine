// The .shaderpkg container is read from disk by the engine, the ShaderReflect
// tool and (through that tool) the web cook, so a truncated, aliased or
// otherwise impossible file must be refused before any chunk is handed out.
#include <gtest/gtest.h>

#include "Rendering/ShaderCache/ShaderPackageContainer.h"

#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

constexpr size_t kHeaderSize = 16;
constexpr size_t kRecordSize = 40;
constexpr size_t kRecordOffsetField = 24;
constexpr size_t kRecordSizeField = 32;

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

void PutU64(std::vector<uint8_t>& data, size_t at, uint64_t value)
{
    std::memcpy(data.data() + at, &value, sizeof(value));
}

void PutU32(std::vector<uint8_t>& data, size_t at, uint32_t value)
{
    std::memcpy(data.data() + at, &value, sizeof(value));
}

uint64_t GetU64(const std::vector<uint8_t>& data, size_t at)
{
    uint64_t value = 0;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return value;
}

class ShaderPackageContainerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const std::vector<ShaderPackageChunkSource> chunks = {
            {"meta-bin", m_Meta},
            {"cs-wgsl", m_Wgsl},
        };
        std::string error;
        ASSERT_TRUE(WriteShaderPackageContainer(chunks, m_Valid, &error)) << error;
    }

    bool Reads(const std::vector<uint8_t>& data)
    {
        std::vector<ShaderPackageChunkView> chunks;
        std::string error;
        const ShaderPackageReadResult result = ReadShaderPackageContainer(data, chunks, &error);
        const bool ok = result == ShaderPackageReadResult::Read;
        EXPECT_EQ(ok, error.empty()) << "a refusal must say why, and success must not";
        EXPECT_NE(result, ShaderPackageReadResult::UnsupportedVersion)
            << "only a changed version word may read as another version: " << error;
        return ok;
    }

    // The second chunk's table entry, where the span tests aim.
    static size_t SecondRecord() { return kHeaderSize + kRecordSize; }

    const std::vector<uint8_t> m_Meta = Bytes("meta payload");
    const std::vector<uint8_t> m_Wgsl = Bytes("@compute fn main() {}");
    std::vector<uint8_t> m_Valid;
};

TEST_F(ShaderPackageContainerTest, RoundTripsNamesTypesAndBytesInOrder)
{
    std::vector<ShaderPackageChunkView> chunks;
    std::string error;
    ASSERT_EQ(ReadShaderPackageContainer(m_Valid, chunks, &error), ShaderPackageReadResult::Read) << error;
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].Name, "meta-bin");
    EXPECT_EQ(chunks[0].Type, 0u);
    EXPECT_EQ(std::vector<uint8_t>(chunks[0].Bytes.begin(), chunks[0].Bytes.end()), m_Meta);
    EXPECT_EQ(chunks[1].Name, "cs-wgsl");
    EXPECT_EQ(chunks[1].Type, 3u);
    EXPECT_EQ(std::vector<uint8_t>(chunks[1].Bytes.begin(), chunks[1].Bytes.end()), m_Wgsl);
}

TEST_F(ShaderPackageContainerTest, TheSameChunksAlwaysWriteTheSameBytes)
{
    const std::vector<ShaderPackageChunkSource> chunks = {{"meta-bin", m_Meta}, {"cs-wgsl", m_Wgsl}};
    std::vector<uint8_t> again;
    ASSERT_TRUE(WriteShaderPackageContainer(chunks, again, nullptr));
    EXPECT_EQ(again, m_Valid) << "table padding or layout is not deterministic";
}

TEST_F(ShaderPackageContainerTest, EveryTruncatedPrefixIsRefused)
{
    for (size_t length = 0; length < m_Valid.size(); ++length)
        EXPECT_FALSE(Reads(std::vector<uint8_t>(m_Valid.begin(), m_Valid.begin() + length)))
            << "a " << length << "-byte prefix read as a whole package";
}

TEST_F(ShaderPackageContainerTest, ImpossiblePayloadSpansAreRefused)
{
    const struct
    {
        uint64_t Offset;
        uint64_t Size;
    } spans[] = {{~uint64_t{0} - 1, 3}, {m_Valid.size(), 1}, {0, ~uint64_t{0}}};
    for (const auto& span : spans)
    {
        std::vector<uint8_t> data = m_Valid;
        PutU64(data, SecondRecord() + kRecordOffsetField, span.Offset);
        PutU64(data, SecondRecord() + kRecordSizeField, span.Size);
        EXPECT_FALSE(Reads(data)) << "offset " << span.Offset << " size " << span.Size;
    }
}

TEST_F(ShaderPackageContainerTest, ChunksCannotAliasTheTableOrAnotherPayload)
{
    const uint64_t metaOffset = GetU64(m_Valid, kHeaderSize + kRecordOffsetField);
    const uint64_t metaSize = GetU64(m_Valid, kHeaderSize + kRecordSizeField);
    const struct
    {
        uint64_t Offset;
        uint64_t Size;
    } spans[] = {{0, 16}, {metaOffset, metaSize}};
    for (const auto& span : spans)
    {
        std::vector<uint8_t> data = m_Valid;
        PutU64(data, SecondRecord() + kRecordOffsetField, span.Offset);
        PutU64(data, SecondRecord() + kRecordSizeField, span.Size);
        EXPECT_FALSE(Reads(data)) << "offset " << span.Offset << " size " << span.Size;
    }
}

TEST_F(ShaderPackageContainerTest, AnImpossibleChunkCountIsRefused)
{
    std::vector<uint8_t> data = m_Valid;
    PutU32(data, 12, 0xFFFFFFFFu);
    EXPECT_FALSE(Reads(data));
}

TEST_F(ShaderPackageContainerTest, VersionOneReportsOnlyTheFormatMismatch)
{
    std::vector<uint8_t> data = m_Valid;
    PutU32(data, 8, 1u);
    std::vector<ShaderPackageChunkView> chunks;
    std::string error;
    EXPECT_EQ(ReadShaderPackageContainer(data, chunks, &error), ShaderPackageReadResult::UnsupportedVersion);
    EXPECT_EQ(error, "Unsupported shaderpkg version 1; this build reads version " +
                         std::to_string(kShaderPackageVersion) + ".");
    EXPECT_TRUE(chunks.empty());
}

TEST_F(ShaderPackageContainerTest, TheWriterRefusesNamesNoReaderWouldKnow)
{
    const std::vector<uint8_t> payload = Bytes("x");
    const std::vector<std::vector<ShaderPackageChunkSource>> refused = {
        {{"", payload}},
        {{"not-a-chunk", payload}},
        {{"a-very-long-stage-spv", payload}},
        {{"vs-spv", payload}, {"vs-spv", payload}},
    };
    for (const auto& chunks : refused)
    {
        std::vector<uint8_t> out;
        std::string error;
        EXPECT_FALSE(WriteShaderPackageContainer(chunks, out, &error)) << chunks.front().Name;
        EXPECT_FALSE(error.empty());
    }
}

} // namespace
