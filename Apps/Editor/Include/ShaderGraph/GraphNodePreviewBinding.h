#pragma once

#include <string>

namespace GameEngine::Editor
{

/// What a material-graph node's preview thumb needs to paint itself: the engine
/// texture name of the kind controller's preview atlas plus the CSS sprite
/// addressing that selects this node's cell. An empty Resource means "no preview".
struct GraphNodePreviewBinding
{
    std::string Resource;
    float SizeXPercent = 100.f;
    float SizeYPercent = 100.f;
    float PosXPercent = 0.f;
    float PosYPercent = 0.f;
};

} // namespace GameEngine::Editor
