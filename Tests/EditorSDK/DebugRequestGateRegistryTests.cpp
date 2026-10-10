// DebugRequestGateRegistry semantics, what the debug server relies on for every request:
//   * every gate's Before is asked in registration order and the request runs,
//   * the first Refuse stops the asking and carries its reason, and so does a Park,
//   * every gate's After hears a sent response, and its Update each server update, in
//     registration order,
//   * Handled runs, in reverse order, on exactly the gates whose Before returned, so a gate
//     whose Before threw gets no Handled,
//   * a registration while a request is being gated is refused,
//   * a gate a module registered pins that module, and the module's reload replaces it.
// Each test builds its own registry, as a server that is not the editor's does.

#include "Editor/Registries/DebugRequestGateRegistry.h"

#include "ECS/ModuleRegistration.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace ed = GameEngine::Editor;
namespace ecs = GameEngine::ECS;

namespace
{

// A gate that records its calls into `log` as "<id>.before" and "<id>.handled".
ed::DebugRequestGate RecordingGate(const std::string& id, std::vector<std::string>& log,
                                   ed::DebugRequestVerdict verdict = {})
{
    ed::DebugRequestGate gate;
    gate.GateId = id;
    gate.Before = [id, &log, verdict](const ed::DebugRequestGateContext&) {
        log.push_back(id + ".before");
        return verdict;
    };
    gate.Handled = [id, &log](const ed::DebugRequestGateContext&) { log.push_back(id + ".handled"); };
    return gate;
}

} // namespace

TEST(DebugRequestGateRegistryTests, EveryGateRunsTheRequestInRegistrationOrder)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    registry.Register(RecordingGate("first", log));
    registry.Register(RecordingGate("second", log));

    const ed::DebugRequestGateContext context{"markup_create"};
    {
        ed::DebugRequestGateRegistry::RequestScope scope(registry, context);
        EXPECT_EQ(scope.Admit().Decision, ed::DebugRequestDecision::Run);
        log.push_back("handler");
    }

    EXPECT_EQ(log, (std::vector<std::string>{"first.before", "second.before", "handler", "second.handled",
                                             "first.handled"}));
}

TEST(DebugRequestGateRegistryTests, TheFirstRefusalStopsTheAskingWithItsReason)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    registry.Register(RecordingGate("open", log));
    registry.Register(RecordingGate(
        "refuses", log, {ed::DebugRequestDecision::Refuse, "Play mode is running: stop it, then retry."}));
    registry.Register(RecordingGate("never", log));

    const ed::DebugRequestGateContext context{"save_scene"};
    ed::DebugRequestVerdict verdict;
    {
        ed::DebugRequestGateRegistry::RequestScope scope(registry, context);
        verdict = scope.Admit();
    }

    EXPECT_EQ(verdict.Decision, ed::DebugRequestDecision::Refuse);
    EXPECT_EQ(verdict.Refusal, "Play mode is running: stop it, then retry.");
    EXPECT_EQ(log, (std::vector<std::string>{"open.before", "refuses.before", "refuses.handled", "open.handled"}));
}

TEST(DebugRequestGateRegistryTests, AParkStopsTheAskingAndEveryAskedGateIsHandled)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    registry.Register(RecordingGate("open", log));
    registry.Register(RecordingGate("parks", log, {ed::DebugRequestDecision::Park, ""}));
    registry.Register(RecordingGate("never", log));

    const ed::DebugRequestGateContext context{"save_scene"};
    ed::DebugRequestVerdict verdict;
    {
        ed::DebugRequestGateRegistry::RequestScope scope(registry, context);
        verdict = scope.Admit();
    }

    EXPECT_EQ(verdict.Decision, ed::DebugRequestDecision::Park);
    EXPECT_EQ(log, (std::vector<std::string>{"open.before", "parks.before", "parks.handled", "open.handled"}));
}

TEST(DebugRequestGateRegistryTests, EveryGateHearsASentResponseInRegistrationOrder)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    for (const std::string id : {"first", "second"})
    {
        ed::DebugRequestGate gate = RecordingGate(id, log);
        gate.After = [id, &log](const ed::DebugRequestGateContext& context, const nlohmann::json* response) {
            log.push_back(id + ".after:" + std::to_string(context.RequestId) + ":" + (response ? response->dump() : "null"));
        };
        registry.Register(std::move(gate));
    }

    const nlohmann::json response{{"ok", true}};
    registry.NotifyAnswered(ed::DebugRequestGateContext{"get_log", 3, nullptr, 41}, &response);

    EXPECT_EQ(log, (std::vector<std::string>{"first.after:41:{\"ok\":true}", "second.after:41:{\"ok\":true}"}));
}

TEST(DebugRequestGateRegistryTests, EveryGateGetsItsUpdateWithTheUndoHistoryInRegistrationOrder)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    for (const std::string id : {"first", "second"})
    {
        ed::DebugRequestGate gate = RecordingGate(id, log);
        gate.Update = [id, &log](ed::UndoRedoService* undo) { log.push_back(id + (undo ? ".update" : ".none")); };
        registry.Register(std::move(gate));
    }
    registry.Register(RecordingGate("without", log));

    ed::UndoRedoService undo;
    registry.NotifyUpdate(&undo);

    EXPECT_EQ(log, (std::vector<std::string>{"first.update", "second.update"}));
}

TEST(DebugRequestGateRegistryTests, AGateWhoseBeforeThrowsGetsNoHandled)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    registry.Register(RecordingGate("open", log));
    ed::DebugRequestGate throws = RecordingGate("throws", log);
    throws.Before = [&log](const ed::DebugRequestGateContext&) -> ed::DebugRequestVerdict {
        log.push_back("throws.before");
        throw std::runtime_error("gate failure");
    };
    registry.Register(std::move(throws));

    const ed::DebugRequestGateContext context{"markup_create"};
    {
        ed::DebugRequestGateRegistry::RequestScope scope(registry, context);
        EXPECT_THROW(scope.Admit(), std::runtime_error);
    }

    EXPECT_EQ(log, (std::vector<std::string>{"open.before", "throws.before", "open.handled"}));
}

TEST(DebugRequestGateRegistryTests, ARegistrationDuringARequestIsRefused)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    ed::DebugRequestGate first = RecordingGate("first", log);
    first.Before = [&registry, &log](const ed::DebugRequestGateContext&) {
        log.push_back("first.before");
        registry.Register(RecordingGate("second", log, {ed::DebugRequestDecision::Refuse, "replaced"}));
        return ed::DebugRequestVerdict{};
    };
    registry.Register(std::move(first));
    registry.Register(RecordingGate("second", log));

    const ed::DebugRequestGateContext context{"markup_create"};
    ed::DebugRequestVerdict verdict;
    {
        ed::DebugRequestGateRegistry::RequestScope scope(registry, context);
        verdict = scope.Admit();
    }

    EXPECT_EQ(verdict.Decision, ed::DebugRequestDecision::Run);
    EXPECT_EQ(log, (std::vector<std::string>{"first.before", "second.before", "second.handled", "first.handled"}));
}

TEST(DebugRequestGateRegistryTests, AModuleGatePinsItsModuleAndTheModuleReloadReplacesIt)
{
    ed::DebugRequestGateRegistry registry;
    std::vector<std::string> log;
    ecs::SetActiveRegistrationModule("GatePack", 1);
    registry.Register(RecordingGate("pack", log));
    ecs::SetActiveRegistrationModule("GatePack", 2);
    registry.Register(RecordingGate("pack", log, {ed::DebugRequestDecision::Refuse, "reloaded"}));
    ecs::ClearActiveRegistrationModule();
    registry.Register(RecordingGate("editor", log));

    std::vector<std::string> pins;
    registry.AppendModulePins("GatePack", pins);
    EXPECT_EQ(pins, (std::vector<std::string>{"debug request gate 'pack'"}));
    std::vector<std::string> otherPins;
    registry.AppendModulePins("OtherPack", otherPins);
    EXPECT_TRUE(otherPins.empty());

    const ed::DebugRequestGateContext context{"markup_create"};
    ed::DebugRequestGateRegistry::RequestScope scope(registry, context);
    EXPECT_EQ(scope.Admit().Refusal, "reloaded");
}
