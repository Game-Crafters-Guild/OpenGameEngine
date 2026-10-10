#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraPixelGrid.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

TEST(PixelPerfectCamera, OrthoDisplayGridTracksCompensatedRasterAcrossPanning)
{
    CameraData camera{};
    for (int i = 0; i < 16; i += 5) camera.view[i] = camera.proj[i] = 1;
    camera.proj[0] = camera.proj[5] = 0.02f;
    constexpr uint32_t width = 161, height = 91;
    for (float pan : {-1.25f, -0.35f, 0.0f, 0.25f, 0.4f, 1.75f})
    {
        camera.view[12] = pan;
        camera.view[13] = -pan;
        std::array<float, 4> uv{};
        const auto snapped = StabilizeCameraToPixelGrid(camera, width, height, 10, uv);
        const std::array<float, 4> grid{1.0f / width, 1.0f / height, -uv[2] / uv[0], -uv[3] / uv[1]};
        for (int axis = 0; axis < 2; ++axis)
        {
            const float size = float(axis == 0 ? width : height);
            // The texel is a sum of order-one float terms scaled by (size + 2), so its rounding error is a
            // few float epsilons of that scale (at most 1.6 measured at 161 px, constant across widths).
            const float tolerance = 4.0f * std::numeric_limits<float>::epsilon() * (size + 2);
            for (float screen : {0.0f, 0.001f, 0.37f, 0.999f, 1.0f})
            {
                const float cell = std::floor((screen - grid[axis + 2]) / grid[axis]);
                const float center = (cell + 0.5f) * grid[axis] + grid[axis + 2];
                const float texel = (center * uv[axis] + 0.5f * (1 - uv[axis]) + uv[axis + 2]) * (size + 2);
                EXPECT_NEAR(texel, cell + 1.5f, tolerance);
            }
        }
        const float rasterU = snapped.viewProj[12] * 0.5f + 0.5f;
        const float displayU = (rasterU - 0.5f * (1 - uv[0]) - uv[2]) / uv[0];
        const float cell = std::floor((displayU - grid[2]) / grid[0]);
        const float displayCenter = (cell + 0.5f) * grid[0] + grid[2];
        const float worldSample = ((displayCenter - 0.5f) * 2.0f) / camera.proj[0] - camera.view[12];
        EXPECT_NEAR(worldSample, 0.0f, 2e-5f);
    }
}

TEST(PixelPerfectCamera, ActiveCameraSnapsInScreenAxesAndPreservesTransform)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Test");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Test", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = true;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    camera.params.PixelPerfectReferenceWidth = 161;
    camera.params.PixelPerfectReferenceHeight = 91;
    for (float angle : {0.0f, 0.7f, 1.5707963f})
    {
        auto* matrix = camera.worldTransform.Data();
        matrix[0] = matrix[5] = std::cos(angle);
        matrix[1] = std::sin(angle);
        matrix[4] = -std::sin(angle);
        for (float pan : {-0.17f, -0.04f, 0.0f, 0.04f, 0.17f})
        {
            matrix[12] = pan;
            matrix[13] = -pan;
            ApplyActiveCameraAspect(services, viewId, cameraId, camera, 483, 273);
            const auto* actual = services.Views().FindCameraData(cameraId);
            ASSERT_NE(actual, nullptr);
            const auto state = services.Views().GetViewPixelPerfect(viewId);
            ASSERT_TRUE(state.Active);
            const auto view = camera.ComputeViewMatrix();
            for (int axis = 0; axis < 2; ++axis)
            {
                const float original = view.Data()[12 + axis];
                const float snapped = std::round(original * 10.0f) / 10.0f;
                EXPECT_NEAR(actual->viewProj[12 + axis], snapped * actual->proj[axis * 5], 1e-5f);
                const float fraction = axis == 0 ? state.FracX : state.FracY;
                EXPECT_NEAR(fraction, (snapped - original) * 10.0f, 1e-5f);
            }
            EXPECT_FLOAT_EQ(matrix[12], pan);
            EXPECT_FLOAT_EQ(matrix[13], -pan);
        }
    }
}

TEST(PixelPerfectCamera, DisabledPixelSnapPreservesProjectionAndZeroCompensation)
{
    RenderServices services;
    const auto cameraId = services.Views().AllocateCamera("PixelPerfect.Disabled");
    const auto viewId = services.Views().AllocateView("PixelPerfect.Disabled", cameraId);
    Engine::Renderer::Camera camera;
    camera.params.Perspective = false;
    camera.params.PixelPerfect = true;
    camera.params.PixelPerfectPixelSnap = false;
    camera.params.PixelPerfectPixelsPerUnit = 10;
    camera.worldTransform.Data()[12] = 0.037f;
    ApplyActiveCameraAspect(services, viewId, cameraId, camera, 640, 360);
    const auto state = services.Views().GetViewPixelPerfect(viewId);
    const auto expected = camera.ToCameraData(float(state.PaddedWidth) / state.PaddedHeight,
                                               float(state.PaddedHeight) / 10.0f);
    const auto* actual = services.Views().FindCameraData(cameraId);
    ASSERT_NE(actual, nullptr);
    EXPECT_FLOAT_EQ(state.FracX, 0.0f);
    EXPECT_FLOAT_EQ(state.FracY, 0.0f);
    for (int element = 0; element < 16; ++element)
        EXPECT_FLOAT_EQ(actual->viewProj[element], expected.viewProj[element]);
}
