#include "Panels/ScriptDiagnosticsByAssembly.h"

#include <algorithm>
#include <tuple>

namespace GameEngine {

namespace {

bool PrecedesInSource(const CompileServerDiagnostic& a, const CompileServerDiagnostic& b)
{
    return std::tie(a.FileUtf8, a.Line, a.Column) < std::tie(b.FileUtf8, b.Line, b.Column);
}

} // namespace

void ScriptDiagnosticsByAssembly::OnCompileStarted(const std::string& assemblyName)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_RowsByAssembly.erase(assemblyName) != 0)
        m_Changed = true;
}

void ScriptDiagnosticsByAssembly::OnDiagnostics(const std::string& assemblyName,
                                                const std::vector<CompileServerDiagnostic>& diagnostics)
{
    if (diagnostics.empty())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<CompileServerDiagnostic>& rows = m_RowsByAssembly[assemblyName];
    rows.insert(rows.end(), diagnostics.begin(), diagnostics.end());
    std::stable_sort(rows.begin(), rows.end(), PrecedesInSource);
    m_Changed = true;
}

void ScriptDiagnosticsByAssembly::SetCompiledAssemblies(std::vector<std::string> compiledAssemblyNames)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CompiledAssemblies = std::move(compiledAssemblyNames);
    m_HasCompiledAssemblies = true;
    m_Changed = true;
}

void ScriptDiagnosticsByAssembly::EraseRowsOutsideCompiledSet()
{
    if (!m_HasCompiledAssemblies)
        return;
    std::erase_if(m_RowsByAssembly, [this](const auto& entry) {
        return std::find(m_CompiledAssemblies.begin(), m_CompiledAssemblies.end(), entry.first) ==
               m_CompiledAssemblies.end();
    });
}

bool ScriptDiagnosticsByAssembly::TakeRowsIfChanged(std::vector<CompileServerDiagnostic>& outRows)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Changed)
        return false;
    m_Changed = false;
    // A reload copies the module list when it starts, so one already running
    // when a package was removed still reports that package afterwards.
    EraseRowsOutsideCompiledSet();
    outRows.clear();
    for (const auto& [assemblyName, rows] : m_RowsByAssembly)
        outRows.insert(outRows.end(), rows.begin(), rows.end());
    return true;
}

} // namespace GameEngine
