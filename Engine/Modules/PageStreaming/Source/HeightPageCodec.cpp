#include "PageStreaming/HeightPageCodec.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr float32 kWordCount = 65535.0f;

} // namespace

HeightEncoding ChooseAbsoluteHeightEncoding(float32 minHeight, float32 maxHeight)
{
    HeightEncoding encoding;
    const float32 range = maxHeight - minHeight;
    if (!(range <= kWordCount * kHeightStepMax))
        return encoding; // 32-bit floats: past 655.35 m a 16-bit step would be coarser than 1 cm
    encoding.Format = PageFieldFormat::HeightR16;
    encoding.Quantum.Offset = minHeight;
    // A flat field still needs a positive step; any step encodes it as word 0.
    encoding.Quantum.Step = range > 0.0f ? range / kWordCount : kHeightStepMax;
    return encoding;
}

HeightEncoding NormalizedHeightEncoding()
{
    HeightEncoding encoding;
    encoding.Format = PageFieldFormat::HeightR16;
    encoding.Quantum.Offset = 0.0f;
    encoding.Quantum.Step = 1.0f / kWordCount;
    return encoding;
}

void EncodeHeightPage(const PageStoreHeader& header, std::span<const float32> samples, float32 footprintMin,
                      float32 footprintMax, EncodedPage& out)
{
    assert(samples.size() == kPageSampleCount);
    out.Bytes.resize(PageStoredBytes(header.Format));
    if (header.Format == PageFieldFormat::HeightR16)
    {
        auto* words = reinterpret_cast<uint16*>(out.Bytes.data());
        for (std::size_t i = 0; i < kPageSampleCount; ++i)
            words[i] = header.Quantum.Encode(samples[i]);
        out.MinWord = header.Quantum.Encode(footprintMin);
        out.MaxWord = header.Quantum.Encode(footprintMax);
    }
    else
    {
        std::memcpy(out.Bytes.data(), samples.data(), out.Bytes.size());
        out.MinWord = std::bit_cast<uint32>(footprintMin);
        out.MaxWord = std::bit_cast<uint32>(footprintMax);
    }
    out.Hash = Hashing::Fnv1a64(out.Bytes.data(), out.Bytes.size());
}

void DecodeHeightPage(const PageStoreHeader& header, std::span<const uint8> bytes, std::span<float32> out)
{
    assert(out.size() == kPageSampleCount && bytes.size() == PageStoredBytes(header.Format));
    if (header.Format == PageFieldFormat::HeightR16)
    {
        uint16 word = 0;
        for (std::size_t i = 0; i < kPageSampleCount; ++i)
        {
            std::memcpy(&word, bytes.data() + i * sizeof(uint16), sizeof(uint16));
            out[i] = header.Quantum.Decode(word);
        }
        return;
    }
    std::memcpy(out.data(), bytes.data(), bytes.size());
}

float32 DecodeHeightWord(const PageStoreHeader& header, uint32 word)
{
    if (header.Format == PageFieldFormat::HeightR16)
        return header.Quantum.Decode(static_cast<uint16>(word));
    return std::bit_cast<float32>(word);
}

} // namespace GameEngine::PageStreaming
