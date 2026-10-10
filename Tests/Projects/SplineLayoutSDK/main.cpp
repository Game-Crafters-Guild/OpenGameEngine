#include "SplineLayout/FenceLayout.h"
#include <cstdio>

int main()
{
    using namespace GameEngine;
    using V3 = Mathematics::Vector3;
    const std::vector<SplineLayout::CenterSample> center = {
        {V3(0, 0, 0), V3(0, 1, 0)}, {V3(0, 0, 6), V3(0, 1, 0)}};
    const float32 boundaries[] = {0, 1};
    const SplineLayout::FencePieceBounds pieces[] = {{V3(0, 1, 0), V3(0.1f, 1, 1.5f)}};
    SplineLayout::FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = pieces;
    params.PostPitch = 3;
    const auto layout = SplineLayout::BuildFenceLayout(center, params);
    if (layout.Spans.size() != 2 || layout.Stations.size() != 3 || !layout.Validation.empty())
        return 1;
    std::puts("PASS: staged runtime SDK creates a fence layout without Editor");
}
