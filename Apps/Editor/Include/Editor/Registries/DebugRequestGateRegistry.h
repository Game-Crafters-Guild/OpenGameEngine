#pragma once

#include "ECS/ModuleRegistration.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Editor
{

// The port this editor's debug server listens on, with the --debug-port >
// GE_EDITOR_DEBUG_PORT > default precedence applied; 0 while no server runs (it failed to
// start, or this process is not the editor).
uint16_t EditorDebugPort();
// Set by the editor when its debug server starts (its port) and stops (0).
void SetEditorDebugPort(uint16_t port);

class UndoRedoService;

// The debug-server request a gate is asked about. A field joins with the first gate that
// reads it.
struct DebugRequestGateContext
{
    std::string_view Method; // the handler's registered name, e.g. "markup_create"
    // The connection the request came from; ids are never reused while the server runs.
    uint32_t ClientId = 0;
    // The request's parameters; null when it has none to read, and in After for a
    // deferred response.
    const nlohmann::json* Params = nullptr;
    // Names the request from its arrival to its response: the same each time a parked
    // request is offered again, never reused while the server runs.
    uint64_t RequestId = 0;
    // The editor's undo history, for a gate that groups a request's edits into one step;
    // null when the server has none.
    UndoRedoService* Undo = nullptr;
};

enum class DebugRequestDecision
{
    Run,
    Refuse,
    // Hold the request unanswered: the server offers it to the gates again at each
    // editor update, until a gate's answer is Run or Refuse or its connection closes
    // (then it is dropped unanswered). Every later request from the same connection waits
    // behind it, and they run in arrival order after it.
    Park,
};

struct DebugRequestVerdict
{
    DebugRequestDecision Decision = DebugRequestDecision::Run;
    // Sent to the client as the refusal's reason; written for the person reading the
    // client's log, so it states what to do instead.
    std::string Refusal;
};

// What a unit registers to see every debug-server request on the main thread.
struct DebugRequestGate
{
    // Stable id and replace-forward key, e.g. "markups".
    std::string GateId;
    // Asked before the handler runs; a Refuse answer keeps the handler from running and
    // the request is answered with the refusal; a Park answer holds it (Park). A parked
    // request is asked again, with the same RequestId, at each offer. A Before that throws
    // undoes its own work: its gate gets no Handled. Required.
    std::function<DebugRequestVerdict(const DebugRequestGateContext&)> Before;
    // Runs when the request's synchronous handling ends: the handler returned (a deferred
    // handler included, before its deferred response exists), threw, or did not run because
    // a gate refused or parked it (once per offer). A scope a gate opened in Before closes
    // here, so it never spans the frames before a deferred response; an undo step that
    // groups a request's edits needs the same moment. Runs from a destructor, possibly
    // while a handler's exception unwinds, so it must not throw. Optional.
    std::function<void(const DebugRequestGateContext&)> Handled;
    // Runs once the request's response is sent, after Handled: the response object the
    // client receives ({"id", "ok", ...}), the deferred one when it is sent; for a response
    // serialized off the main thread, the same object without the result's PNG payload
    // (`pngBase64`; its `filePath` and size stay). A request whose connection closed while
    // it was parked, and a deferred response that is never sent, get no After. Optional.
    std::function<void(const DebugRequestGateContext&, const nlohmann::json* response)> After;
    // Runs once per server update, after that update's requests, with the editor's undo
    // history (null when the server has none): where a gate does work the user asked for
    // between requests, such as undoing the steps a conversation's turn made. Optional.
    std::function<void(UndoRedoService* undo)> Update;
    // Methods whose whole answer is this gate's decision (a session binding, a permission
    // asked before an action outside the editor). Their editor handlers answer an
    // admission only while a registered gate claims the method and refuse otherwise, so a
    // client asking an editor where no gate understands the method is refused, never
    // admitted by default. Optional.
    std::vector<std::string> ClaimedMethods;
    // Runs on the main thread once the connection `clientId` has closed, after every
    // request it sent was gated; the server never reuses an id while it runs. State a
    // gate keeps per connection is released here. Optional.
    std::function<void(uint32_t clientId)> ClientClosed;
};

// The gates the editor's debug server consults for every request, in registration order.
// A deferred response that is never sent (dropped by the server's Stop(), or a handler
// that returned the deferred marker without enqueueing its poll) gets no After, so Handled
// is the last call a request that ran is sure to make, and a gate holds no per-request
// state that only After would release. Registration and consultation are main-thread
// only, like the other editor registries; registering a GateId again replaces the earlier
// gate in place. A registration while a request is being gated (a RequestScope is open)
// is refused.
class DebugRequestGateRegistry
{
  public:
    // The editor's registry, which its debug server consults. A separately built registry
    // serves a server that is not the editor's (tests).
    static DebugRequestGateRegistry& Get();

    DebugRequestGateRegistry() = default;
    DebugRequestGateRegistry(const DebugRequestGateRegistry&) = delete;
    DebugRequestGateRegistry& operator=(const DebugRequestGateRegistry&) = delete;

    void Register(DebugRequestGate gate);

    // A registered gate lists `method` in its ClaimedMethods.
    bool IsClaimed(std::string_view method) const;
    // Tells every gate that the connection `clientId` has closed (ClientClosed).
    void NotifyClientClosed(uint32_t clientId) const;
    // Tells every gate, in registration order, that the response of the request
    // `context` describes was sent (After).
    void NotifyAnswered(const DebugRequestGateContext& context, const nlohmann::json* response) const;
    // Gives every gate, in registration order, its once-per-update call (Update).
    void NotifyUpdate(UndoRedoService* undo) const;

    // C12 editor-kind unload-refusal diagnostics: appends a description of every gate
    // registered while `moduleId`'s registrars ran (ECS/ModuleRegistration.h). A gate's
    // functions are module code and pin the module's images mapped; the module's reload
    // registers the same GateId again, which replaces the gate in place.
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

    // One request's pass through the gates (one offer, for a parked request). Admit asks
    // each gate's Before in registration order and stops at the first Refuse or Park; the
    // destructor runs Handled, in reverse order, on every gate whose Before returned, the
    // refusing or parking gate included.
    class RequestScope
    {
      public:
        RequestScope(const DebugRequestGateRegistry& registry, const DebugRequestGateContext& context);
        ~RequestScope();
        RequestScope(const RequestScope&) = delete;
        RequestScope& operator=(const RequestScope&) = delete;

        // Called once, before the handler.
        DebugRequestVerdict Admit();

      private:
        const DebugRequestGateRegistry& m_Registry;
        const DebugRequestGateContext& m_Context;
        size_t m_Asked = 0; // gates whose Before returned, from the first
    };

  private:
    // A gate and the module stamp active when it was registered.
    struct Entry
    {
        DebugRequestGate Gate;
        ECS::ModuleRegistrationStamp Module;
    };

    std::vector<Entry> m_Gates; // registration order
    // RequestScopes alive over this registry; Register refuses while any is.
    mutable uint32_t m_OpenScopes = 0;
};

} // namespace GameEngine::Editor
