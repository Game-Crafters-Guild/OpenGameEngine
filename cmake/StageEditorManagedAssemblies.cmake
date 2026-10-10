if(NOT DEFINED GE_SOURCE_DIR OR NOT DEFINED GE_MANAGED_DEST OR NOT DEFINED GE_CONFIG)
  message(FATAL_ERROR "GE_SOURCE_DIR, GE_MANAGED_DEST, and GE_CONFIG are required")
endif()

file(MAKE_DIRECTORY "${GE_MANAGED_DEST}")

set(_whole_directories
  "${GE_SOURCE_DIR}/Managed/CoreBridge/bin/${GE_CONFIG}/net10.0"
  "${GE_SOURCE_DIR}/Managed/HotReload/bin/${GE_CONFIG}/net10.0"
  "${GE_SOURCE_DIR}/Managed/Editor.Scripting.ABI/bin/${GE_CONFIG}"
  "${GE_SOURCE_DIR}/Managed/ECS.ABI/bin/${GE_CONFIG}"
  "${GE_SOURCE_DIR}/Managed/Input.ABI/bin/${GE_CONFIG}"
  "${GE_SOURCE_DIR}/Managed/Physics.ABI/bin/${GE_CONFIG}"
  "${GE_SOURCE_DIR}/Managed/Platform.ABI/bin/${GE_CONFIG}"
  "${GE_SOURCE_DIR}/Managed/Editor.Managed/bin/${GE_CONFIG}"
  "${GE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/${GE_CONFIG}"
)
foreach(_source_dir IN LISTS _whole_directories)
  if(NOT IS_DIRECTORY "${_source_dir}")
    message(FATAL_ERROR "Managed assembly directory is missing: ${_source_dir}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${_source_dir}" "${GE_MANAGED_DEST}"
    COMMAND_ERROR_IS_FATAL ANY)
endforeach()

file(MAKE_DIRECTORY "${GE_MANAGED_DEST}/SourceGenerators")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${GE_SOURCE_DIR}/Managed/Scripting.ABI/bin/${GE_CONFIG}/GameEngine.Scripting.ABI.dll"
  "${GE_MANAGED_DEST}/GameEngine.Scripting.ABI.dll" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${GE_SOURCE_DIR}/Managed/Scripting.ABI/bin/${GE_CONFIG}/GameEngine.Scripting.ABI.deps.json"
  "${GE_MANAGED_DEST}/GameEngine.Scripting.ABI.deps.json" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${GE_SOURCE_DIR}/Managed/SourceGenerators/EntitySystemGenerator/bin/${GE_CONFIG}/netstandard2.0/EntitySystemGenerator.dll"
  "${GE_MANAGED_DEST}/SourceGenerators/EntitySystemGenerator.dll" COMMAND_ERROR_IS_FATAL ANY)

set(_compile_server_source
  "${GE_SOURCE_DIR}/Managed/CompileServerHost/bin/${GE_CONFIG}/net10.0")
set(_compile_server_dest
  "${GE_MANAGED_DEST}/CompileServerHost/bin/${GE_CONFIG}/net10.0")
file(REMOVE_RECURSE "${_compile_server_dest}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
          "${_compile_server_source}" "${_compile_server_dest}"
  COMMAND_ERROR_IS_FATAL ANY)
# Runtime logs are never bundle resources. Older build trees may still contain
# one from before CompileServerHost moved logging to GE_LOGFILE's directory.
file(REMOVE "${_compile_server_dest}/CompileServerHost.log")

if(EXISTS "${_compile_server_dest}/CompileServerHost.dll"
   AND NOT EXISTS "${_compile_server_dest}/GameEngine.CompileServerHost.dll")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${_compile_server_dest}/CompileServerHost.dll"
    "${_compile_server_dest}/GameEngine.CompileServerHost.dll"
    COMMAND_ERROR_IS_FATAL ANY)
endif()
if(EXISTS "${_compile_server_dest}/GameEngine.CompileServerHost")
  file(CHMOD "${_compile_server_dest}/GameEngine.CompileServerHost"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

file(GLOB_RECURSE _pdb_files "${GE_MANAGED_DEST}/*.pdb")
file(GLOB_RECURSE _roslyn_satellites
  "${GE_MANAGED_DEST}/Microsoft.CodeAnalysis*.resources.dll")
file(REMOVE ${_pdb_files} ${_roslyn_satellites})
file(REMOVE_RECURSE
  "${_compile_server_dest}/zh-Hans"
  "${_compile_server_dest}/zh-Hant")

get_filename_component(_resources_dir "${GE_MANAGED_DEST}" DIRECTORY)
get_filename_component(_contents_dir "${_resources_dir}" DIRECTORY)
file(REMOVE_RECURSE
  "${_contents_dir}/MacOS/.Cache"
  "${_contents_dir}/MacOS/ScriptAssemblies"
  "${_contents_dir}/MacOS/zh-Hans"
  "${_contents_dir}/MacOS/zh-Hant")
file(GLOB _legacy_managed_files
  "${_contents_dir}/MacOS/*.dll"
  "${_contents_dir}/MacOS/*.json"
  "${_contents_dir}/MacOS/*.pdb"
  "${_contents_dir}/MacOS/*.xml")
file(REMOVE ${_legacy_managed_files})
