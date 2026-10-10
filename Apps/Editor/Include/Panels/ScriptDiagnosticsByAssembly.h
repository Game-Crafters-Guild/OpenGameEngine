#pragma once

#include "Jobs/CompileServerClient.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace GameEngine {

/**
 * @brief The compile diagnostics on screen, kept per assembly.
 *
 * One reload compiles every package module, the editor scripts and the project
 * scripts, one compile-server request each. A request that starts replaces only
 * its own assembly's diagnostics, so a package's errors stay listed while the
 * requests after it run, until that package compiles again.
 *
 * The two intake calls take the compile-server observers' arguments and may run
 * on any thread; TakeRowsIfChanged runs on the thread that owns the rows.
 */
class ScriptDiagnosticsByAssembly {
public:
    /// The named assembly's request started: its previous diagnostics go.
    void OnCompileStarted(const std::string& assemblyName);
    /// Diagnostics of the named assembly's request, added to its rows.
    void OnDiagnostics(const std::string& assemblyName, const std::vector<CompileServerDiagnostic>& diagnostics);
    /// The assemblies a reload compiles now (a package was added, removed or
    /// disabled). Rows of any other assembly go, since no request will start for
    /// it again to replace them, and so do rows a reload that was already running
    /// delivers for one later. Until the first call every assembly is kept.
    void SetCompiledAssemblies(std::vector<std::string> compiledAssemblyNames);

    /**
     * @brief Copy every assembly's rows into @p outRows when they changed since the last call.
     *
     * Only assemblies in the compiled set, in name order; within one, sorted by
     * file, line and column.
     * @return false, leaving @p outRows untouched, when nothing changed.
     */
    bool TakeRowsIfChanged(std::vector<CompileServerDiagnostic>& outRows);

private:
    /// Erase the rows of assemblies outside m_CompiledAssemblies. Call under m_Mutex.
    void EraseRowsOutsideCompiledSet();

    std::mutex m_Mutex;
    std::vector<std::string> m_CompiledAssemblies;
    bool m_HasCompiledAssemblies = false;
    std::map<std::string, std::vector<CompileServerDiagnostic>> m_RowsByAssembly;
    bool m_Changed = false;
};

} // namespace GameEngine
