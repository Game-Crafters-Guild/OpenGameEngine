#include "Engine/GameUI/GameplayUI.h"
#include "SessionEvent.h"

#include "Engine/GameUI/GameUIHost.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <utility>

namespace GameEngine {
namespace GameUI {

namespace {

GameUIHost* g_Host = nullptr;
uint64_t g_Session = 0;

} // namespace

void SetHost(GameUIHost* host)
{
    g_Host = host;
}

GameUIHost* GetHost()
{
    return g_Host;
}

UIManager* GetUIManager()
{
    return g_Host ? g_Host->GetUIManager() : nullptr;
}

UIElement* FindDocumentRoot(uint64_t entityId)
{
    return g_Host ? g_Host->FindDocumentRoot(entityId) : nullptr;
}

UIElement* FindElementById(uint64_t entityId, std::string_view id)
{
    return g_Host ? g_Host->FindElementById(entityId, id) : nullptr;
}

UIElement* FindElementByIdAny(std::string_view id, uint64_t* outOwnerEntityId)
{
    return g_Host ? g_Host->FindElementByIdAny(id, outOwnerEntityId) : nullptr;
}

uint64_t GetBindGeneration()
{
    return g_Host ? g_Host->GetBindGeneration() : 0;
}

void NotifyGameplayStopped()
{
    ++g_Session;
    // Drop bound document subtrees. User-module std::function targets die with
    // the elements while UserScripts.dll is still mapped. The next
    // SyncFromWorld rebuilds from UIDocument — the same source play-mode
    // already snapshots — so edit hover is CSS on a fresh tree, not a
    // walk that could clear engine handlers on the UIManager root.
    if (g_Host)
        g_Host->ResetBoundDocuments();
}

uint64_t GetGameplaySession()
{
    return g_Session;
}


namespace Detail
{

namespace
{

// Element resolution for the gate: O(1) instance-id probe plus the manager's
// root-reachability filter, so a destroyed, detached or re-homed element fails
// closed. Never a raw pointer held across calls.
UIElement* ResolveByInstanceId(uint64_t instanceId)
{
    UIManager* ui = GetUIManager();
    return ui ? ui->FindElementByInstanceId(instanceId) : nullptr;
}

} // namespace

uint64_t RegisterSessionEvent(uint64_t instanceId, EventId eventId, std::function<void(const UIEvent&)> onEvent)
{
    if (!onEvent || eventId == 0)
        return 0;
    UIElement* el = ResolveByInstanceId(instanceId);
    if (!el)
        return 0;

    const uint64_t session = g_Session;

    // The gate needs a wrapper, and a wrapper hides what it wraps: the registered
    // callable is Engine.dll's, so the ownership stamp resolves to Engine.dll and a
    // caller's callable from a hot-swappable module would never be revoked at that
    // module's unload — the blind spot 722094c79 removed from Button by deleting
    // its nesting. The wrapper COPIES rather than moves, so `onEvent` survives for
    // the hand-off below, which is the only thing that knows the inner callable's
    // owning image.
    //
    // Every registration that wraps the caller's callable needs this hand-off —
    // this one and Manipulator::Subscribe among them. A path that stores the
    // caller's callable flat resolves its owner directly and needs none.
    auto token = el->RegisterEventHandler(eventId,
                                          [session, onEvent](UIEvent& e)
                                          {
                                              if (g_Session != session)
                                                  return;
                                              onEvent(e);
                                          });
    el->AdoptHandlerOwnerFrom(token, onEvent);
    return token.Key;
}

void UnregisterSessionEvent(uint64_t instanceId, EventId eventId, uint64_t token)
{
    UIElement* el = ResolveByInstanceId(instanceId);
    if (!el)
        return;
    UIElement::EventHandlerToken t{};
    t.Id = eventId;
    t.Key = token;
    el->UnregisterEventHandler(t);
}

} // namespace Detail

} // namespace GameUI
} // namespace GameEngine
