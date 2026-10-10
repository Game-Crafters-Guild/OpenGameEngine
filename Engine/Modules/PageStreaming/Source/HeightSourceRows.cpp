#include "HeightSourceRows.h"

#include <cstring>

#include <stb_image.h>

namespace GameEngine::PageStreaming
{
namespace
{

// Today's 16-bit decoders' constant (Terrain/Heightfield.cpp): a normalized sample is word * this.
constexpr float32 kInv65535 = 1.0f / 65535.0f;

std::size_t BytesPerSample(HeightSourceFormat format)
{
    return format == HeightSourceFormat::R32 ? sizeof(float32) : sizeof(uint16);
}

} // namespace

std::string HeightSourceRows::Open(const HeightCookSource& source, AssetIOService* io)
{
    m_Source = source;
    m_Decoded.clear();
    if (source.Format != HeightSourceFormat::Png16)
    {
        m_Reader = std::make_unique<SourceRangeReader>(source.File, io);
        return m_Reader->IsOpen() ? std::string{} : "the heightmap " + source.File.string() + " could not be opened";
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_us* pixels = stbi_load_16(source.File.string().c_str(), &width, &height, &channels, 1);
    if (!pixels)
        return "the PNG " + source.File.string() + " could not be decoded";
    if (static_cast<uint32>(width) != source.SamplesX || static_cast<uint32>(height) != source.SamplesZ)
    {
        stbi_image_free(pixels);
        return "the PNG " + source.File.string() + " changed size since it was resolved; cook it again";
    }
    m_Decoded.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    stbi_image_free(pixels);
    return {};
}

void HeightSourceRows::RequestRows(uint32 firstRow, uint32 rowCount)
{
    m_FirstRow = firstRow;
    m_RowCount = rowCount;
    if (!m_Reader)
        return;
    const uint64 rowBytes = static_cast<uint64>(m_Source.SamplesX) * BytesPerSample(m_Source.Format);
    m_Reader->Request(rowBytes * firstRow, rowBytes * rowCount);
}

bool HeightSourceRows::TakeRows(float32* out)
{
    const std::size_t count = static_cast<std::size_t>(m_Source.SamplesX) * m_RowCount;
    if (m_Source.Format == HeightSourceFormat::Png16)
    {
        const std::size_t first = static_cast<std::size_t>(m_Source.SamplesX) * m_FirstRow;
        if (first + count > m_Decoded.size())
            return false;
        for (std::size_t i = 0; i < count; ++i)
            out[i] = static_cast<float32>(m_Decoded[first + i]) * kInv65535;
        return true;
    }

    if (!m_Reader->Take(m_Band) || m_Band.size() != count * BytesPerSample(m_Source.Format))
        return false;
    if (m_Source.Format == HeightSourceFormat::R32)
    {
        std::memcpy(out, m_Band.data(), m_Band.size());
        return true;
    }
    uint16 word = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::memcpy(&word, m_Band.data() + i * sizeof(uint16), sizeof(uint16));
        out[i] = static_cast<float32>(word) * kInv65535;
    }
    return true;
}

} // namespace GameEngine::PageStreaming
