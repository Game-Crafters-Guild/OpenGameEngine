#pragma once

#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"

namespace GameEngine::Components {

void ApplyHeightFogPreset(HeightFogEffect& effect, HeightFogPreset preset);

float ComputeHeightFogNightBlend(float timeOfDayHours);

void ApplyHeightFogTimeOfDay(HeightFogEffect& effect, float timeOfDayHours);
void ApplyHeightFogTimeOfDay(HeightFogEffect& effect, float timeOfDayHours, const float dayKeyTimesHours[4]);

void LerpHeightFogEffect(const HeightFogEffect& a, const HeightFogEffect& b, float t, HeightFogEffect& out);

} // namespace GameEngine::Components
