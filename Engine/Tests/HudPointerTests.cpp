// The runtime host's HUD pointer mapping: window client pixels to the pixels the HUD
// is laid out at.
//
// These rows pin it against the arithmetic it has to reproduce
// (`mx *= scaleX; my *= scaleY`, from the window's framebuffer-to-client content
// scale) at both scales that ship: 1 where cursor coordinates are already
// physical pixels, 2 on a Retina display.
#include <gtest/gtest.h>

#include "Engine/Hosting/HudPointer.h"

using namespace GameEngine;

namespace
{
// The content-scale arithmetic, written out so the parity is checked against the
// formula rather than against this file's own restatement of it.
void ContentScaleMapping(float clientX, float clientY, float scaleX, float scaleY, float& outX,
                     float& outY)
{
    outX = clientX;
    outY = clientY;
    outX *= scaleX;
    outY *= scaleY;
}
} // namespace

TEST(HudPointerTests, PhysicalPixelCursorsLandOnTheHudUnchanged)
{
    float hudX = 0.0f;
    float hudY = 0.0f;
    MapClientToHudPixels(640.0f, 360.0f, /*contentScaleX=*/1.0f, /*contentScaleY=*/1.0f, hudX, hudY);
    EXPECT_FLOAT_EQ(hudX, 640.0f);
    EXPECT_FLOAT_EQ(hudY, 360.0f);

    float scaledX = 0.0f;
    float scaledY = 0.0f;
    ContentScaleMapping(640.0f, 360.0f, 1.0f, 1.0f, scaledX, scaledY);
    EXPECT_FLOAT_EQ(hudX, scaledX);
    EXPECT_FLOAT_EQ(hudY, scaledY);
}

TEST(HudPointerTests, ARetinaFramebufferScalesTheCursorOntoTheHud)
{
    float hudX = 0.0f;
    float hudY = 0.0f;
    MapClientToHudPixels(640.0f, 360.0f, /*contentScaleX=*/2.0f, /*contentScaleY=*/2.0f, hudX, hudY);
    EXPECT_FLOAT_EQ(hudX, 1280.0f);
    EXPECT_FLOAT_EQ(hudY, 720.0f);

    float scaledX = 0.0f;
    float scaledY = 0.0f;
    ContentScaleMapping(640.0f, 360.0f, 2.0f, 2.0f, scaledX, scaledY);
    EXPECT_FLOAT_EQ(hudX, scaledX) << "the HUD pointer moved off the content-scale mapping";
    EXPECT_FLOAT_EQ(hudY, scaledY);
}

// GetContentScale reports each axis separately (framebuffer/window per axis), so
// the mapping must not fold them into one factor.
TEST(HudPointerTests, EachAxisTakesItsOwnScale)
{
    float hudX = 0.0f;
    float hudY = 0.0f;
    MapClientToHudPixels(100.0f, 100.0f, /*contentScaleX=*/2.0f, /*contentScaleY=*/1.0f, hudX, hudY);
    EXPECT_FLOAT_EQ(hudX, 200.0f);
    EXPECT_FLOAT_EQ(hudY, 100.0f);
}
