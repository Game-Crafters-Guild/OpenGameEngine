#pragma once

#include "PageStreaming/HeightPageCooker.h"

#include "SourceRangeReader.h"

#include <memory>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{

/// Reads a height source's rows in order, as floats in the source's units: .r32 samples as they
/// are, .r16 and 16-bit PNG samples normalized by 1 / 65535 (the expression of today's decoders,
/// so a normalized sample is bit-identical to theirs). A raw file is read a band at a time with one
/// band of read-ahead (SourceRangeReader); a PNG is decoded whole at Open, within the decoder's
/// limit. One thread at a time.
class HeightSourceRows
{
public:
    /// Opens `source`, reading through `io` when given. Returns an empty string, else the reason.
    std::string Open(const HeightCookSource& source, AssetIOService* io);

    /// Starts reading rows [firstRow, firstRow + rowCount). One band is outstanding at a time.
    void RequestRows(uint32 firstRow, uint32 rowCount);

    /// Waits for the requested rows and writes them to `out` (rowCount * SamplesX floats).
    bool TakeRows(float32* out);

private:
    HeightCookSource m_Source;
    std::unique_ptr<SourceRangeReader> m_Reader;
    std::vector<uint8> m_Band;     // a band of raw samples
    std::vector<uint16> m_Decoded; // the whole PNG
    uint32 m_FirstRow = 0;
    uint32 m_RowCount = 0;
};

} // namespace GameEngine::PageStreaming
