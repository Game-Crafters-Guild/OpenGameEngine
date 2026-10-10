#include "TerrainECS/TerrainZonePayload.h"

#include <cstring>

namespace GameEngine::TerrainECS
{
namespace
{

// 'T','Z','O','N' little-endian. Bumped only on an incompatible layout change.
constexpr uint32 kZonePayloadMagic = 0x4E4F5A54u;
constexpr uint32 kZonePayloadVersion = 1u;

// Header: magic, version, format, width, height (5 x uint32).
constexpr std::size_t kHeaderBytes = 5 * sizeof(uint32);

void AppendU32(std::vector<uint8>& out, uint32 value)
{
    const std::size_t offset = out.size();
    out.resize(offset + sizeof(uint32));
    std::memcpy(out.data() + offset, &value, sizeof(uint32));
}

uint32 ReadU32(const uint8* data, std::size_t offset)
{
    uint32 value = 0;
    std::memcpy(&value, data + offset, sizeof(uint32));
    return value;
}

} // namespace

std::vector<uint8> EncodeZonePayload(const TerrainZonePayload& payload)
{
    const bool sculpt = payload.IsSculpt();
    const std::size_t texelCount = payload.TexelCount();
    const std::size_t dataBytes = sculpt ? texelCount * sizeof(float32) : texelCount;

    std::vector<uint8> out;
    out.reserve(kHeaderBytes + dataBytes);
    AppendU32(out, kZonePayloadMagic);
    AppendU32(out, kZonePayloadVersion);
    AppendU32(out, static_cast<uint32>(payload.Format));
    AppendU32(out, payload.Width);
    AppendU32(out, payload.Height);

    const std::size_t offset = out.size();
    out.resize(offset + dataBytes);
    if (dataBytes > 0)
    {
        if (sculpt)
            std::memcpy(out.data() + offset, payload.Offsets.data(), dataBytes);
        else
            std::memcpy(out.data() + offset, payload.Mask.data(), dataBytes);
    }
    return out;
}

bool DecodeZonePayload(const uint8* data, std::size_t size, TerrainZonePayload& out)
{
    if (data == nullptr || size < kHeaderBytes)
        return false;
    if (ReadU32(data, 0) != kZonePayloadMagic || ReadU32(data, sizeof(uint32)) != kZonePayloadVersion)
        return false;

    const uint32 formatRaw = ReadU32(data, 2 * sizeof(uint32));
    const uint32 width = ReadU32(data, 3 * sizeof(uint32));
    const uint32 height = ReadU32(data, 4 * sizeof(uint32));
    if (formatRaw > static_cast<uint32>(ZonePayloadFormat::PaintMaskR8))
        return false;
    // Cap dimensions before any width*height math so a crafted header can't
    // overflow the texel count or force a huge allocation.
    if (width > kMaxZoneDimension || height > kMaxZoneDimension)
        return false;

    const auto format = static_cast<ZonePayloadFormat>(formatRaw);
    const bool sculpt = format == ZonePayloadFormat::SculptOffsetR32F;
    const std::size_t texelCount = static_cast<std::size_t>(width) * height;
    const std::size_t dataBytes = sculpt ? texelCount * sizeof(float32) : texelCount;
    if (size - kHeaderBytes < dataBytes)
        return false;

    TerrainZonePayload decoded;
    decoded.Allocate(format, width, height);
    if (dataBytes > 0)
    {
        if (sculpt)
            std::memcpy(decoded.Offsets.data(), data + kHeaderBytes, dataBytes);
        else
            std::memcpy(decoded.Mask.data(), data + kHeaderBytes, dataBytes);
    }
    out = std::move(decoded);
    return true;
}

} // namespace GameEngine::TerrainECS
