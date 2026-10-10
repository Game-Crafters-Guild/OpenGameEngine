// SkyRenderer::CameraState is unused by the live sky path
// (SkyRenderNode::ExtractCameraVectors copies view row 2). Its default is still
// the look a future SetCamera caller would inherit — pin LH +Z so it cannot
// silently go back to the right-handed (0,0,-1) default.

#include <gtest/gtest.h>

#include "Rendering/Sky/SkyRenderer.h"

TEST(SkyRendererCameraState, DefaultForwardIsPlusZ)
{
    const GameEngine::Rendering::SkyRenderer::CameraState s{};
    EXPECT_FLOAT_EQ(s.forward[0], 0.0f);
    EXPECT_FLOAT_EQ(s.forward[1], 0.0f);
    EXPECT_FLOAT_EQ(s.forward[2], 1.0f);
}
