#include "Engine/Hosting/HudPointer.h"

namespace GameEngine
{
void MapClientToHudPixels(float clientX, float clientY, float contentScaleX, float contentScaleY,
                          float& hudX, float& hudY)
{
    hudX = clientX * contentScaleX;
    hudY = clientY * contentScaleY;
}
} // namespace GameEngine
