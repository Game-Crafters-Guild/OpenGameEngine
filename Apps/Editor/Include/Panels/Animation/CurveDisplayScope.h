#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace GameEngine
{
struct PinnedCurve
{
    int Channel = -1;
    uint32_t Component = 0;
    bool operator==(const PinnedCurve&) const = default;
};

inline bool IsCurvePinned(const std::vector<PinnedCurve>& pins, int channel, uint32_t component)
{
    return std::find(pins.begin(), pins.end(), PinnedCurve{channel, component}) != pins.end();
}

// Pins extend drawing only. They must never extend the channel list used for
// key selection, hit testing, retiming, drawing, or destructive curve tools.
inline void AppendPinnedChannels(std::vector<int>& selected, const std::vector<PinnedCurve>& pins)
{
    for (const auto& pin : pins)
        if (pin.Channel >= 0 && std::find(selected.begin(), selected.end(), pin.Channel) == selected.end())
            selected.push_back(pin.Channel);
}
} // namespace GameEngine
