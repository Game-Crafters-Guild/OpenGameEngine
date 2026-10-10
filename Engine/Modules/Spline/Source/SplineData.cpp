#include "Spline/SplineData.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Spline
{

namespace
{

// Centered-difference tangent scaled to a cubic Bezier handle: (next - prev)/2
// is the Catmull-Rom tangent, and a Bezier handle is a third of the tangent.
constexpr float32 kAutoBezierTangentScale = 1.0f / 6.0f;

Mathematics::Vector3 CenteredDiffTangent(const std::vector<SplineFrame>& frames, size_t i)
{
    const size_t n = frames.size();
    if (n < 2)
        return Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    const Mathematics::Vector3& prev = frames[i > 0 ? i - 1 : 0].Position;
    const Mathematics::Vector3& next = frames[i + 1 < n ? i + 1 : n - 1].Position;
    return (next - prev) * 0.5f;
}

} // namespace

uint32 SplineData::GetSegmentCount() const
{
    if (Points.size() < 2)
        return 0;
    return IsEffectivelyClosed()
        ? static_cast<uint32>(Points.size())
        : static_cast<uint32>(Points.size() - 1);
}

void SplineData::ReplacePointsFromFrames(const std::vector<SplineFrame>& frames)
{
    Points.clear();
    Points.reserve(frames.size());
    for (const SplineFrame& frame : frames)
    {
        SplineControlPoint p{};
        p.Position = frame.Position;
        p.Radius = frame.Radius;
        p.Roll = frame.Roll;
        p.Rotation = frame.Rotation;
        p.Scale = frame.Scale;
        Points.push_back(p);
    }

    if (Type == SplineType::CubicBezier)
    {
        for (size_t i = 0; i < Points.size(); ++i)
        {
            const Mathematics::Vector3 tangent =
                CenteredDiffTangent(frames, i) * kAutoBezierTangentScale;
            Points[i].TangentIn = tangent * -1.0f;
            Points[i].TangentOut = tangent;
        }
    }

    MarkDirty();
}

void SplineData::AddPoint(const Mathematics::Vector3& position, float32 radius)
{
    SplineControlPoint p{};
    p.Position = position;
    p.Radius = radius;
    Points.push_back(p);
    MarkDirty();
}

void SplineData::InsertPoint(uint32 index, const Mathematics::Vector3& position, float32 radius)
{
    if (index > static_cast<uint32>(Points.size()))
        index = static_cast<uint32>(Points.size());

    SplineControlPoint p{};
    p.Position = position;
    p.Radius = radius;
    Points.insert(Points.begin() + index, p);
    MarkDirty();
}

bool SplineData::RemovePoint(uint32 index)
{
    if (index >= static_cast<uint32>(Points.size()))
        return false;
    if (Points.size() <= kMinPointCount)
        return false;
    Points.erase(Points.begin() + index);
    MarkDirty();
    return true;
}

void SplineData::SetPointPosition(uint32 index, const Mathematics::Vector3& position)
{
    if (index >= static_cast<uint32>(Points.size()))
        return;
    Points[index].Position = position;
    MarkDirty();
}

void SplineData::SetPointRadius(uint32 index, float32 radius)
{
    if (index >= static_cast<uint32>(Points.size()))
        return;
    Points[index].Radius = std::max(0.0f, radius);
    MarkDirty();
}

void SplineData::SetPointRoll(uint32 index, float32 rollRadians)
{
    if (index >= static_cast<uint32>(Points.size()))
        return;
    Points[index].Roll = rollRadians;
    MarkDirty();
}

void SplineData::SetPointRotation(uint32 index, const Mathematics::Vector3& rotationDegrees)
{
    if (index >= static_cast<uint32>(Points.size()))
        return;
    Points[index].Rotation = rotationDegrees;
    MarkDirty();
}

void SplineData::SetPointScale(uint32 index, const Mathematics::Vector3& scale)
{
    if (index >= static_cast<uint32>(Points.size()))
        return;
    Points[index].Scale = scale;
    MarkDirty();
}

void SplineData::SetPointTangents(uint32 index,
                                   const Mathematics::Vector3& tangentIn,
                                   const Mathematics::Vector3& tangentOut)
{
    if (index >= static_cast<uint32>(Points.size()))
        return;
    Points[index].TangentIn = tangentIn;
    Points[index].TangentOut = tangentOut;
    MarkDirty();
}

} // namespace GameEngine::Spline
