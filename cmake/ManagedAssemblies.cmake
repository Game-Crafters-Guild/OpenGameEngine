# ManagedAssemblies.cmake
#
# Defines the managed (.NET) build infrastructure: the stamp-driven
# ge_add_managed_assembly() wrapper and every *Assembly target built out of
# Managed/.
#
# Included from the root CMakeLists BEFORE the Apps/, Tests/ and Examples/
# subdirectories so those can declare dependencies on these targets directly.
# A consumer configured before the targets exist evaluates
# if(TARGET SomeAssembly) as false and silently drops its dependency, which
# surfaces only as an explicit --target build failing on a clean tree.
#
# Requires BUILD_EDITOR to be declared before inclusion (gates the two
# editor-only assemblies).

if(NOT ENABLE_SCRIPTING)
  return()
endif()

# Xcode GUI builds typically do not inherit your interactive shell PATH, so invoking
# `dotnet` by name from custom build phases can fail with "no such file or directory".
# Resolve dotnet at configure time and use the absolute path in custom targets.
if(NOT DOTNET_EXECUTABLE OR DOTNET_EXECUTABLE STREQUAL "DOTNET_EXECUTABLE-NOTFOUND")
  set(_ge_dotnet_hints "")

  if(DEFINED ENV{DOTNET_ROOT} AND NOT "$ENV{DOTNET_ROOT}" STREQUAL "")
    list(APPEND _ge_dotnet_hints "$ENV{DOTNET_ROOT}")
  endif()

  # If we detected nethost from a dotnet pack, derive the dotnet root.
  # Example: /usr/local/share/dotnet/packs/... -> /usr/local/share/dotnet
  if(DEFINED NETHOST_LIBRARY AND EXISTS "${NETHOST_LIBRARY}")
    if("${NETHOST_LIBRARY}" MATCHES "(.*/share/dotnet)/packs/")
      list(APPEND _ge_dotnet_hints "${CMAKE_MATCH_1}")
    endif()
  endif()

  # Common install locations (macOS/Linux)
  list(APPEND _ge_dotnet_hints
    "/usr/local/share/dotnet"
    "/usr/share/dotnet"
    "/opt/homebrew/share/dotnet"
    "/usr/local/bin"
    "/opt/homebrew/bin"
  )

  if(WIN32)
    # Common Windows install locations (useful for IDEs that don't inherit PATH).
    list(APPEND _ge_dotnet_hints
      "C:/Program Files/dotnet"
      "C:/Program Files (x86)/dotnet"
    )
  endif()

  find_program(DOTNET_EXECUTABLE
    NAMES dotnet dotnet.exe
    HINTS ${_ge_dotnet_hints}
    PATH_SUFFIXES "" "bin"
  )

  if(NOT DOTNET_EXECUTABLE OR DOTNET_EXECUTABLE STREQUAL "DOTNET_EXECUTABLE-NOTFOUND")
    message(FATAL_ERROR
      "ENABLE_SCRIPTING=ON but 'dotnet' was not found. "
      "Install the .NET 10 SDK, or set DOTNET_ROOT / DOTNET_EXECUTABLE. "
      "Common macOS path: /usr/local/share/dotnet/dotnet"
    )
  endif()
endif()

# ---- Managed assembly build infrastructure ----
#
# Each managed assembly is wrapped as a stamp-driven custom target. The stamp
# file's DEPENDS list captures every .cs / .csproj / .props / .targets file
# in the project's source tree, outside its obj/ and bin/ outputs (re-globbed
# each build via CONFIGURE_DEPENDS, cmake/ManagedProjectSources.cmake), any
# other file the build reads (INPUTS), plus the stamps of any declared
# upstream managed targets.
#
# Effect: MSBuild visits each *Assembly project on every build, but the
# custom command's DEPENDS check skips the dotnet invocation entirely if
# no source file is newer than the stamp. A no-op visit costs ~150 ms of
# MSBuild bookkeeping instead of the 2-5 s a `dotnet build` invocation takes
# even when "All projects are up-to-date".
#
# IMPORTANT: Xcode/CMake generators may set an environment variable `TargetName`
# for the build script target. MSBuild imports env vars as properties, and
# `TargetName` overrides the managed output assembly name. Always unset it.

include("${CMAKE_CURRENT_LIST_DIR}/ManagedProjectSources.cmake")

set(_GE_MANAGED_STAMP_DIR "${CMAKE_BINARY_DIR}/_managed_stamps")
file(MAKE_DIRECTORY "${_GE_MANAGED_STAMP_DIR}")

function(ge_add_managed_assembly target_name)
  set(options ALL)
  set(oneValueArgs CSPROJ COMMENT)
  set(multiValueArgs DEPENDS_ON INPUTS)
  cmake_parse_arguments(GE_AMA "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT GE_AMA_CSPROJ)
    message(FATAL_ERROR "ge_add_managed_assembly(${target_name}): CSPROJ argument is required")
  endif()

  get_filename_component(_proj_dir "${GE_AMA_CSPROJ}" DIRECTORY)

  # Every source the dotnet build could read from this project's tree. The
  # globs are re-checked before each build, so an added file re-runs CMake
  # and is picked up; they never see the project's own obj/ and bin/ outputs.
  ge_managed_project_sources(_sources "${_proj_dir}")

  # Shared MSBuild imports (Directory.Build.props/.targets) live at the Managed/
  # root, ABOVE each project dir, so the per-project glob above never sees them.
  # An edit to Directory.Build.props must still rebuild every managed assembly —
  # e.g. its RollForward is baked into each generated *.runtimeconfig.json, which
  # the standalone CompileServerHost launches with, and it reads the root VERSION file into
  # every assembly's version. Depend on those files explicitly.
  foreach(_shared
      "${CMAKE_SOURCE_DIR}/Managed/Directory.Build.props"
      "${CMAKE_SOURCE_DIR}/Managed/Directory.Build.targets"
      "${CMAKE_SOURCE_DIR}/VERSION")
    if(EXISTS "${_shared}")
      list(APPEND _sources "${_shared}")
    endif()
  endforeach()
  # Managed/Common sources are compiled into several assemblies through csproj
  # Compile Links (the native shim loader, the ABI binding), outside every
  # project dir, so an edit there must rebuild each of them too.
  file(GLOB _common_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Managed/Common/*.cs")
  list(APPEND _sources ${_common_sources})

  # Upstream-target stamps go in the file-level DEPENDS so an upstream
  # rebuild also drives this one. Target-level add_dependencies below
  # additionally enforces build ordering for MSBuild.
  set(_dep_stamps "")
  foreach(_dep IN LISTS GE_AMA_DEPENDS_ON)
    list(APPEND _dep_stamps "${_GE_MANAGED_STAMP_DIR}/${_dep}.$<CONFIG>.stamp")
  endforeach()

  set(_stamp "${_GE_MANAGED_STAMP_DIR}/${target_name}.$<CONFIG>.stamp")

  # --unset=Platform: VS dev shells (vcvars64) export Platform=x64, and MSBuild
  # treats the env var as the build property — outputs would land in
  # bin/x64/<Config>/ instead of the bin/<Config>/ path every consumer expects.
  add_custom_command(
    OUTPUT "${_stamp}"
    COMMAND ${CMAKE_COMMAND} -E env --unset=TargetName --unset=TARGETNAME
            --unset=Platform --unset=PLATFORM
            "${DOTNET_EXECUTABLE}" build "${GE_AMA_CSPROJ}"
            --configuration $<CONFIG>
            -nologo
            -v:minimal
            /p:UseSharedCompilation=true
    COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
    DEPENDS ${_sources} ${GE_AMA_INPUTS} ${_dep_stamps}
    COMMENT "${GE_AMA_COMMENT}"
    VERBATIM
  )

  set(_target_args "")
  if(GE_AMA_ALL)
    list(APPEND _target_args ALL)
  endif()
  add_custom_target(${target_name} ${_target_args} DEPENDS "${_stamp}")

  if(GE_AMA_DEPENDS_ON)
    add_dependencies(${target_name} ${GE_AMA_DEPENDS_ON})
  endif()
endfunction()

# Leaf ABI assemblies (no ProjectReferences to other wrapped assemblies) —
# build in parallel.
ge_add_managed_assembly(HotReloadAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/HotReload/GameEngine.HotReload.csproj
  COMMENT "Building core hot-reload assembly (GameEngine.HotReload.dll)")

ge_add_managed_assembly(CoreBridgeAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/GameEngine.CoreBridge.csproj
  COMMENT "Building CoreBridge assembly (GameEngine.CoreBridge.dll)")

ge_add_managed_assembly(InputAbiAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Input.ABI/GameEngine.Input.ABI.csproj
  COMMENT "Building Input ABI assembly (GameEngine.Input.ABI.dll)")

# Editor Scripting ABI: not part of ALL — Editor.Managed builds it transitively
# via ProjectReference, so adding it to the default build would race.
if(BUILD_EDITOR)
  ge_add_managed_assembly(EditorScriptingAbiAssembly
    CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Editor.Scripting.ABI/GameEngine.Editor.Scripting.ABI.csproj
    COMMENT "Building Editor Scripting ABI assembly (GameEngine.Editor.Scripting.ABI.dll)")
endif()

ge_add_managed_assembly(EcsAbiAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/ECS.ABI/GameEngine.ECS.ABI.csproj
  COMMENT "Building ECS ABI assembly (GameEngine.ECS.ABI.dll)")

ge_add_managed_assembly(PhysicsAbiAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Physics.ABI/GameEngine.Physics.ABI.csproj
  COMMENT "Building Physics ABI assembly (GameEngine.Physics.ABI.dll)")

ge_add_managed_assembly(PlatformAbiAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Platform.ABI/GameEngine.Platform.ABI.csproj
  COMMENT "Building Platform ABI assembly (GameEngine.Platform.ABI.dll)")

ge_add_managed_assembly(EntitySystemGeneratorAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/SourceGenerators/EntitySystemGenerator/EntitySystemGenerator.csproj
  COMMENT "Building EntitySystemGenerator (source generator for IEntitySystem)")

# Dependent assemblies — chain mirrors the .csproj ProjectReference graph.
# The edges are load-bearing: each wrapper's `dotnet build` compiles its
# whole ProjectReference closure, so two unserialized wrappers that share a
# project race on its obj/ output under parallel MSBuild (CS2012).
ge_add_managed_assembly(ScriptingAbiAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Scripting.ABI/GameEngine.Scripting.ABI.csproj
  COMMENT "Building Scripting.ABI assembly (GameEngine.Scripting.ABI.dll)"
  DEPENDS_ON EcsAbiAssembly)

ge_add_managed_assembly(ScriptingRuntimeAssembly ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/GameEngine.Scripting.Runtime.csproj
  COMMENT "Building Scripting.Runtime assembly (GameEngine.Scripting.Runtime.dll)"
  DEPENDS_ON ScriptingAbiAssembly EcsAbiAssembly)

if(BUILD_EDITOR)
  ge_add_managed_assembly(EditorManagedAssembly ALL
    CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/Editor.Managed/GameEngine.Editor.Managed.csproj
    COMMENT "Building Editor managed integration assembly (GameEngine.Editor.Managed.dll)"
    DEPENDS_ON ScriptingAbiAssembly EcsAbiAssembly InputAbiAssembly ScriptingRuntimeAssembly)
endif()

# The csproj generates the host's protocol version from CompileServerVersion.txt;
# a bump must rebuild the host the client then expects.
ge_add_managed_assembly(CompileServerHost ALL
  CSPROJ  ${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/GameEngine.CompileServerHost.csproj
  COMMENT "Building CompileServerHost (GameEngine.CompileServerHost.dll)"
  INPUTS  ${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/CompileServerVersion.txt
  DEPENDS_ON EntitySystemGeneratorAssembly)

