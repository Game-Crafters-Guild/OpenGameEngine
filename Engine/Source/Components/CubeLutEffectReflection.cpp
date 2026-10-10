// Manual reflection for CubeLutEffect.
//
// The build-time scanner skips AssetRef<> fields because its lightweight C++
// parser cannot resolve templates. Registering the complete component here
// gives it the same ComponentFactory/default-bytes contract as every other
// effect offered by the Add Post FX picker.

#include "Components/ComponentRegistration.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"

GE_REGISTER_COMPONENT_BEGIN(GameEngine::Components::CubeLutEffect)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, Enabled)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, StackOrder)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, Intensity)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, InputEncoding)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, TextureFormat)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, LutAssetGuid)
GE_REGISTER_COMPONENT_END(GameEngine::Components::CubeLutEffect)
