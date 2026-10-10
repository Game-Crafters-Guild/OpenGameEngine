#pragma once

namespace GameEngine
{
// Registers the asset inspector for Texture assets: shows dimensions and a
// Color Space override (Auto / sRGB / Linear) persisted to the texture's .meta.
void RegisterTextureInspector();
}
