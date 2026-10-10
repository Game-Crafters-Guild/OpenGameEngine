# StageMoltenVkAppBundle.cmake
#
# Build-time helper to stage MoltenVK into a macOS .app bundle so Vulkan works
# when launched from Finder/Dock (clean environment).
#
# Required (via -D on cmake -P):
# - GE_APP_EXE: full path to Contents/MacOS/<exe>
#
# Optional:
# - GE_REQUIRE_MOLTENVK: ON/OFF (default ON) - fail if MoltenVK cannot be found
# - GE_MOLTENVK_VERBOSE: ON/OFF (default OFF)
# - GE_MOLTENVK_SOURCE_LIB: explicit libMoltenVK.dylib to stage before auto-detected copies
# - GE_MOLTENVK_SOURCE_ICD: explicit MoltenVK_icd.json to use as the source manifest
# - VCPKG_INSTALLED_DIR / VCPKG_TARGET_TRIPLET: passed through when available
#

if(NOT APPLE)
  return()
endif()

if(NOT DEFINED GE_APP_EXE OR GE_APP_EXE STREQUAL "")
  message(FATAL_ERROR "StageMoltenVkAppBundle: GE_APP_EXE must be set (path to Contents/MacOS/<exe>)")
endif()
if(NOT EXISTS "${GE_APP_EXE}")
  message(FATAL_ERROR "StageMoltenVkAppBundle: executable not found: ${GE_APP_EXE}")
endif()

set(_require ON)
if(DEFINED GE_REQUIRE_MOLTENVK)
  set(_require "${GE_REQUIRE_MOLTENVK}")
endif()
set(_verbose OFF)
if(DEFINED GE_MOLTENVK_VERBOSE)
  set(_verbose "${GE_MOLTENVK_VERBOSE}")
endif()

get_filename_component(_macos_dir "${GE_APP_EXE}" DIRECTORY)        # .../Contents/MacOS
get_filename_component(_contents_dir "${_macos_dir}" DIRECTORY)     # .../Contents
get_filename_component(_app_dir "${_contents_dir}" DIRECTORY)       # .../*.app

set(_icd_dir "${_contents_dir}/Resources/vulkan/icd.d")
set(_dst_lib "${_icd_dir}/libMoltenVK.dylib")
set(_dst_json "${_icd_dir}/MoltenVK_icd.json")

# Note: We intentionally write the ICD json directly here (instead of spawning a
# nested `cmake -P WriteMoltenVkIcdJson.cmake`). In some environments, the nested
# script invocation can succeed (rc=0) but still fail to produce the output file.
# Keeping this self-contained makes staging deterministic.

function(_ge_exists_first _outVar)
  set(_found "")
  foreach(_p IN LISTS ARGN)
    if(NOT _found AND _p AND EXISTS "${_p}")
      set(_found "${_p}")
    endif()
  endforeach()
  set(${_outVar} "${_found}" PARENT_SCOPE)
endfunction()

set(_lib_cands "")
set(_icd_cands "")

# Explicit override, useful for local MoltenVK PR builds.
if(DEFINED GE_MOLTENVK_SOURCE_LIB AND NOT "${GE_MOLTENVK_SOURCE_LIB}" STREQUAL "")
  list(APPEND _lib_cands "${GE_MOLTENVK_SOURCE_LIB}")
endif()
if(DEFINED GE_MOLTENVK_SOURCE_ICD AND NOT "${GE_MOLTENVK_SOURCE_ICD}" STREQUAL "")
  list(APPEND _icd_cands "${GE_MOLTENVK_SOURCE_ICD}")
endif()

# Common Homebrew/system locations.
list(APPEND _lib_cands
  "/opt/homebrew/lib/libMoltenVK.dylib"
  "/usr/local/lib/libMoltenVK.dylib"
)
list(APPEND _icd_cands
  "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json"
  "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json"
  "/etc/vulkan/icd.d/MoltenVK_icd.json"
)

# Vulkan SDK (if present in this build environment).
if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
  list(APPEND _lib_cands
    "$ENV{VULKAN_SDK}/lib/libMoltenVK.dylib"
    "$ENV{VULKAN_SDK}/MoltenVK/dylib/macOS/libMoltenVK.dylib"
  )
  list(APPEND _icd_cands
    "$ENV{VULKAN_SDK}/share/vulkan/icd.d/MoltenVK_icd.json"
    "$ENV{VULKAN_SDK}/MoltenVK/icd/MoltenVK_icd.json"
  )
endif()

# Default LunarG user install: ~/VulkanSDK/<version>/macOS/...
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  file(GLOB _home_vk_libs
    "$ENV{HOME}/VulkanSDK/*/macOS/lib/libMoltenVK.dylib"
    "$ENV{HOME}/VulkanSDK/*/macOS/MoltenVK/dylib/macOS/libMoltenVK.dylib"
  )
  file(GLOB _home_vk_icds
    "$ENV{HOME}/VulkanSDK/*/macOS/share/vulkan/icd.d/MoltenVK_icd.json"
    "$ENV{HOME}/VulkanSDK/*/macOS/MoltenVK/icd/MoltenVK_icd.json"
  )
  list(APPEND _lib_cands ${_home_vk_libs})
  list(APPEND _icd_cands ${_home_vk_icds})
endif()

# vcpkg-provided MoltenVK (when installed).
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  list(APPEND _lib_cands
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libMoltenVK.dylib"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib/libMoltenVK.dylib"
  )
  list(APPEND _icd_cands
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/vulkan/icd.d/MoltenVK_icd.json"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/share/vulkan/icd.d/MoltenVK_icd.json"
  )
endif()

list(REMOVE_DUPLICATES _lib_cands)
list(REMOVE_DUPLICATES _icd_cands)

_ge_exists_first(_src_lib ${_lib_cands})
_ge_exists_first(_src_icd ${_icd_cands})

if(_verbose)
  message(STATUS "StageMoltenVkAppBundle: app=${_app_dir}")
  message(STATUS "StageMoltenVkAppBundle: exe=${GE_APP_EXE}")
  message(STATUS "StageMoltenVkAppBundle: icd_dir=${_icd_dir}")
  if(_src_lib)
    message(STATUS "StageMoltenVkAppBundle: using libMoltenVK from: ${_src_lib}")
  else()
    message(STATUS "StageMoltenVkAppBundle: libMoltenVK not found in candidates")
  endif()
  if(_src_icd)
    message(STATUS "StageMoltenVkAppBundle: using MoltenVK_icd.json from: ${_src_icd}")
  else()
    message(STATUS "StageMoltenVkAppBundle: MoltenVK_icd.json not found in candidates (will emit minimal)")
  endif()
endif()

if(NOT _src_lib)
  if(_require)
    message(FATAL_ERROR
      "StageMoltenVkAppBundle: libMoltenVK.dylib not found. "
      "Install the Vulkan SDK or Homebrew MoltenVK, or add MoltenVK via vcpkg, then rebuild.")
  else()
    message(STATUS "StageMoltenVkAppBundle: skipping (libMoltenVK.dylib not found)")
    return()
  endif()
endif()

file(MAKE_DIRECTORY "${_icd_dir}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src_lib}" "${_dst_lib}"
  RESULT_VARIABLE _rc_copy
)
if(NOT _rc_copy EQUAL 0)
  message(FATAL_ERROR "StageMoltenVkAppBundle: failed copying '${_src_lib}' -> '${_dst_lib}' (rc=${_rc_copy})")
endif()

# Always write an app-bundle-friendly ICD JSON whenever we stage the dylib.
# Use a path relative to the manifest's own dir ("./libMoltenVK.dylib") so the
# bundle is relocatable — the Vulkan loader resolves a relative library_path
# against the directory containing this json, which is exactly where we stage
# the dylib. The engine points VK_DRIVER_FILES at THIS bundle's json
# (VulkanDevice bootstrapMoltenVkIcd), so there is no ambiguity with other SDK
# copies. An absolute build-machine path would break the instant the .app is
# moved or copied to another Mac.
set(_desired_library_path "./libMoltenVK.dylib")
set(_content "")
if(_src_icd AND EXISTS "${_src_icd}")
  file(READ "${_src_icd}" _content)
else()
  set(_content [=[
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version": "1.0.0",
        "is_portability_driver": true
    }
}
]=])
endif()

# Patch library_path to point at the dylib staged alongside this manifest.
string(REGEX REPLACE "\"library_path\"[ \t]*:[ \t]*\"[^\"]*\"" "\"library_path\": \"${_desired_library_path}\"" _content "${_content}")

file(WRITE "${_dst_json}" "${_content}\n")

if(NOT EXISTS "${_dst_json}")
  message(FATAL_ERROR "StageMoltenVkAppBundle: expected MoltenVK_icd.json to exist but it was not created: ${_dst_json}")
endif()

message(STATUS "StageMoltenVkAppBundle: OK (${_app_dir})")
