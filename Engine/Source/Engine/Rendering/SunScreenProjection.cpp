#include "Engine/Rendering/SunScreenProjection.h"

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

bool ProjectToNdc(const float* viewProj, float x, float y, float z, float& outNdcX,
                  float& outNdcY, float& outNdcDepth)
{
    const float cx = viewProj[0] * x + viewProj[4] * y + viewProj[8] * z + viewProj[12];
    const float cy = viewProj[1] * x + viewProj[5] * y + viewProj[9] * z + viewProj[13];
    const float cz = viewProj[2] * x + viewProj[6] * y + viewProj[10] * z + viewProj[14];
    const float cw = viewProj[3] * x + viewProj[7] * y + viewProj[11] * z + viewProj[15];
    if (cw <= 1e-5f)
        return false;
    outNdcX = cx / cw;
    outNdcY = cy / cw;
    outNdcDepth = cz / cw;
    return true;
}

bool ProjectSunDirectionToNdc(const CameraData& cam, const float direction[3], float& outNdcX,
                              float& outNdcY, float& outNdcDepth)
{
    if (cam.cameraPos[3] < 0.5f)
    {
        const float cx = cam.viewProj[0] * direction[0] + cam.viewProj[4] * direction[1] +
                         cam.viewProj[8] * direction[2];
        const float cy = cam.viewProj[1] * direction[0] + cam.viewProj[5] * direction[1] +
                         cam.viewProj[9] * direction[2];
        const float cw = cam.viewProj[3] * direction[0] + cam.viewProj[7] * direction[1] +
                         cam.viewProj[11] * direction[2];
        if (cw <= 1e-5f)
            return false;

        outNdcX = cx / cw;
        outNdcY = cy / cw;
    }
    else
    {
        constexpr float kOrthoSunDistance = 1000.0f;
        if (!ProjectToNdc(cam.viewProj, cam.cameraPos[0] + direction[0] * kOrthoSunDistance,
                          cam.cameraPos[1] + direction[1] * kOrthoSunDistance,
                          cam.cameraPos[2] + direction[2] * kOrthoSunDistance, outNdcX, outNdcY,
                          outNdcDepth))
        {
            return false;
        }
    }

    outNdcDepth = 0.0f;
    return true;
}

} // namespace GameEngine::Engine::Renderer
