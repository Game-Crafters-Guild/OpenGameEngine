#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Pass-ordering phases. The graph schedules by dependency order first; Phase is
// the tiebreak between otherwise-unordered passes. AddPass / SetPassPhase take a
// raw int32_t, so these are plain constants — the 100-value gaps leave room for
// user-defined intermediate phases (e.g. 250 between PostProcess and Overlay).
namespace PassPhase
{
constexpr int32_t kEarlySetup  = 0;
constexpr int32_t kSkyRender   = 50;
constexpr int32_t kWorldRender = 100;
constexpr int32_t kPostProcess = 200;
constexpr int32_t kOverlay     = 300;
constexpr int32_t kUI          = 400;
constexpr int32_t kFinalize    = 500;
constexpr int32_t kPresent     = 600;
constexpr int32_t kDefault     = kWorldRender;
} // namespace PassPhase

} // namespace Rendering
} // namespace GameEngine
