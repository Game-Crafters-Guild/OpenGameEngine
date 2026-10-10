# FixupVulkanLoaderNames.cmake
#
# Ensures common Vulkan loader soname aliases exist in a macOS app bundle:
# - libvulkan.1.dylib
# - libvulkan.dylib
#
# Some GLFW versions (and other tooling) dlopen these exact names.
# vcpkg often installs a fully-versioned dylib (e.g. libvulkan.1.4.309.dylib)
# which may be copied into the bundle without the expected alias names.
#
# Required:
# -DGE_APP_EXE=/path/to/<App>.app/Contents/MacOS/<exe>
#

if(NOT APPLE)
  return()
endif()

if(NOT DEFINED GE_APP_EXE OR GE_APP_EXE STREQUAL "")
  message(FATAL_ERROR "FixupVulkanLoaderNames: GE_APP_EXE must be set (path to Contents/MacOS/<exe>)")
endif()
if(NOT EXISTS "${GE_APP_EXE}")
  message(FATAL_ERROR "FixupVulkanLoaderNames: executable not found: ${GE_APP_EXE}")
endif()

get_filename_component(_macos_dir "${GE_APP_EXE}" DIRECTORY)        # .../Contents/MacOS
get_filename_component(_contents_dir "${_macos_dir}" DIRECTORY)     # .../Contents
set(_frameworks_dir "${_contents_dir}/Frameworks")

if(NOT EXISTS "${_frameworks_dir}")
  message(STATUS "FixupVulkanLoaderNames: no Frameworks dir; skipping")
  return()
endif()

file(GLOB _cands "${_frameworks_dir}/libvulkan*.dylib")
if(_cands STREQUAL "")
  message(STATUS "FixupVulkanLoaderNames: no libvulkan*.dylib in Frameworks; skipping")
  return()
endif()

# Prefer an existing libvulkan.1.dylib, else prefer libvulkan.1.*.dylib, else first candidate.
set(_src "")
if(EXISTS "${_frameworks_dir}/libvulkan.1.dylib")
  set(_src "${_frameworks_dir}/libvulkan.1.dylib")
endif()

if(_src STREQUAL "")
  foreach(_c IN LISTS _cands)
    get_filename_component(_n "${_c}" NAME)
    if(_n MATCHES "^libvulkan\\.1\\..+\\.dylib$")
      set(_src "${_c}")
      break()
    endif()
  endforeach()
endif()

if(_src STREQUAL "")
  list(GET _cands 0 _src)
endif()

if(NOT EXISTS "${_src}")
  message(STATUS "FixupVulkanLoaderNames: no usable libvulkan candidate; skipping")
  return()
endif()

set(_dst_1 "${_frameworks_dir}/libvulkan.1.dylib")
set(_dst_plain "${_frameworks_dir}/libvulkan.dylib")

execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${_dst_1}" RESULT_VARIABLE _rc1)
if(NOT _rc1 EQUAL 0)
  message(FATAL_ERROR "FixupVulkanLoaderNames: failed creating '${_dst_1}' from '${_src}' (rc=${_rc1})")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dst_1}" "${_dst_plain}" RESULT_VARIABLE _rc2)
if(NOT _rc2 EQUAL 0)
  message(FATAL_ERROR "FixupVulkanLoaderNames: failed creating '${_dst_plain}' from '${_dst_1}' (rc=${_rc2})")
endif()

message(STATUS "FixupVulkanLoaderNames: OK (source=${_src})")

