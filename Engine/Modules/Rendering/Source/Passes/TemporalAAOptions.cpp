#include "Rendering/Passes/TemporalAAOptions.h"

#include <atomic>

namespace GameEngine {
namespace Rendering {
namespace Passes {
namespace {

std::atomic<bool> g_TaaSubpixelCorrection{true};

} // namespace

bool IsTaaSubpixelCorrectionEnabled()
{
    return g_TaaSubpixelCorrection.load(std::memory_order_relaxed);
}

void SetTaaSubpixelCorrectionEnabled(bool enabled)
{
    g_TaaSubpixelCorrection.store(enabled, std::memory_order_relaxed);
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
