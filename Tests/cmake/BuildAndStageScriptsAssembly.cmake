# Build-time helper for the ScriptingAbiIntegrationTests staging chain.
#
# The GameEngine.Scripts project is EMITTED by the engine's ScriptManager into
# bin/<config>/ScriptAssemblies at first editor/test run — in a fresh worktree it
# does not exist yet. A plain custom-target `dotnet build` hard-fails the whole
# ScriptingAbiIntegrationTests build in that case, so this script builds and
# stages the scripts assembly only when the emitted project is present.
#
# Arguments (via -D):
#   DOTNET_EXE     - dotnet executable
#   SCRIPTS_DIR    - bin/<engine config>/ScriptAssemblies directory
#   CONFIG         - GE_MANAGED_FIXTURE_CONFIG: the configuration the managed
#                    fixtures are built into, and the staged
#                    ScriptAssemblies/<config>/net10.0 segment the suites probe

set(_csproj "${SCRIPTS_DIR}/GameEngine.Scripts.csproj")
if(NOT EXISTS "${_csproj}")
    message(STATUS "GameEngine.Scripts.csproj not emitted yet (fresh worktree); skipping scripts assembly build/stage")
    return()
endif()

execute_process(
    COMMAND "${DOTNET_EXE}" build "${_csproj}" -c "${CONFIG}"
    WORKING_DIRECTORY "${SCRIPTS_DIR}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "GameEngine.Scripts build failed (rc=${_rc})")
endif()

# Stage under ScriptAssemblies/<config>/net10.0 for tests that probe this layout.
set(_dll "${SCRIPTS_DIR}/GameEngine.Scripts.dll")
if(EXISTS "${_dll}")
    file(MAKE_DIRECTORY "${SCRIPTS_DIR}/${CONFIG}/net10.0")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_dll}" "${SCRIPTS_DIR}/${CONFIG}/net10.0/GameEngine.Scripts.dll")
endif()
