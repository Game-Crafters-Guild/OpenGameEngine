#pragma once

namespace GameEngine::Video
{

enum class VideoPixelFormat
{
    RGBA8,
    BGRA8,
    // 10-bit packed (A2B10G10R10: R in the low 10 bits, A in the top 2),
    // matching Rendering::TextureFormat::RGB10A2_UNORM. Used by HDR recording,
    // where the source already carries PQ/HLG-encoded BT.2020 values.
    RGB10A2,
};

// HDR delivery encoding for recorded movies. Off keeps the SDR sRGB path; the
// HDR modes write display-referred BT.2020 values (PQ or HLG) into a 10-bit
// target and tag the stream so players/YouTube treat it as HDR.
enum class VideoHdrMode
{
    Off,
    HDR10_PQ, // BT.2020 primaries + SMPTE ST 2084 (PQ), with mastering + CLL metadata
    HLG,      // BT.2020 primaries + ARIB STD-B67 (HLG)
};

} // namespace GameEngine::Video
