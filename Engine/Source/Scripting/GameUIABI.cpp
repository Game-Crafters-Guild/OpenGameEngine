// GameUIABI.cpp — the DOCUMENT layer of the game-UI scripting ABI, and the only export in
// the whole surface that knows what an entity is. Mapping an entity to a mounted UIDocument
// subtree is precisely what the gameplay GameUIHost does, so that is where the entity id
// belongs; everything a script then does to the element goes through the entity-blind
// GE_UIElement_* layer (UIElementABI.cpp).
//
// GE_GameUI_FindElement resolves an element by id ONCE and returns its instance id — a
// process-wide monotonic value that is never reused. The caller keeps that integer instead of
// re-walking every mounted document's subtree by string on every call, and because ids are
// never reused it can never come to name a different element.
//
// Mirrors ECSABI.cpp: extern "C", GE_API/GE_CDECL, every body try/catch -> GE_Result_Fail. The
// host is the process-wide gameplay host published by GameUI::SetHost (the Player and the editor
// Game View register theirs; the Scene View preview does not), reached through the GameUI
// namespace rather than the host pointer so a null host is one check in one place.
//
// Threading: UI element state has no internal locking; the host ticks it on the main thread.
// Every export here must be called from the user script's main-thread update.

#include "Scripting/ScriptingABI.h" // GE_API, GE_CDECL, GE_Result

#include "Engine/GameUI/GameplayUI.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Logger/Logger.h"

#include "UI/UIElement.h"

#include <atomic>
#include <string_view>

namespace
{
// A miss has three causes and one return code. "The id is not in the document" and "the subtree is
// still loading" are both ordinary, and a polling script is written for them. The third is not
// ordinary and looks identical: NO GAMEPLAY HOST HAS EVER PUBLISHED A DOCUMENT, so every id in the
// project misses forever and no amount of polling will help. That is the state an editor session
// sits in until the Game View composites — the Scene View's preview host deliberately does not
// publish — and it cost a full session to diagnose from the outside.
//
// Said ONCE per process. This is called from a script's per-frame poll, which is exactly the
// caller that would turn a diagnostic into a flood.
void ReportUnresolvableHostOnce()
{
    // The latch is spent only by a branch that actually SAYS something. Testing it up front would
    // let the first ordinary miss — wrong id, subtree still loading, with a host perfectly well
    // published — burn the budget, after which the unpollable case this exists for could never be
    // reported at all. That is the exact configuration it was written to catch.
    static std::atomic<bool> s_Reported{false};
    if (s_Reported.load(std::memory_order_relaxed))
        return;

    const GameEngine::GameUIHost* host = GameEngine::GameUI::GetHost();
    if (!host)
    {
        if (s_Reported.exchange(true))
            return;
        Logger::Log::Warning(
            "GameUI: a script asked for an element but NO gameplay UI host is published, so every "
            "lookup will miss for as long as that is true. In the editor the host is published by "
            "the Game View; the Scene View's HUD preview deliberately does not publish one. "
            "Reported once per process.");
        return;
    }
    if (!host->HasAnyDocuments())
    {
        if (s_Reported.exchange(true))
            return;
        Logger::Log::Warning(
            "GameUI: a script asked for an element but the published gameplay UI host has no bound "
            "document subtrees, so every lookup will miss until one binds. Reported once per "
            "process.");
    }
    // A host with documents bound is the ordinary miss — wrong id, or a subtree still loading —
    // and a script polling through it is behaving correctly. Nothing to say.
}
} // namespace

extern "C"
{

// Resolve an element by id and report its instance id. entityId == 0 searches every mounted
// document (lowest entity id wins, so the winner is stable); otherwise the search is scoped to
// that UIDocument's subtree. GE_Result_NotFound while the subtree is still loading — a script
// polls until it succeeds, then keeps the instance id.
GE_API GE_Result GE_CDECL GE_GameUI_FindElement(uint64_t entityId, const char* id, uint64_t* outInstanceId)
{
    try
    {
        if (!outInstanceId)
            return GE_Result_InvalidArg;
        *outInstanceId = 0;
        // An empty id is a caller error, not a miss: FindById matches the first element
        // whose id is empty, which is most of a tree, so an unguarded empty string would
        // hand back a durable handle to an arbitrary element and every later write would
        // land on it. Rejected before the lookup, so the result does not depend on what
        // happens to be mounted.
        if (!id || *id == '\0')
            return GE_Result_InvalidArg;
        const std::string_view key{id};
        GameEngine::UIElement* el = (entityId == 0) ? GameEngine::GameUI::FindElementByIdAny(key)
                                                    : GameEngine::GameUI::FindElementById(entityId, key);
        if (!el)
        {
            ReportUnresolvableHostOnce();
            return GE_Result_NotFound;
        }
        *outInstanceId = el->GetInstanceId();
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

} // extern "C"
