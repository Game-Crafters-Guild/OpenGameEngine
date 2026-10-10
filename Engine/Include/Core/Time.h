#pragma once

#include <optional>

namespace GameEngine::Time
{

// The engine's canonical per-frame delta time, in seconds. Stamped once per
// frame by the application loop, so any subsystem or user script can query the
// current frame's delta without threading it through call chains. Non-negative.
//
// Reports the deterministic step instead of the measured delta whenever one is
// set (see SetDeterministicStep) — e.g. a fixed 1/60 during editor UI replay so
// golden screenshots are reproducible. A per-frame clock you READ, the same
// model as Unity's Time.deltaTime.
//
// Thread-safety: readable from any thread; refreshed once per frame on the main
// thread before subsystem updates run.
float GetDeltaTime();

// Process-global cumulative animation time, in seconds — the clock animated
// materials derive their uniforms from. Advanced once per app frame: by the
// deterministic step exactly when one is pinned (reproducible under
// SetDeterministicStep for movie-capture / UI replay), else by the measured
// delta capped at a per-frame maximum so a multi-second frame hitch cannot jump
// animation phase (a large single-frame advance makes scrolling UVs appear to
// snap backward). It is therefore a *smoothed* timeline that intentionally lags
// wall-clock across a stall, not raw elapsed time. Double precision keeps it
// drift-free over long sessions.
//
// Because it lives here rather than in any renderer, it is NOT reset when a
// RenderServices instance is destroyed and recreated (device-lost recovery,
// window/context rebuild), so a renderer rebuild no longer snaps every animated
// UV back to t=0. It is the one app timeline shared by all RenderServices
// instances.
//
// Thread-safety: readable from any thread; advanced once per frame on the main
// thread inside the same call that refreshes the per-frame delta.
double GetCumulativeSeconds();

// Force GetDeltaTime() to report a fixed step regardless of real frame timing.
// Pass a value (e.g. 1.0f/60.0f) to pin the cadence for deterministic runs
// (editor UI replay, golden screenshots); pass std::nullopt to resume reporting
// the measured per-frame delta. Tooling/editor use, not gameplay.
void SetDeterministicStep(std::optional<float> stepSeconds);

namespace Detail
{
// Engine-internal: the application loop stamps the measured per-frame delta once
// per frame (negative inputs are clamped to zero). Gameplay/user code should read
// GetDeltaTime() instead of calling this.
void SetFrameDeltaTime(float deltaSeconds);
} // namespace Detail

} // namespace GameEngine::Time
