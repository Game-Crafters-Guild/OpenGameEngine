#pragma once

// Test-only seam onto GameUIHost's otherwise-private per-frame steps.
//
// Production hosts drive the host exclusively through SyncDocuments/UpdateAndRender (or the
// SyncAndRender composition); the headless suites pump SetPointer/Update/SyncFromWorld
// directly (no world tick or RG render), so this one friend grants the seam instead of
// widening the public API. Friended by GameUIHost.

#include "Engine/GameUI/GameUIHost.h"

#include <cstdint>

namespace GameEngine
{
namespace ECS
{
class World;
}

struct GameUIHostTestAccess
{
    static void SetPointer(GameUIHost& host, float x, float y, bool down) { host.SetPointer(x, y, down); }
    // The production leave transition, which CancelPointer runs when the pointer stops
    // being over the surface. Driven directly here: the input-path tests hold no pointer
    // source to take away.
    static void PointerLeftSurface(GameUIHost& host) { host.PointerLeftSurface(); }
    static void Update(GameUIHost& host, float dt, uint32_t w, uint32_t h) { host.Update(dt, w, h); }
    static void SyncFromWorld(GameUIHost& host, ECS::World& world) { host.SyncFromWorld(world); }
    static bool AnyReadyFullscreenSubtree(const GameUIHost& host) { return host.AnyReadyFullscreenSubtree(); }
    // CreateForHost is the only production route to this flag and it also reads a font
    // off disk; the headless tests construct the host directly, so they set it here.
    static void SetNeverClearTarget(GameUIHost& host, bool neverClear) { host.SetNeverClearTarget(neverClear); }
};

} // namespace GameEngine
