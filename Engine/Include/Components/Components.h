#pragma once

// Engine Components
// This header includes all components that are part of the Engine library
// These components are registered by the Engine with the ECS system

#include "Components/Transform.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Animation/AnimatedSprite2D.h"

namespace GameEngine {
namespace Components {
// Aggregator header for Engine components; no redundant type aliases needed.
}
} // namespace GameEngine
