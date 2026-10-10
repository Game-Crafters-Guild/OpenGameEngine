#include "Rendering/Passes/TemporalDither.h"

#include "Logger/Logger.h"

#include <atomic>
#include <cmath>

namespace GameEngine {
namespace Rendering {
namespace Passes {
namespace {

// Golden-ratio conjugate, phi^-1 = (sqrt(5) - 1) / 2. The additive recurrence
// fract(n * kDitherPhaseIncrement) is the one-dimensional low-discrepancy
// sequence with the best worst-case gap: no run of indices clusters, and no
// index repeats a previous realisation. See the header for why the phase is
// applied past the noise function rather than inside it.
constexpr double kDitherPhaseIncrement = 0.6180339887498949;

std::atomic<bool> g_TemporalMovieDither{false};
std::atomic<bool> g_TemporalScreenDither{false};

} // namespace

float TemporalDitherPhaseForIndex(uint64_t realisationIndex)
{
	const double phase = static_cast<double>(realisationIndex) * kDitherPhaseIncrement;
	return static_cast<float>(phase - std::floor(phase));
}

float MovieDitherPhase(uint64_t movieOrdinal)
{
	return IsTemporalMovieDitherEnabled() ? TemporalDitherPhaseForIndex(movieOrdinal) : 0.0f;
}

float ScreenDitherPhase(uint64_t renderFrameIndex)
{
	return IsTemporalScreenDitherEnabled() ? TemporalDitherPhaseForIndex(renderFrameIndex) : 0.0f;
}

bool IsTemporalMovieDitherEnabled()
{
	return g_TemporalMovieDither.load(std::memory_order_relaxed);
}

void SetTemporalMovieDitherEnabled(bool enabled)
{
	// Logged on transitions only: whether the experiment was actually armed for
	// a given recording is otherwise unanswerable after the fact, and an A/B
	// whose "on" arm was off is a void experiment rather than a null result.
	if (g_TemporalMovieDither.exchange(enabled, std::memory_order_relaxed) != enabled)
		Logger::Log::Info("TemporalDither: movie dither {}", enabled ? "ON" : "OFF");
}

bool IsTemporalScreenDitherEnabled()
{
	return g_TemporalScreenDither.load(std::memory_order_relaxed);
}

void SetTemporalScreenDitherEnabled(bool enabled)
{
	if (g_TemporalScreenDither.exchange(enabled, std::memory_order_relaxed) != enabled)
		Logger::Log::Info("TemporalDither: screen dither {}", enabled ? "ON" : "OFF");
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
