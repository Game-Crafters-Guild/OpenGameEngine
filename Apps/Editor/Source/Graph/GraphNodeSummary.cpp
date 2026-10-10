#include "Graph/GraphNodeSummary.h"

namespace GameEngine {

bool IsTextureValueNode(const Graph::Node& node)
{
    return node.TypeId == "SampleTexture" ||
           node.TypeId == "SampleTexture2D" ||
           node.TypeId == "SampleNormal" ||
           node.TypeId == "SampleTextureArray" ||
           node.TypeId == "SampleCubemap" ||
           node.TypeId == "TriplanarTexture" ||
           node.TypeId == "CurveTexture";
}

} // namespace GameEngine
