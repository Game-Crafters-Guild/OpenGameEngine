#include "Placement/SplineRebuildGate.h"

#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <cstring>

namespace GameEngine::Editor
{

bool SplineObservedInputs::operator==(const SplineObservedInputs& other) const
{
    return Valid == other.Valid && SplineVersion == other.SplineVersion &&
           DataIndex == other.DataIndex && DataGeneration == other.DataGeneration &&
           std::memcmp(WorldMatrix, other.WorldMatrix, sizeof(WorldMatrix)) == 0;
}

SplineObservedInputs ObserveSpline(const SplineECS::SplineService& splineService,
                                   uint32 dataIndex,
                                   uint32 dataGeneration,
                                   const float32 (&worldMatrix)[16])
{
    SplineObservedInputs inputs;
    if (const auto* data = splineService.GetSplineData(SplineECS::SplineHandle(dataIndex, dataGeneration)))
    {
        inputs.SplineVersion = data->Version;
        inputs.Valid = data->IsValid();
    }
    inputs.DataIndex = dataIndex;
    inputs.DataGeneration = dataGeneration;
    std::memcpy(inputs.WorldMatrix, worldMatrix, sizeof(inputs.WorldMatrix));
    return inputs;
}

} // namespace GameEngine::Editor
