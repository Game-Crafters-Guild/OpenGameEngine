#pragma once

namespace GameEngine
{
/// Window client pixels to the pixels the HUD is laid out at.
///
/// The HUD composites onto the frame's colour target, which the runtime host sizes to
/// the window's framebuffer, so the ratio between the two spaces is the window's
/// framebuffer-to-client content scale: 1 where cursor coordinates are already
/// physical pixels (Windows), 2 on a Retina display, the device pixel ratio in a
/// browser. Feeding client pixels straight through would put every hit test a
/// factor of that scale away from the control the player sees.
void MapClientToHudPixels(float clientX, float clientY, float contentScaleX, float contentScaleY,
                          float& hudX, float& hudY);
} // namespace GameEngine
