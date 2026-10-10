#include "Editor/Registries/DebugRequestGateRegistry.h"

#include "ECS/ModuleRegistration.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

namespace
{
uint16_t s_EditorDebugPort = 0;
} // namespace

uint16_t EditorDebugPort()
{
    return s_EditorDebugPort;
}

void SetEditorDebugPort(uint16_t port)
{
    s_EditorDebugPort = port;
}

DebugRequestGateRegistry& DebugRequestGateRegistry::Get()
{
    static DebugRequestGateRegistry s_Instance;
    return s_Instance;
}

void DebugRequestGateRegistry::Register(DebugRequestGate gate)
{
    if (gate.GateId.empty() || !gate.Before)
    {
        Logger::Log::Error("DebugRequestGateRegistry: rejecting gate registration '{}': "
                           "GateId and Before are required",
                           gate.GateId);
        return;
    }
    if (m_OpenScopes > 0)
    {
        Logger::Log::Error("DebugRequestGateRegistry: refusing gate registration '{}' while a request is "
                           "being gated; register gates at startup or module load",
                           gate.GateId);
        return;
    }

    const auto sameId = [&](const Entry& existing) { return existing.Gate.GateId == gate.GateId; };
    const auto existing = std::find_if(m_Gates.begin(), m_Gates.end(), sameId);
    if (existing != m_Gates.end())
    {
        existing->Gate = std::move(gate);
        existing->Module = ECS::GetActiveRegistrationModule();
        return;
    }
    m_Gates.push_back(Entry{std::move(gate), ECS::GetActiveRegistrationModule()});
}

bool DebugRequestGateRegistry::IsClaimed(std::string_view method) const
{
    for (const Entry& entry : m_Gates)
    {
        const std::vector<std::string>& claimed = entry.Gate.ClaimedMethods;
        if (std::find(claimed.begin(), claimed.end(), method) != claimed.end())
            return true;
    }
    return false;
}

void DebugRequestGateRegistry::NotifyClientClosed(uint32_t clientId) const
{
    for (const Entry& entry : m_Gates)
        if (entry.Gate.ClientClosed)
            entry.Gate.ClientClosed(clientId);
}

void DebugRequestGateRegistry::NotifyAnswered(const DebugRequestGateContext& context,
                                              const nlohmann::json* response) const
{
    for (const Entry& entry : m_Gates)
        if (entry.Gate.After)
            entry.Gate.After(context, response);
}

void DebugRequestGateRegistry::NotifyUpdate(UndoRedoService* undo) const
{
    for (const Entry& entry : m_Gates)
        if (entry.Gate.Update)
            entry.Gate.Update(undo);
}

void DebugRequestGateRegistry::AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const Entry& entry : m_Gates)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("debug request gate '" + entry.Gate.GateId + "'");
    }
}

DebugRequestGateRegistry::RequestScope::RequestScope(const DebugRequestGateRegistry& registry,
                                                     const DebugRequestGateContext& context)
    : m_Registry(registry), m_Context(context)
{
    ++m_Registry.m_OpenScopes;
}

DebugRequestGateRegistry::RequestScope::~RequestScope()
{
    for (size_t index = m_Asked; index > 0; --index)
    {
        const DebugRequestGate& gate = m_Registry.m_Gates[index - 1].Gate;
        if (gate.Handled)
            gate.Handled(m_Context);
    }
    --m_Registry.m_OpenScopes;
}

DebugRequestVerdict DebugRequestGateRegistry::RequestScope::Admit()
{
    for (const Entry& entry : m_Registry.m_Gates)
    {
        const DebugRequestGate& gate = entry.Gate;
        DebugRequestVerdict verdict = gate.Before(m_Context);
        ++m_Asked;
        if (verdict.Decision != DebugRequestDecision::Run)
            return verdict;
    }
    return {};
}

} // namespace GameEngine::Editor
