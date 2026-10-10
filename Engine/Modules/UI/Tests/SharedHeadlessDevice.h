#pragma once

// The headless device a UI test executable's tests share (SharedHeadlessDevice.cpp).
//
// The device frees a released buffer or texture only at its next BeginFrame or
// WaitForIdle, and most UI tests call neither. Every UIManager holds about 113 MB of SDF
// ring buffers, so the releases of a loop that builds one fixture per case stay
// allocated, as wired GPU memory on a unified-memory Mac, until something retires them.
// Two points do: every test's end (a gtest listener), and the destruction of every
// fixture that builds a UIManager on this device (SharedDeviceReleaseRetirement).

namespace GameEngine::Rendering
{
class IDevice;
}

// The executable's shared headless device: created on first use, shut down after the
// last test, owned by neither caller. Tests that assert nothing about the device itself
// use it; creating a device per test was most of their run time. Null when no Vulkan
// device is available — callers GTEST_SKIP on that.
GameEngine::Rendering::IDevice* SharedHeadlessDevice();

// Idles the shared device and destroys everything released on it so far. A no-op when
// the device was never created.
void RetireSharedDeviceReleases();

// Retires the shared device's releases when it is destroyed. A fixture that builds a
// UIManager on the shared device declares one as its FIRST member: members are destroyed
// in reverse order, so this runs after the fixture's UIManager and render harness have
// released their buffers, and a loop of fixtures inside one test holds one fixture's
// memory at a time.
struct SharedDeviceReleaseRetirement
{
    ~SharedDeviceReleaseRetirement() { RetireSharedDeviceReleases(); }
};
