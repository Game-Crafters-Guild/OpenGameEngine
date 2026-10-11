#pragma once

#include "ECS/SystemWaveTrace.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace GameEngine::Editor
{

// What the get_ecs_wave_trace reply describes besides the frames themselves.
struct SystemWaveTraceContext
{
    bool Enabled = false;
    std::uint64_t FramesRecorded = 0;
    // System names by SystemManager slot; a slot past the end reads as "<slot N>".
    std::span<const std::string> SystemNames;
    // How many of the newest frames to describe one by one.
    std::size_t DetailFrames = 0;
};

// The get_ecs_wave_trace reply for recorded frames (oldest first): a summary over every frame
// (the ECS part of the frame, the frame period, the caller's join wait and inline work, forks,
// and where the frame's job publishes came from), one row per plan wave (makespan, the gap
// before it, its join wait, its slowest forked body, the publish-to-start delay of its forks),
// one row per system (its update time and the threads it ran on), and the newest frames in full.
// Times are milliseconds; distributions are {median, p95, max} over the frames that have them.
nlohmann::json BuildSystemWaveTraceReport(std::span<const ECS::SystemWaveTrace::Frame> frames,
                                          const SystemWaveTraceContext& context);

} // namespace GameEngine::Editor
