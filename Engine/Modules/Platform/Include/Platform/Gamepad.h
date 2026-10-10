#pragma once

namespace GameEngine
{
namespace Platform
{

// Layout of a mapped gamepad: the standard Xbox-style controller the windowing
// layer's mapping database normalizes every physical device to (six axes in
// left-X/left-Y/right-X/right-Y/LT/RT order, fifteen buttons A..DpadLeft).
constexpr int kMappedGamepadAxisCount = 6;
constexpr int kMappedGamepadButtonCount = 15;

/**
 * @brief Reads joystick `jid` through the windowing layer's gamepad-mapping
 * database, so axes and buttons arrive in the mapped layout above regardless
 * of the physical device. `axes` must hold kMappedGamepadAxisCount floats and
 * `buttons` kMappedGamepadButtonCount bytes; on success both counts are set to
 * those sizes.
 *
 * Returns false when the joystick has no mapping entry, or on platforms whose
 * windowing layer has no mapping API at all (web). Callers then fall back to
 * raw joystick polling.
 */
bool ReadMappedGamepadState(int jid,
                            float* axes,
                            unsigned char* buttons,
                            int& axisCount,
                            int& buttonCount);

} // namespace Platform
} // namespace GameEngine
