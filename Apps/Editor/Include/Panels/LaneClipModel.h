#pragma once

#include "AssetCore/GUID.h"
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{

// Default 0xAARRGGBB row tint for clip-editor lanes (#373737).
inline constexpr uint32_t kDefaultLaneRowColorArgb = 0xFF373737u;

// A named time marker; used on lanes (absolute time) and clip instances (relative to instance start).
struct LaneMarker
{
    float time = 0.0f;
    std::string name;
};

// Editor-only: a clip instance on a lane (reference + start time, scale, loop).
struct LaneClipInstance
{
    GUID clipGuid;
    std::string name;
    float startTimeOnLane = 0.0f;
    float scale = 1.0f;
    float sourceDuration = 1.0f;
    int loopCount = 1;
    bool muted = false;
    // Cross-fade ramps. fadeInDuration: gain ramps 0→1 over this many seconds at clip start;
    // fadeOutDuration: gain ramps 1→0 over this many seconds at clip end. Overlapping clips with
    // matching out/in fades produce a cross-fade.
    float fadeInDuration = 0.0f;
    float fadeOutDuration = 0.0f;
    std::vector<LaneMarker> markers; // times relative to instance start (0 = instance start)
};

// Editor-only: a lane containing clip instances.
struct LaneClipLane
{
    std::string name;
    std::vector<LaneClipInstance> clips;
    std::vector<LaneMarker> markers; // absolute times on the lane timeline
    // 0xAARRGGBB row tint. Defaults to a neutral dark gray that matches the
    // editor's normal lane row background; users override via the sidebar swatch.
    uint32_t color = kDefaultLaneRowColorArgb;
};

// Editor-only: lane-based clip editor model.
struct LaneClipModel
{
    std::vector<LaneClipLane> lanes;
};

// JSON serialization for the .clipset asset format.
nlohmann::json SerializeLaneClipModel(const LaneClipModel& model);
bool DeserializeLaneClipModel(const nlohmann::json& doc, LaneClipModel& outModel);

} // namespace GameEngine
