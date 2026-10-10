#pragma once

#include <cmath>

namespace GameEngine::Audio
{

// miniaudio defaults to right-handed OpenGL (forward = -Z). This engine is
// left-handed +Z forward. Stereo L/R is decided in the look-at at
// miniaudio.h (~52969): axisX = cross(look, worldUp), then negated when
// listener.config.handedness is ma_handedness_left. We always run left-handed
// so world +X (listener looking along +Z, up +Y) is the right ear.
//
// ma_spatializer.handedness is a different field: it only swaps the emitter's
// default cone direction at spatializer init. Panning reads the listener.
inline constexpr bool kMiniaudioListenerLeftHanded = true;

// Listener-space X of a world-space offset. Mirrors miniaudio's look-at:
//   axisZ = look
//   axisX = cross(axisZ, worldUp)
//   if left-handed: axisX = -axisX
// Positive X is the right ear after the transform.
inline float ListenerSpaceX(float lookX, float lookY, float lookZ,
                            float upX, float upY, float upZ,
                            float offsetX, float offsetY, float offsetZ,
                            bool leftHanded)
{
    float axisX = lookY * upZ - lookZ * upY;
    float axisY = lookZ * upX - lookX * upZ;
    float axisZ = lookX * upY - lookY * upX;
    const float len2 = axisX * axisX + axisY * axisY + axisZ * axisZ;
    if (len2 < 1.0e-12f)
        return offsetX;
    const float inv = 1.0f / std::sqrt(len2);
    axisX *= inv;
    axisY *= inv;
    axisZ *= inv;
    if (leftHanded)
    {
        axisX = -axisX;
        axisY = -axisY;
        axisZ = -axisZ;
    }
    return axisX * offsetX + axisY * offsetY + axisZ * offsetZ;
}

} // namespace GameEngine::Audio
