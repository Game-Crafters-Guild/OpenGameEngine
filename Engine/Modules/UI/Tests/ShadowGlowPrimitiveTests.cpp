#include <gtest/gtest.h>
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"

using namespace GameEngine;
using namespace GameEngine::UI;

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_NoEffects_NoChange)
{
    UIPrimitive p = MakeRect(100, 50, 200, 80, PackColor(1, 1, 1, 1));

    ExpandForEffects(p);

    EXPECT_FLOAT_EQ(p.X, 100.0f);
    EXPECT_FLOAT_EQ(p.Y, 50.0f);
    EXPECT_FLOAT_EQ(p.W, 200.0f);
    EXPECT_FLOAT_EQ(p.H, 80.0f);
}

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_Shadow_ExpandsQuad)
{
    UIPrimitive p = MakeRect(100, 50, 200, 80, PackColor(1, 1, 1, 1));
    AddShadow(p, 4.0f, 6.0f, 10.0f, PackColor(0, 0, 0, 0.5f));

    float origX = p.X, origY = p.Y, origW = p.W, origH = p.H;
    ExpandForEffects(p);

    EXPECT_LT(p.X, origX);
    EXPECT_LT(p.Y, origY);
    EXPECT_GT(p.W, origW);
    EXPECT_GT(p.H, origH);

    // Padding in uvRect reconstructs the original element rect
    EXPECT_FLOAT_EQ(p.X + p.UvRect[0], origX);
    EXPECT_FLOAT_EQ(p.Y + p.UvRect[1], origY);
    EXPECT_FLOAT_EQ(p.W - p.UvRect[0] - p.UvRect[2], origW);
    EXPECT_FLOAT_EQ(p.H - p.UvRect[1] - p.UvRect[3], origH);
}

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_Glow_ExpandsQuad)
{
    UIPrimitive p = MakeRect(50, 50, 100, 100, PackColor(1, 1, 1, 1));
    AddGlow(p, 12.0f, PackColor(0.2f, 0.4f, 1.0f, 0.8f));

    float origX = p.X, origY = p.Y, origW = p.W, origH = p.H;
    ExpandForEffects(p);

    // Expanded by at least glowRadius on each side
    EXPECT_LE(p.X, origX - 12.0f);
    EXPECT_LE(p.Y, origY - 12.0f);
    EXPECT_GE(p.X + p.W, origX + origW + 12.0f);
    EXPECT_GE(p.Y + p.H, origY + origH + 12.0f);

    // Padding in uvRect reconstructs the original element rect
    EXPECT_FLOAT_EQ(p.X + p.UvRect[0], origX);
    EXPECT_FLOAT_EQ(p.Y + p.UvRect[1], origY);
    EXPECT_FLOAT_EQ(p.W - p.UvRect[0] - p.UvRect[2], origW);
    EXPECT_FLOAT_EQ(p.H - p.UvRect[1] - p.UvRect[3], origH);
}

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_ShadowOffset_AsymmetricExpansion)
{
    UIPrimitive p = MakeRect(100, 100, 60, 40, PackColor(1, 1, 1, 1));
    // Shadow offset to the right and down: need more padding on right/bottom
    AddShadow(p, 10.0f, 10.0f, 5.0f, PackColor(0, 0, 0, 1));

    ExpandForEffects(p);

    // Right edge must extend further than left edge (due to positive offset)
    float rightExtent = (p.X + p.W) - (100.0f + 60.0f);
    float leftExtent = 100.0f - p.X;
    EXPECT_GT(rightExtent, leftExtent);

    // Bottom edge must extend further than top edge
    float bottomExtent = (p.Y + p.H) - (100.0f + 40.0f);
    float topExtent = 100.0f - p.Y;
    EXPECT_GT(bottomExtent, topExtent);
}

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_NegativeOffset_ExpandsOppositeDirection)
{
    UIPrimitive p = MakeRect(100, 100, 60, 40, PackColor(1, 1, 1, 1));
    // Shadow offset to the left and up
    AddShadow(p, -8.0f, -6.0f, 5.0f, PackColor(0, 0, 0, 1));

    ExpandForEffects(p);

    // Left edge must extend further than right edge (negative X offset)
    float leftExtent = 100.0f - p.X;
    float rightExtent = (p.X + p.W) - 160.0f;
    EXPECT_GT(leftExtent, rightExtent);
}

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_CombinedShadowAndGlow)
{
    UIPrimitive p = MakeRect(0, 0, 100, 50, PackColor(1, 1, 1, 1));
    AddShadow(p, 2.0f, 4.0f, 8.0f, PackColor(0, 0, 0, 0.6f));
    AddGlow(p, 15.0f, PackColor(0.5f, 0.5f, 1.0f, 0.4f));

    ExpandForEffects(p);

    // Must cover both shadow extent and glow radius
    EXPECT_LE(p.X, -15.0f);
    EXPECT_LE(p.Y, -15.0f);
    EXPECT_GE(p.X + p.W, 100.0f + 15.0f);
    EXPECT_GE(p.Y + p.H, 50.0f + 15.0f);
}

TEST(ShadowGlowPrimitiveTests, TexturedQuadDoesNotEnableRoundedClipByDefault)
{
    UIPrimitive p = MakeTexturedQuad(10.0f, 20.0f, 80.0f, 60.0f, 7u);

    for (float value : p.Radii)
        EXPECT_FLOAT_EQ(value, 0.0f);
    for (float value : p.RadiiY)
        EXPECT_FLOAT_EQ(value, 0.0f);
    for (float value : p.BorderWidths)
        EXPECT_FLOAT_EQ(value, 0.0f);
}

// The clip corners are ellipses: each takes a horizontal then a vertical
// semi-axis, and the two are deliberately unequal here so a call that dropped or
// duplicated one axis cannot pass.
TEST(ShadowGlowPrimitiveTests, SetTextureRoundedClipStoresPaintBoxAndRadii)
{
    UIPrimitive p = MakeTexturedQuad(5.0f, 10.0f, 120.0f, 90.0f, 7u);

    SetTextureRoundedClip(p, 10.0f, 15.0f, 100.0f, 70.0f,
                          2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f);

    EXPECT_FLOAT_EQ(p.BorderWidths[0], 10.0f);
    EXPECT_FLOAT_EQ(p.BorderWidths[1], 15.0f);
    EXPECT_FLOAT_EQ(p.BorderWidths[2], 100.0f);
    EXPECT_FLOAT_EQ(p.BorderWidths[3], 70.0f);
    EXPECT_FLOAT_EQ(p.Radii[0], 2.0f);
    EXPECT_FLOAT_EQ(p.RadiiY[0], 3.0f);
    EXPECT_FLOAT_EQ(p.Radii[1], 4.0f);
    EXPECT_FLOAT_EQ(p.RadiiY[1], 5.0f);
    EXPECT_FLOAT_EQ(p.Radii[2], 6.0f);
    EXPECT_FLOAT_EQ(p.RadiiY[2], 7.0f);
    EXPECT_FLOAT_EQ(p.Radii[3], 8.0f);
    EXPECT_FLOAT_EQ(p.RadiiY[3], 9.0f);
}

TEST(ShadowGlowPrimitiveTests, ExpandForEffects_InsetShadow_DoesNotExpandQuad)
{
    UIPrimitive p = MakeRect(100, 50, 200, 80, PackColor(1, 1, 1, 1));
    AddShadow(p, 0.0f, 1.0f, 2.0f, PackColor(0, 0, 0, 0.35f));
    p.ModeAndFlags |= kPrimInsetShadowBit;

    ExpandForEffects(p);

    EXPECT_FLOAT_EQ(p.X, 100.0f);
    EXPECT_FLOAT_EQ(p.Y, 50.0f);
    EXPECT_FLOAT_EQ(p.W, 200.0f);
    EXPECT_FLOAT_EQ(p.H, 80.0f);
    EXPECT_FLOAT_EQ(p.UvRect[0], 0.0f);
    EXPECT_FLOAT_EQ(p.UvRect[1], 0.0f);
    EXPECT_FLOAT_EQ(p.UvRect[2], 0.0f);
    EXPECT_FLOAT_EQ(p.UvRect[3], 0.0f);
}

TEST(ShadowGlowPrimitiveTests, BoxShadowValueStruct_DefaultsToZero)
{
    BoxShadowValue sv{};
    EXPECT_FLOAT_EQ(sv.OffsetX, 0.0f);
    EXPECT_FLOAT_EQ(sv.OffsetY, 0.0f);
    EXPECT_FLOAT_EQ(sv.Blur, 0.0f);
    EXPECT_EQ(sv.Color, 0x00000000u);
    EXPECT_FALSE(sv.Inset);
}

TEST(ShadowGlowPrimitiveTests, GlowValueStruct_DefaultsToZero)
{
    GlowValue gv{};
    EXPECT_FLOAT_EQ(gv.Radius, 0.0f);
    EXPECT_EQ(gv.Color, 0x00000000u);
}
