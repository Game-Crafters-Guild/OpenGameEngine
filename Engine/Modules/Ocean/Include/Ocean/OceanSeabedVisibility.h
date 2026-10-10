#pragma once

#include "Ocean/OceanTypes.h"

namespace GameEngine::Ocean
{

// The fraction of the seabed's colour, in every channel, at or below which the seabed reads as
// deep water through the surface's depth fog.
inline constexpr float32 kOceanSeabedVisibleTransmittance = 0.02f;
// The deepest result: no fog setting the inspector allows shows the seabed past this depth (m).
inline constexpr float32 kOceanSeabedVisibleDepthMaxM = 100.0f;

// How deep (m, vertically below the calm surface) the seabed stays visible through this ocean seen
// from above: a seabed at least this deep shows at most kOceanSeabedVisibleTransmittance of its
// colour in every channel, whatever the view angle. A view ray crosses at least the vertical depth
// of water, so bounding the vertical depth is the conservative side. The largest of three bounds:
// - the density: Beer-Lambert on the fog's least-absorbed channel;
// - an authored fog end distance (the Linear / Smooth falloffs; 0 = auto), which can only hide the
//   seabed sooner than the density does, while a longer one cannot show it past the density's
//   reach;
// - the shallow clarity window (OceanShallowFogScale in ocean_common.glsl): over the first
//   ShallowClarityDistance meters of path the fog is scaled down, so there the density alone
//   overstates how fast the seabed fades. Past that distance the scale is 1 and the density bound
//   holds again, so a seabed at least that deep is covered. A floor of 1 turns the window off.
// Not modelled: DepthFogStartDistance and a falloff power other than 1, which also delay the fog.
// The CBT terrain classifier coarsens seabed deeper than this as hidden ground.
float32 OceanSeabedVisibleDepthM(const OceanParamsGPU& params);

} // namespace GameEngine::Ocean
