#pragma once

#include "Types/Types.h"

#include <memory>
#include <string>

namespace GameEngine {

struct SvgRasterizedImage
{
    uint32 Width = 0;
    uint32 Height = 0;
    std::unique_ptr<uint8[]> Data; // RGBA8, Width * Height * 4
    size_t DataSize = 0;
};

inline constexpr uint32 kMinSvgRasterSize = 16u;
inline constexpr uint32 kMaxSvgRasterSize = 16384u;

// Parse a decimal per-asset raster size (kSvgRasterSizeMetaKey: the target pixel
// length of the longest axis; the SVG aspect ratio is kept). Valid numeric values
// are clamped to the supported range; malformed or empty values leave the caller's
// fallback intact.
bool ParseSvgRasterSizeMeta(const std::string& value, uint32& outTargetPixels);

/// Read the SVG's resolved source dimensions without rasterizing it. Returns
/// false when the SVG cannot be parsed or ThorVG is unavailable.
bool GetSvgSourceSize(const std::string& svgUtf8,
                      float32& outWidth,
                      float32& outHeight);

/// Calculate the exact raster dimensions for a longest-axis target using the
/// same rounding and clamping rules as RasterizeSvgToRgbaAtSize.
bool CalculateSvgRasterDimensions(float32 sourceWidth,
                                  float32 sourceHeight,
                                  float32 targetPixels,
                                  uint32& outWidth,
                                  uint32& outHeight);

/// Set the editor/UI SVG default for the longest rasterized axis. The legacy
/// names remain the default used by RasterizeSvgToRgba's scale-based API.
void SetSvgRasterizerUserScale(float32 targetPixels);
float32 GetSvgRasterizerUserScale();

/// Set/get the independent default used by imported SVG texture assets that do
/// not carry a per-asset kSvgRasterSizeMetaKey override.
void SetSvgTextureRasterizerDefaultSize(float32 targetPixels);
float32 GetSvgTextureRasterizerDefaultSize();

// Rasterize with an explicit longest-axis target. This bypasses the process-wide
// default so asset importers can honor a persisted per-SVG size.
bool RasterizeSvgToRgbaAtSize(const std::string& svgUtf8,
                              float32 targetPixels,
                              SvgRasterizedImage& outImage);

// Rasterize an SVG UTF-8 string into an RGBA8 image buffer using ThorVG.
// `scale` multiplies the process-wide target configured above.
// Returns true on success and fills outImage; returns false on failure.
bool RasterizeSvgToRgba(const std::string& svgUtf8,
                        float32 scale,
                        SvgRasterizedImage& outImage);

} // namespace GameEngine
