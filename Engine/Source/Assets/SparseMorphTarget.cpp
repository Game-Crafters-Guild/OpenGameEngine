#include "SparseMorphTarget.h"

#include "Assets/ModelAsset.h"

#include <cassert>
#include <utility>

namespace GameEngine
{

namespace
{

// True when `deltas` is not empty and moves `vertex`.
bool MovesVertex(std::span<const float> deltas, size_t vertex)
{
    if (deltas.empty())
        return false;
    const float* delta = deltas.data() + vertex * 3u;
    return delta[0] != 0.0f || delta[1] != 0.0f || delta[2] != 0.0f;
}

// True when a position, normal or tangent delta of `vertex` has a non-zero component: the rule
// that lists a vertex in the sparse target.
bool TargetMovesVertex(std::span<const float> positionDeltas, std::span<const float> normalDeltas,
                       std::span<const float> tangentDeltas, size_t vertex)
{
    return MovesVertex(positionDeltas, vertex) || MovesVertex(normalDeltas, vertex) ||
           MovesVertex(tangentDeltas, vertex);
}

// Appends the delta of `vertex` from `deltas` to `listed`, or a zero delta when `deltas` is empty.
void AppendDelta(Vector<float>& listed, std::span<const float> deltas, size_t vertex)
{
    if (deltas.empty())
    {
        listed.insert(listed.end(), 3u, 0.0f);
        return;
    }
    const float* delta = deltas.data() + vertex * 3u;
    listed.insert(listed.end(), delta, delta + 3u);
}

} // namespace

MorphTarget MakeSparseMorphTarget(String name, size_t vertexCount, std::span<const float> positionDeltas,
                                  std::span<const float> normalDeltas, std::span<const float> tangentDeltas)
{
    assert(positionDeltas.empty() || positionDeltas.size() == vertexCount * 3u);
    assert(normalDeltas.empty() || normalDeltas.size() == vertexCount * 3u);
    assert(tangentDeltas.empty() || tangentDeltas.size() == vertexCount * 3u);

    size_t listedCount = 0;
    for (size_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        if (TargetMovesVertex(positionDeltas, normalDeltas, tangentDeltas, vertex))
            ++listedCount;
    }

    MorphTarget target;
    target.Name = std::move(name);
    target.VertexIndices.reserve(listedCount);
    target.PositionDeltas.reserve(listedCount * 3u);
    if (!normalDeltas.empty())
        target.NormalDeltas.reserve(listedCount * 3u);
    if (!tangentDeltas.empty())
        target.TangentDeltas.reserve(listedCount * 3u);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        if (!TargetMovesVertex(positionDeltas, normalDeltas, tangentDeltas, vertex))
            continue;
        target.VertexIndices.push_back(static_cast<uint32>(vertex));
        AppendDelta(target.PositionDeltas, positionDeltas, vertex);
        if (!normalDeltas.empty())
            AppendDelta(target.NormalDeltas, normalDeltas, vertex);
        if (!tangentDeltas.empty())
            AppendDelta(target.TangentDeltas, tangentDeltas, vertex);
    }
    return target;
}

} // namespace GameEngine
