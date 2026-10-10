#pragma once

// Session-gated event registration — PRIVATE to the engine's implementation.
//
// This header deliberately does not live under Engine/Include: it is not part of
// the public C++ surface, and it should not become part of it. C++ callers hold
// the element and register on it directly through the ordinary UI APIs; the TREE
// LIFECYCLE owns the result — play-stop drops every bound UIDocument subtree
// (GameUIHost::ResetBoundDocuments, #1061), and a native module unload revokes
// handlers whose callable belongs to the unmapped image (UI/ModuleOwnedHandlers.h).
// Neither needs a session gate to be correct, and a public wrapper here would be
// an engine callable wrapping a caller's callable — exactly the nesting that hides
// a user module's ownership from the unload-time revocation.
//
// The gate exists for the SCRIPTING ABI, whose managed callers have no native
// notification of a collectible load context going away. UIElementABI.cpp
// (GE_UIElement_RegisterEvent / GE_UIElement_UnregisterEvent) is the only
// production consumer; GameUIHostTests reaches it as a test-access seam, the
// same way it reaches UIManager_Internal.h.

#include "UI/UIEvents.h"

#include <cstdint>
#include <functional>

namespace GameEngine
{
namespace GameUI
{
namespace Detail
{

// Register a handler for ONE event id on ONE element, bound to the CURRENT gameplay
// session. Addressed by element instance id: resolution runs through the published
// host's UIManager, which fails closed on destruction, detachment and re-home.
// Returns 0 when the id does not resolve (the caller polls until its subtree
// mounts), else a token key that is unique within that element instance.
//
// The event id is whatever the caller asks for; the semantics are the dispatcher's.
// kEventButtonClick in particular is Button's own armed-and-inside notion of a click
// (and keyboard activation), never a raw mouse-up — and a press the pointer stream
// abandons is delivered as kEventMouseCancel, so it never becomes a click.
//
// The handler re-checks GetGameplaySession() when it FIRES, so a subscription
// that somehow outlives play-stop (uxml hot-reload mid-play, a missed
// ResetBoundDocuments) goes inert rather than running script code against the
// restored edit-mode world. That is a damage bound, not a lifetime guarantee.
std::uint64_t RegisterSessionEvent(std::uint64_t instanceId, EventId eventId,
                                   std::function<void(const UIEvent&)> onEvent);

// Remove a handler minted above. The (eventId, token) pair is what identifies it:
// handler keys are unique within an element's lifetime but NOT across event ids.
// Re-resolves the instance id, so it is a safe no-op if the element was already
// destroyed — and since instance ids are never reused, the token can never land on
// a different element's handler.
void UnregisterSessionEvent(std::uint64_t instanceId, EventId eventId, std::uint64_t token);

} // namespace Detail
} // namespace GameUI
} // namespace GameEngine
