#pragma once

#include "Graph/GraphModel.h"

#include <string>
#include <vector>

namespace GameEngine {

struct BlendSpace2DSampleDesc
{
    float X = 0.f;
    float Y = 0.f;
    std::string Label;
    std::string ClipGuid;
};

struct BlendSpace2DAxis
{
    float Min = 0.f;
    float Max = 1.f;
    static BlendSpace2DAxis FromSamples(const std::vector<BlendSpace2DSampleDesc>& samples, bool useX);
    float ClampedT(float value) const;
};

class GraphBlendSpace2DStore
{
public:
    static constexpr const char* kExtensionKey = "blendSpace2D";
    /** Missing/wrong-type key: leave `out` unchanged, return false. */
    static bool TryLoad(const Graph::Node& host, std::vector<BlendSpace2DSampleDesc>& out);
    static void Store(Graph::Node& host, const std::vector<BlendSpace2DSampleDesc>& samples);
};

} // namespace GameEngine
