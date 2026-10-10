#include "Rendering/ShaderCache/ShaderPackageContainer.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace GameEngine::Rendering
{
namespace
{
constexpr char kMagic[8] = {'G', 'E', 'S', 'H', 'D', 'R', 'P', 'K'};
constexpr size_t kHeaderSize = sizeof(kMagic) + sizeof(uint32_t) * 2;

// The on-disk table entry. Written and read as raw bytes, so its layout is the
// format: 16-byte name, type, 4 bytes of padding, offset, size.
struct ChunkRecord
{
    char Name[kShaderPackageChunkNameMax + 1];
    uint32_t Type;
    uint64_t Offset;
    uint64_t Size;
};
static_assert(sizeof(ChunkRecord) == 40, "the chunk table entry is 40 bytes on disk");

// Type codes stored in the table. Readers identify chunks by name; the code is
// kept so a listing can show what each chunk holds.
enum class ChunkType : uint32_t
{
    Meta = 0,
    Spirv = 1,
    CacheInfo = 2,
    Wgsl = 3,
};

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool TypeForName(std::string_view name, ChunkType& out)
{
    if (name == kShaderPackageMetaChunk)
        out = ChunkType::Meta;
    else if (name == kShaderPackageCacheInfoChunk)
        out = ChunkType::CacheInfo;
    else if (EndsWith(name, kShaderPackageSpirvSuffix))
        out = ChunkType::Spirv;
    else if (EndsWith(name, kShaderPackageWgslSuffix))
        out = ChunkType::Wgsl;
    else
        return false;
    return true;
}

bool Fail(std::string* outError, std::string message)
{
    if (outError)
        *outError = std::move(message);
    return false;
}

ShaderPackageReadResult Malformed(std::string* outError, std::string message)
{
    if (outError)
        *outError = std::move(message);
    return ShaderPackageReadResult::Malformed;
}

template <typename T>
void AppendRaw(std::vector<uint8_t>& out, const T& value)
{
    const auto* raw = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}
} // namespace

bool WriteShaderPackageContainer(std::span<const ShaderPackageChunkSource> chunks,
                                 std::vector<uint8_t>& out,
                                 std::string* outError)
{
    std::vector<ChunkRecord> records(chunks.size());
    std::unordered_set<std::string_view> names;
    uint64_t payloadOffset = kHeaderSize + chunks.size() * sizeof(ChunkRecord);
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        const ShaderPackageChunkSource& chunk = chunks[i];
        ChunkType type{};
        if (chunk.Name.empty() || chunk.Name.size() > kShaderPackageChunkNameMax)
            return Fail(outError, "shaderpkg chunk name '" + chunk.Name + "' must be 1 to " +
                                      std::to_string(kShaderPackageChunkNameMax) + " characters");
        if (!TypeForName(chunk.Name, type))
            return Fail(outError, "shaderpkg chunk name '" + chunk.Name + "' is not a known chunk kind");
        if (!names.insert(chunk.Name).second)
            return Fail(outError, "shaderpkg chunk '" + chunk.Name + "' appears twice");

        ChunkRecord& record = records[i];
        std::memset(&record, 0, sizeof(record));
        std::memcpy(record.Name, chunk.Name.data(), chunk.Name.size());
        record.Type = static_cast<uint32_t>(type);
        record.Offset = payloadOffset;
        record.Size = chunk.Bytes.size();
        payloadOffset += record.Size;
    }

    out.clear();
    out.reserve(static_cast<size_t>(payloadOffset));
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    AppendRaw(out, kShaderPackageVersion);
    AppendRaw(out, static_cast<uint32_t>(records.size()));
    for (const ChunkRecord& record : records)
        AppendRaw(out, record);
    for (const ShaderPackageChunkSource& chunk : chunks)
        out.insert(out.end(), chunk.Bytes.begin(), chunk.Bytes.end());
    return true;
}

ShaderPackageReadResult ReadShaderPackageContainer(std::span<const uint8_t> bytes,
                                                   std::vector<ShaderPackageChunkView>& out,
                                                   std::string* outError)
{
    out.clear();
    if (bytes.size() < kHeaderSize)
        return Malformed(outError, "shaderpkg header truncated");
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return Malformed(outError, "Invalid shaderpkg magic");

    uint32_t version = 0;
    uint32_t count = 0;
    std::memcpy(&version, bytes.data() + sizeof(kMagic), sizeof(version));
    std::memcpy(&count, bytes.data() + sizeof(kMagic) + sizeof(version), sizeof(count));
    if (version != kShaderPackageVersion)
    {
        if (outError)
            *outError = "Unsupported shaderpkg version " + std::to_string(version) +
                        "; this build reads version " + std::to_string(kShaderPackageVersion) + ".";
        return ShaderPackageReadResult::UnsupportedVersion;
    }
    if (count == 0)
        return Malformed(outError, "Empty shaderpkg (no chunks)");

    // Divide before multiplying: size_t is 32-bit on Wasm, while the file's
    // count is an arbitrary uint32_t. Reject impossible tables before allocation.
    if (count > (bytes.size() - kHeaderSize) / sizeof(ChunkRecord))
        return Malformed(outError, "shaderpkg chunk table truncated");
    std::vector<ChunkRecord> records(count);
    std::memcpy(records.data(), bytes.data() + kHeaderSize, count * sizeof(ChunkRecord));
    const uint64_t tableEnd = kHeaderSize + static_cast<uint64_t>(count) * sizeof(ChunkRecord);

    // Validate the whole container before exposing any payload. Aliased chunks
    // could otherwise turn a small package into many separately allocated stages.
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    spans.reserve(records.size());
    for (const ChunkRecord& record : records)
    {
        if (record.Offset < tableEnd || record.Offset > bytes.size() ||
            record.Size > bytes.size() - record.Offset)
            return Malformed(outError, "shaderpkg chunk payload out of bounds");
        if (record.Size != 0)
            spans.emplace_back(record.Offset, record.Offset + record.Size);
    }
    std::sort(spans.begin(), spans.end());
    uint64_t previousEnd = tableEnd;
    for (const auto& [begin, end] : spans)
    {
        if (begin < previousEnd)
            return Malformed(outError, "shaderpkg chunk payloads overlap");
        previousEnd = end;
    }

    out.reserve(records.size());
    for (const ChunkRecord& record : records)
    {
        ShaderPackageChunkView& view = out.emplace_back();
        view.Name.assign(record.Name, strnlen(record.Name, sizeof(record.Name)));
        view.Type = record.Type;
        view.Bytes = bytes.subspan(static_cast<size_t>(record.Offset), static_cast<size_t>(record.Size));
    }
    return ShaderPackageReadResult::Read;
}

} // namespace GameEngine::Rendering
