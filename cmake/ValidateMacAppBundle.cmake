# ValidateMacAppBundle.cmake
#
# Post-build verification for macOS .app bundles.
# Intended to run on macOS only (guarded by APPLE).
#
# Inputs (via -D on cmake -P):
# - GE_APP_EXE: Full path to the executable inside the app bundle (Contents/MacOS/<name>)
#
# Optional:
# - GE_VALIDATE_VERBOSE: ON/OFF (default OFF)
#

if(NOT APPLE)
  message(STATUS "ValidateMacAppBundle: non-Apple platform; skipping")
  return()
endif()

set(_verbose OFF)
if(DEFINED GE_VALIDATE_VERBOSE)
  set(_verbose "${GE_VALIDATE_VERBOSE}")
endif()

if(DEFINED ENV{GE_SKIP_MAC_BUNDLE_VALIDATION})
  if(NOT "$ENV{GE_SKIP_MAC_BUNDLE_VALIDATION}" STREQUAL "" AND NOT "$ENV{GE_SKIP_MAC_BUNDLE_VALIDATION}" STREQUAL "0")
    message(STATUS "ValidateMacAppBundle: skipped (GE_SKIP_MAC_BUNDLE_VALIDATION is set)")
    return()
  endif()
endif()

if(NOT DEFINED GE_APP_EXE OR GE_APP_EXE STREQUAL "")
  message(FATAL_ERROR "ValidateMacAppBundle: GE_APP_EXE must be set (path to Contents/MacOS/<exe>)")
endif()

set(_exe "${GE_APP_EXE}")
if(NOT EXISTS "${_exe}")
  message(FATAL_ERROR "ValidateMacAppBundle: executable not found: ${_exe}")
endif()

get_filename_component(_macos_dir "${_exe}" DIRECTORY)       # .../Contents/MacOS
get_filename_component(_contents_dir "${_macos_dir}" DIRECTORY) # .../Contents
get_filename_component(_app_dir "${_contents_dir}" DIRECTORY)   # .../*.app

set(_resources_dir "${_contents_dir}/Resources")
set(_frameworks_dir "${_contents_dir}/Frameworks")

if(_verbose)
  message(STATUS "ValidateMacAppBundle: exe=${_exe}")
  message(STATUS "ValidateMacAppBundle: app=${_app_dir}")
  message(STATUS "ValidateMacAppBundle: contents=${_contents_dir}")
  message(STATUS "ValidateMacAppBundle: resources=${_resources_dir}")
  message(STATUS "ValidateMacAppBundle: frameworks=${_frameworks_dir}")
endif()

if(NOT _app_dir MATCHES "\\.app$")
  message(FATAL_ERROR "ValidateMacAppBundle: expected a .app bundle directory, got: ${_app_dir}")
endif()

foreach(_dir IN ITEMS "${_contents_dir}" "${_macos_dir}" "${_resources_dir}")
  if(NOT EXISTS "${_dir}")
    message(FATAL_ERROR "ValidateMacAppBundle: missing required directory: ${_dir}")
  endif()
endforeach()

# Standard bundle data layout checks
set(_assets_dir "${_resources_dir}/Assets")
set(_shaders_dir "${_assets_dir}/Shaders")
if(NOT EXISTS "${_assets_dir}")
  message(FATAL_ERROR "ValidateMacAppBundle: missing Resources/Assets directory: ${_assets_dir}")
endif()
if(NOT EXISTS "${_shaders_dir}")
  message(FATAL_ERROR "ValidateMacAppBundle: missing Resources/Assets/Shaders directory: ${_shaders_dir}")
endif()

# Old layout should not exist (or should be empty).
if(EXISTS "${_macos_dir}/Assets")
  message(FATAL_ERROR "ValidateMacAppBundle: found legacy directory inside Contents/MacOS: ${_macos_dir}/Assets (should be in Contents/Resources)")
endif()
if(EXISTS "${_macos_dir}/Shaders")
  message(FATAL_ERROR "ValidateMacAppBundle: found legacy directory inside Contents/MacOS: ${_macos_dir}/Shaders (should be in Contents/Resources)")
endif()
if(EXISTS "${_resources_dir}/Shaders")
  message(FATAL_ERROR "ValidateMacAppBundle: found legacy Resources/Shaders directory: ${_resources_dir}/Shaders (shaders are now under Resources/Assets/Shaders)")
endif()

# Shared-identity managed assemblies (HotReloadManager.s_SharedAssemblyNames):
# each must load from the default ALC at startup, so each must be staged.
# Only checked when the bundle has a Managed dir (scripting-enabled builds).
set(_managed_dir "${_resources_dir}/Managed")
if(EXISTS "${_managed_dir}")
  foreach(_shared IN ITEMS
      GameEngine.Scripting.ABI
      GameEngine.ECS.ABI
      GameEngine.Input.ABI
      GameEngine.Physics.ABI
      GameEngine.Platform.ABI
      GameEngine.Editor.Scripting.ABI
      GameEngine.Scripting.Runtime
      GameEngine.CoreBridge
      GameEngine.HotReload)
    if(NOT EXISTS "${_managed_dir}/${_shared}.dll")
      message(FATAL_ERROR "ValidateMacAppBundle: missing shared managed assembly: ${_managed_dir}/${_shared}.dll (cross-ALC type identity breaks without it; check StageEditorManagedAssemblies.cmake)")
    endif()
  endforeach()
endif()

# Ensure Frameworks exists for bundled dylibs.
if(NOT EXISTS "${_frameworks_dir}")
  message(FATAL_ERROR "ValidateMacAppBundle: missing Contents/Frameworks directory: ${_frameworks_dir}")
endif()

file(GLOB _dylibs "${_frameworks_dir}/*.dylib")
file(GLOB _framework_dirs "${_frameworks_dir}/*.framework")

if(_verbose)
  list(LENGTH _dylibs _dylib_count)
  list(LENGTH _framework_dirs _framework_count)
  message(STATUS "ValidateMacAppBundle: Frameworks dylibs=${_dylib_count} frameworks=${_framework_count}")
endif()

# Require at least one bundled code object in Frameworks (Editor is non-trivial; if this is empty something went wrong).
if(_dylibs STREQUAL "" AND _framework_dirs STREQUAL "")
  message(FATAL_ERROR "ValidateMacAppBundle: Contents/Frameworks is empty; expected staged dylibs/frameworks for a self-contained bundle")
endif()

function(_ge_run_otool_L _bin _outVar)
  execute_process(
    COMMAND otool -L "${_bin}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "ValidateMacAppBundle: otool -L failed for '${_bin}': ${_err}")
  endif()
  set(${_outVar} "${_out}" PARENT_SCOPE)
endfunction()

function(_ge_run_otool_l _bin _outVar)
  execute_process(
    COMMAND otool -l "${_bin}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "ValidateMacAppBundle: otool -l failed for '${_bin}': ${_err}")
  endif()
  set(${_outVar} "${_out}" PARENT_SCOPE)
endfunction()

function(_ge_check_allowed_deps _bin)
  _ge_run_otool_L("${_bin}" _deps)

  string(REPLACE "\r\n" "\n" _deps "${_deps}")
  string(REPLACE "\r" "\n" _deps "${_deps}")
  string(REGEX REPLACE "\n+$" "" _deps "${_deps}")
  string(REPLACE "\n" ";" _lines "${_deps}")

  # First line is '<bin>:'; skip it.
  list(LENGTH _lines _n)
  if(_n GREATER 0)
    list(REMOVE_AT _lines 0)
  endif()

  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _s)
    if(_s STREQUAL "")
      continue()
    endif()

    # dep path is up to first whitespace
    string(REGEX MATCH "^([^ ]+)" _m "${_s}")
    set(_dep "${CMAKE_MATCH_1}")
    if(_dep STREQUAL "")
      continue()
    endif()

    # Robustness: Only consider actual dyld-style dependency paths.
    # (Some toolchains can inject non-dependency text; we should ignore it.)
    if(NOT (_dep MATCHES "^@" OR _dep MATCHES "^/"))
      continue()
    endif()

    # Allow rpath-style deps and system libraries.
    if(_dep MATCHES "^@rpath/" OR _dep MATCHES "^@executable_path/" OR _dep MATCHES "^@loader_path/")
      continue()
    endif()
    if(_dep MATCHES "^/usr/lib/" OR _dep MATCHES "^/System/Library/")
      continue()
    endif()

    # Reject everything else (absolute paths into build trees, package managers, etc).
    message(FATAL_ERROR
      "ValidateMacAppBundle: invalid dependency '${_dep}' in '${_bin}'. "
      "Bundle is not self-contained; expected @rpath/@executable_path/@loader_path or system libs only.")
  endforeach()
endfunction()

function(_ge_otool_has_dep _bin _needle _outVar)
  _ge_run_otool_L("${_bin}" _deps)
  string(REPLACE "\r\n" "\n" _deps "${_deps}")
  string(REPLACE "\r" "\n" _deps "${_deps}")
  string(FIND "${_deps}" "${_needle}" _idx)
  if(_idx EQUAL -1)
    set(${_outVar} FALSE PARENT_SCOPE)
  else()
    set(${_outVar} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(_ge_check_has_rpath _bin _expected)
  _ge_run_otool_l("${_bin}" _lout)
  string(FIND "${_lout}" "${_expected}" _idx)
  if(_idx EQUAL -1)
    message(FATAL_ERROR "ValidateMacAppBundle: '${_bin}' is missing LC_RPATH entry '${_expected}'")
  endif()
endfunction()

# Executable must load Frameworks via rpath.
_ge_check_has_rpath("${_exe}" "@executable_path/../Frameworks")

# Ensure executable and bundled dylibs only reference allowed deps.
_ge_check_allowed_deps("${_exe}")
foreach(_d IN LISTS _dylibs)
  _ge_check_allowed_deps("${_d}")
  # Dylibs should be able to resolve @rpath deps within Frameworks; ensure @loader_path is present.
  _ge_check_has_rpath("${_d}" "@loader_path")
endforeach()

# Vulkan ICD staging:
# The Editor's renderer uses Vulkan-on-Metal (MoltenVK). Finder launches should not
# rely on environment variables or a globally installed Vulkan SDK.
# If the bundle contains/depends on a Vulkan loader, require bundled MoltenVK ICD.
set(_needs_vulkan_icd FALSE)
_ge_otool_has_dep("${_exe}" "libvulkan" _has_vulkan)
if(_has_vulkan)
  set(_needs_vulkan_icd TRUE)
endif()
if(EXISTS "${_frameworks_dir}/libvulkan.dylib" OR EXISTS "${_frameworks_dir}/libvulkan.1.dylib")
  set(_needs_vulkan_icd TRUE)
endif()

set(_molten_icd "${_resources_dir}/vulkan/icd.d/MoltenVK_icd.json")
set(_molten_lib "${_resources_dir}/vulkan/icd.d/libMoltenVK.dylib")
if(_needs_vulkan_icd)
  if(NOT EXISTS "${_molten_icd}")
    message(FATAL_ERROR "ValidateMacAppBundle: bundle requires Vulkan but MoltenVK_icd.json is missing: ${_molten_icd}")
  endif()
  if(NOT EXISTS "${_molten_lib}")
    message(FATAL_ERROR "ValidateMacAppBundle: bundle requires Vulkan but libMoltenVK.dylib is missing: ${_molten_lib}")
  endif()
endif()

message(STATUS "ValidateMacAppBundle: OK (${_app_dir})")

