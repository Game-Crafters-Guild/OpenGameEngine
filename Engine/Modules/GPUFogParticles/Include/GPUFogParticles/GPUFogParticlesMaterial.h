#pragma once

#include "Rendering/Materials/MaterialDocument.h"

#include <string_view>

namespace GameEngine::GPUFogParticles
{

inline constexpr std::string_view kSurfaceShaderPath = "Surfaces/gpu_fog_particles.glsl";

inline constexpr std::string_view kSimpleNoiseScale = "gpuFogSimpleNoiseScale";
inline constexpr std::string_view kSimplexNoiseScale = "gpuFogSimplexNoiseScale";
inline constexpr std::string_view kVoronoiScale = "gpuFogVoronoiScale";
inline constexpr std::string_view kCombinedNoiseRemap = "gpuFogCombinedNoiseRemap";

inline constexpr std::string_view kSimpleNoiseAmount = "gpuFogSimpleNoiseAmount";
inline constexpr std::string_view kSimplexNoiseAmount = "gpuFogSimplexNoiseAmount";
inline constexpr std::string_view kVoronoiNoiseAmount = "gpuFogVoronoiNoiseAmount";
inline constexpr std::string_view kRadialMaskPower = "gpuFogRadialMaskPower";

inline constexpr std::string_view kSimpleNoiseRemap = "gpuFogSimpleNoiseRemap";
inline constexpr std::string_view kSimplexNoiseRemap = "gpuFogSimplexNoiseRemap";
inline constexpr std::string_view kVoronoiNoiseRemap = "gpuFogVoronoiNoiseRemap";
inline constexpr std::string_view kEdgeSoftness = "gpuFogEdgeSoftness";

inline constexpr std::string_view kSimpleAnimationX = "gpuFogSimpleAnimationX";
inline constexpr std::string_view kSimpleAnimationY = "gpuFogSimpleAnimationY";
inline constexpr std::string_view kSimpleAnimationZ = "gpuFogSimpleAnimationZ";
inline constexpr std::string_view kSimpleAnimationW = "gpuFogSimpleAnimationW";
inline constexpr std::string_view kSimplexAnimationX = "gpuFogSimplexAnimationX";
inline constexpr std::string_view kSimplexAnimationY = "gpuFogSimplexAnimationY";
inline constexpr std::string_view kSimplexAnimationZ = "gpuFogSimplexAnimationZ";
inline constexpr std::string_view kSimplexAnimationW = "gpuFogSimplexAnimationW";
inline constexpr std::string_view kVoronoiAnimationX = "gpuFogVoronoiAnimationX";
inline constexpr std::string_view kVoronoiAnimationY = "gpuFogVoronoiAnimationY";
inline constexpr std::string_view kVoronoiAnimationZ = "gpuFogVoronoiAnimationZ";
inline constexpr std::string_view kVoronoiAnimationW = "gpuFogVoronoiAnimationW";

inline constexpr std::string_view kSurfaceDepthFade = "gpuFogSurfaceDepthFade";
inline constexpr std::string_view kShapeDistortion = "gpuFogShapeDistortion";
inline constexpr std::string_view kWispyNoiseAmount = "gpuFogWispyNoiseAmount";
inline constexpr std::string_view kDetailNoiseAmount = "gpuFogDetailNoiseAmount";
inline constexpr std::string_view kCameraDepthFadeRange = "gpuFogCameraDepthFadeRange";
inline constexpr std::string_view kCameraDepthFadeOffset = "gpuFogCameraDepthFadeOffset";

MaterialDocument CreateDefaultMaterial(std::string_view name = "GPU Fog Particles");
MaterialDocument CreateLargeFogMaterial();
MaterialDocument CreateSmallFogMaterial();

} // namespace GameEngine::GPUFogParticles
