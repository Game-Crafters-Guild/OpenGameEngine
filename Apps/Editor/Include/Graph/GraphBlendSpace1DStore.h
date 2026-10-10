#pragma once

#include "Graph/GraphModel.h"

#include <string>
#include <vector>

namespace GameEngine {

struct BlendSpace1DSampleDesc
{
    float Position = 0.f;
    std::string Label;
    std::string ClipGuid;
};

struct BlendSpace1DAxis
{
    float Min = 0.f;
    float Max = 1.f;
    static BlendSpace1DAxis FromSamples(const std::vector<BlendSpace1DSampleDesc>& samples);
    float PositionFromX(float x, float trackLeft, float trackWidth) const;
    float XFromPosition(float position, float trackLeft, float trackWidth) const;
    float ClampedT(float value) const;
};

class GraphBlendSpace1DStore
{
public:
    static constexpr const char* kExtensionKey = "blendSpace1D";
    /** Missing/wrong-type key: leave `out` unchanged, return false. */
    static bool TryLoad(const Graph::Node& host, std::vector<BlendSpace1DSampleDesc>& out);
    static void Store(Graph::Node& host, const std::vector<BlendSpace1DSampleDesc>& samples);
};

} // namespace GameEngine
