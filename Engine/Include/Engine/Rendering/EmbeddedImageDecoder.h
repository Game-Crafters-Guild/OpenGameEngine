#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine
{

// Result of decoding an embedded image (PNG/JPEG/etc.) to RGBA pixels.
struct DecodedImage
{
    std::vector<uint8_t> pixels; // RGBA8, row-major
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

// Decode compressed image data (PNG, JPEG, etc.) into RGBA8 pixels.
// Isolates stb_image from the rendering layer.
DecodedImage DecodeImageToRGBA(const uint8_t* compressedData, size_t compressedSize);

} // namespace GameEngine
