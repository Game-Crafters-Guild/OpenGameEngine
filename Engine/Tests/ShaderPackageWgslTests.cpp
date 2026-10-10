// .shaderpkg WGSL chunk extension: packages may carry "<stage>-wgsl" chunks
// (the WebGPU backend's browser format) beside the SPIR-V chunks, and readers
// that predate the extension skip them — pinned here by round-tripping both
// chunk kinds through Save/Parse, and by the per-stage form the parse serves
// for each ShaderSourceKind.

#include "Rendering/Core/Device.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace GameEngine::Rendering;

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> ReadFileBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

} // namespace

TEST(ShaderPackageWgsl, RoundTripsSpvAndWgslChunks)
{
    const fs::path out = fs::temp_directory_path() / "ge_wgsl_chunk_test.shaderpkg";

    ShaderMeta meta{};
    meta.EntryPoints["vs"] = "main";

    const std::unordered_map<std::string, std::vector<uint8_t>> spv = {
        {"vs", {0x03, 0x02, 0x23, 0x07, 0x01}},
        {"fs", {0x03, 0x02, 0x23, 0x07, 0x02}},
    };
    const std::unordered_map<std::string, std::vector<uint8_t>> wgsl = {
        {"vs", Bytes("@vertex fn main() {}")},
        {"fs", Bytes("@fragment fn main() {}")},
    };

    std::string error;
    ASSERT_TRUE(SaveShaderPkg(out, meta, spv, std::nullopt, &error, wgsl)) << error;

    ShaderPackage asSpirv{};
    ASSERT_EQ(ParseShaderPkgFromBytes(ReadFileBytes(out), ShaderSourceKind::SpirV, asSpirv, &error), ShaderPackageReadResult::Read)
        << error;
    EXPECT_EQ(asSpirv.stageBytes.size(), 2u);
    EXPECT_EQ(asSpirv.stageBytes.at("vs"), spv.at("vs"));
    EXPECT_EQ(asSpirv.stageBytes.at("fs"), spv.at("fs"));
    EXPECT_EQ(asSpirv.wgslStages.size(), 2u);

    ShaderPackage asWgsl{};
    ASSERT_EQ(ParseShaderPkgFromBytes(ReadFileBytes(out), ShaderSourceKind::Wgsl, asWgsl, &error), ShaderPackageReadResult::Read)
        << error;
    EXPECT_EQ(asWgsl.stageBytes.size(), 2u);
    EXPECT_EQ(asWgsl.stageBytes.at("vs"), wgsl.at("vs"));
    EXPECT_EQ(asWgsl.stageBytes.at("fs"), wgsl.at("fs"));

    std::error_code ec;
    fs::remove(out, ec);
}

TEST(ShaderPackageWgsl, PackagesWithoutWgslStayValid)
{
    const fs::path out = fs::temp_directory_path() / "ge_wgsl_absent_test.shaderpkg";

    ShaderMeta meta{};
    meta.EntryPoints["cs"] = "main";
    const std::unordered_map<std::string, std::vector<uint8_t>> spv = {
        {"cs", {0x03, 0x02, 0x23, 0x07, 0x03}},
    };

    std::string error;
    ASSERT_TRUE(SaveShaderPkg(out, meta, spv, std::nullopt, &error)) << error;

    ShaderPackage parsed{};
    ASSERT_EQ(ParseShaderPkgFromBytes(ReadFileBytes(out), ShaderSourceKind::SpirV, parsed, &error), ShaderPackageReadResult::Read)
        << error;
    EXPECT_EQ(parsed.stageBytes.size(), 1u);
    EXPECT_TRUE(parsed.wgslStages.empty());

    // A stage with no WGSL chunk keeps its SPIR-V under a WGSL-ingesting
    // device: the failure belongs to module creation, which names the package,
    // not to the loader silently dropping the stage.
    ShaderPackage asWgsl{};
    ASSERT_EQ(ParseShaderPkgFromBytes(ReadFileBytes(out), ShaderSourceKind::Wgsl, asWgsl, &error), ShaderPackageReadResult::Read)
        << error;
    EXPECT_EQ(asWgsl.stageBytes.at("cs"), spv.at("cs"));

    std::error_code ec;
    fs::remove(out, ec);
}


namespace
{
// Package v1 has a 16-byte header and 40-byte table entries. Build the valid
// control with the public writer so each hostile input differs in one field.
std::vector<uint8_t> ValidBoundsControl()
{
    const fs::path path = fs::temp_directory_path() / "ge_shader_bounds_control.shaderpkg";
    ShaderMeta meta{};
    meta.EntryPoints["cs"] = "main";
    std::string error;
    EXPECT_TRUE(SaveShaderPkg(path, meta, {{"cs", Bytes("spirv")}}, std::nullopt,
                             &error, {{"cs", Bytes("@compute @workgroup_size(1) fn main() {}")}})) << error;
    auto bytes = ReadFileBytes(path);
    std::error_code ec;
    fs::remove(path, ec);
    return bytes;
}

template <typename T>
void WriteField(std::vector<uint8_t>& bytes, size_t offset, T value)
{
    ASSERT_LE(offset + sizeof(value), bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void ExpectRejectedForBothSources(const std::vector<uint8_t>& bytes)
{
    for (const auto source : {ShaderSourceKind::SpirV, ShaderSourceKind::Wgsl})
    {
        ShaderPackage parsed;
        std::string error;
        EXPECT_EQ(ParseShaderPkgFromBytes(bytes, source, parsed, &error), ShaderPackageReadResult::Malformed);
        EXPECT_FALSE(error.empty());
    }
}
} // namespace

TEST(ShaderPackageWgsl, RejectsWrappedChunkExtent)
{
    auto bytes = ValidBoundsControl();
    ASSERT_GE(bytes.size(), 136u);
    ShaderPackage control;
    ASSERT_EQ(ParseShaderPkgFromBytes(bytes, ShaderSourceKind::Wgsl, control), ShaderPackageReadResult::Read);
    // An unknown extension must still have a valid span. Naming it unknown
    // makes the red build expose acceptance without dereferencing the bad span.
    constexpr size_t entry = 16 + 40;
    std::memset(bytes.data() + entry, 0, 16);
    std::memcpy(bytes.data() + entry, "unknown", 7);
    WriteField<uint64_t>(bytes, entry + 24, std::numeric_limits<uint64_t>::max() - 1);
    WriteField<uint64_t>(bytes, entry + 32, 3);
    ExpectRejectedForBothSources(bytes);
}

TEST(ShaderPackageWgsl, RejectsEveryTruncatedPrefix)
{
    const auto bytes = ValidBoundsControl();
    ASSERT_GE(bytes.size(), 136u);
    for (size_t length = 0; length < bytes.size(); ++length)
    {
        SCOPED_TRACE(length);
        ExpectRejectedForBothSources({bytes.begin(), bytes.begin() + length});
    }
}

TEST(ShaderPackageWgsl, RejectsOversizedChunkCountsBeforeAllocation)
{
    const auto control = ValidBoundsControl();
    ASSERT_GE(control.size(), 136u);
    // The first count wraps count * 40 to zero on 32-bit Wasm.
    for (uint32_t count : {0x20000000u, std::numeric_limits<uint32_t>::max()})
    {
        auto bytes = control;
        WriteField<uint32_t>(bytes, 12, count);
        ExpectRejectedForBothSources(bytes);
    }
}

TEST(ShaderPackageWgsl, RejectsInvalidMetadata)
{
    auto bytes = ValidBoundsControl();
    ASSERT_GE(bytes.size(), 136u);
    uint64_t offset = 0;
    std::memcpy(&offset, bytes.data() + 16 + 24, sizeof(offset));
    ASSERT_LT(offset, bytes.size());
    bytes[static_cast<size_t>(offset)] = 0xff;
    ExpectRejectedForBothSources(bytes);
}


TEST(ShaderPackageWgsl, RejectsChunksThatAliasTheTableOrOtherPayloads)
{
    const auto control = ValidBoundsControl();
    ASSERT_GE(control.size(), 136u);
    uint64_t metaOffset = 0, metaSize = 0;
    std::memcpy(&metaOffset, control.data() + 16 + 24, sizeof(metaOffset));
    std::memcpy(&metaSize, control.data() + 16 + 32, sizeof(metaSize));
    // Repeated aliases can amplify a small file into many separately allocated
    // stage payloads. The container writer gives each chunk its own span.
    for (const auto [offset, size] : {std::pair<uint64_t, uint64_t>{0, 16},
                                     std::pair<uint64_t, uint64_t>{metaOffset, metaSize}})
    {
        auto bytes = control;
        constexpr size_t entry = 16 + 2 * 40;
        std::memset(bytes.data() + entry, 0, 16);
        std::memcpy(bytes.data() + entry, "unknown", 7);
        WriteField<uint64_t>(bytes, entry + 24, offset);
        WriteField<uint64_t>(bytes, entry + 32, size);
        ExpectRejectedForBothSources(bytes);
    }
}
