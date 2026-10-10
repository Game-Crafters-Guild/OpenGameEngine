#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// How a height store encodes its samples.
struct HeightEncoding
{
    PageFieldFormat Format = PageFieldFormat::HeightR32F;
    HeightQuantum Quantum;
};

/// The encoding of absolute heights spanning [minHeight, maxHeight]: 16-bit words at
/// range / 65535 from minHeight when that step is at most kHeightStepMax (a range up to
/// 655.35 m), else 32-bit floats.
HeightEncoding ChooseAbsoluteHeightEncoding(float32 minHeight, float32 maxHeight);

/// The encoding of normalized heights (.r16 and 16-bit PNG sources): 16-bit words at 1 / 65535
/// from 0, so a 16-bit source sample is its own word and decodes to the value today's decoders give.
HeightEncoding NormalizedHeightEncoding();

/// One page in a store's encoding, ready to write.
struct EncodedPage
{
    std::vector<uint8> Bytes; ///< PageStoredBytes(format) bytes
    uint64 Hash = 0;          ///< FNV-1a 64 of Bytes
    uint32 MinWord = 0;
    uint32 MaxWord = 0;
};

/// Encodes the kPageSampleCount samples of a page (row-major, apron included) in `header`'s
/// encoding, with the footprint's lowest and highest level-0 values. Every value is one the store
/// already holds (the cooker quantizes before it filters or bounds), so each encodes to its word.
void EncodeHeightPage(const PageStoreHeader& header, std::span<const float32> samples, float32 footprintMin,
                      float32 footprintMax, EncodedPage& out);

/// Decodes a page's stored bytes into kPageSampleCount samples.
void DecodeHeightPage(const PageStoreHeader& header, std::span<const uint8> bytes, std::span<float32> out);

/// The value of a page's MinWord or MaxWord.
float32 DecodeHeightWord(const PageStoreHeader& header, uint32 word);

} // namespace GameEngine::PageStreaming
