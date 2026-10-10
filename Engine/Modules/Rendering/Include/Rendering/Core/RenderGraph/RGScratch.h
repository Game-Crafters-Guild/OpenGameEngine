#pragma once

// Frame-scratch helper: size a vector<vector> for n entries clearing each inner
// vector INDIVIDUALLY. Calling outer clear() would destroy the inner vectors and
// free their capacity — the whole point of member scratch is that steady-state
// frames reuse capacity with zero heap allocations.

#include <cstddef>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

template <class T>
inline void PrepareNested(std::vector<std::vector<T>>& v, size_t n)
{
    if (v.size() < n)
        v.resize(n);
    for (size_t i = 0; i < n; ++i)
        v[i].clear();
}

} // namespace GameEngine::Rendering::RenderGraph
