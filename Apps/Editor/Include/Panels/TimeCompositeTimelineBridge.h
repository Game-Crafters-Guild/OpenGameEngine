#pragma once

#include "Animation/AnimationTimeline.h"
#include "Panels/TimeCompositeModel.h"

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Editor
{

void BuildRuntimeTimelineFromCompositeModel(const TimeCompositeModel& model, Animation::Timeline& outTimeline);

void ApplyCompositeModelFromRuntimeTimeline(const Animation::Timeline& timeline, TimeCompositeModel& outModel);

nlohmann::json SaveTimelineDocumentFromCompositeModel(const TimeCompositeModel& model);

bool LoadCompositeModelFromTimelineDocument(const nlohmann::json& doc, TimeCompositeModel& outModel, std::string* outError = nullptr);

} // namespace GameEngine::Editor
