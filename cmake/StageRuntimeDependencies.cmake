# StageRuntimeDependencies.cmake
#
# Script-mode helper used by POST_BUILD steps to copy runtime dependencies next to a host
# executable (or module) so it can run from its target output directory.
#
# Inputs (must be set by the caller before including this file):
# - GE_RUNTIME_DEPS_EXECUTABLE: full path to the built executable (or module) to scan
# - GE_RUNTIME_DEPS_DEST_DIR  : destination directory to copy dependencies into
# - GE_RUNTIME_DEPS_SEARCH_DIRS (optional): additional search directories (CMake list)
# - GE_RUNTIME_DEPS_VERBOSE (optional): ON/OFF

if(NOT DEFINED GE_RUNTIME_DEPS_EXECUTABLE OR GE_RUNTIME_DEPS_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "StageRuntimeDependencies: GE_RUNTIME_DEPS_EXECUTABLE must be set")
endif()

if(NOT EXISTS "${GE_RUNTIME_DEPS_EXECUTABLE}")
  message(FATAL_ERROR "StageRuntimeDependencies: executable not found: ${GE_RUNTIME_DEPS_EXECUTABLE}")
endif()

if(NOT DEFINED GE_RUNTIME_DEPS_DEST_DIR OR GE_RUNTIME_DEPS_DEST_DIR STREQUAL "")
  get_filename_component(GE_RUNTIME_DEPS_DEST_DIR "${GE_RUNTIME_DEPS_EXECUTABLE}" DIRECTORY)
endif()

file(MAKE_DIRECTORY "${GE_RUNTIME_DEPS_DEST_DIR}")

include("${CMAKE_CURRENT_LIST_DIR}/GeStagedCopy.cmake")
ge_staged_copy_cleanup("${GE_RUNTIME_DEPS_DEST_DIR}")

# glibc is an inseparable part of the target Linux system. Staging only the
# subset discovered by an executable creates an invalid mixed runtime as soon
# as a host library or an external runtime such as CoreCLR loads another glibc
# component. Remove stale copies from warm output trees and always resolve the
# complete glibc set from the host.
if(UNIX AND NOT APPLE)
  file(GLOB _ge_staged_glibc
      "${GE_RUNTIME_DEPS_DEST_DIR}/ld-linux*.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libanl.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libc.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libdl.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libm.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libpthread.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libresolv.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/librt.so*"
      "${GE_RUNTIME_DEPS_DEST_DIR}/libutil.so*")
  if(_ge_staged_glibc)
    file(REMOVE ${_ge_staged_glibc})
  endif()
endif()

set(_ge_search_dirs "")
if(DEFINED GE_RUNTIME_DEPS_SEARCH_DIRS)
  list(APPEND _ge_search_dirs ${GE_RUNTIME_DEPS_SEARCH_DIRS})
endif()
# Drop empty entries. The generator script uses $<$<CONFIG:Debug>:...> guards on
# the vcpkg debug-CRT dirs, which collapse to empty strings in non-Debug configs
# and leave `;;` placeholders in the serialized list. file(GET_RUNTIME_DEPENDENCIES)
# tolerates them, but filtering keeps the search list clean and avoids any future
# CMake behaviour change that might treat "" as the current working directory.
list(FILTER _ge_search_dirs EXCLUDE REGEX "^$")

# Refresh already-staged runtime libraries from the search dirs BEFORE the
# dependency scan. file(GET_RUNTIME_DEPENDENCIES) emulates the platform
# loader: on Windows a scanned module's imports resolve against the module's
# OWN directory before any DIRECTORIES entry. The executable is scanned in the
# staged destination, so its first-party imports (Engine.dll, EditorSDK.dll,
# ...) resolve to the PREVIOUS build's staged copies and the scan reads THEIR
# import tables. A dependency edge added since the last staging run (e.g.
# Engine.dll gaining a DirectXTex.dll import) is invisible to that first scan:
# the stale table names no such import, the new transitive DLL is never
# discovered, and the build stays green while the staged app dies at launch
# with STATUS_DLL_NOT_FOUND. Search-dir ORDER cannot prevent this — the
# own-directory probe runs before DIRECTORIES — so make the staged copies
# current first; the scan then reads current import tables everywhere. The
# next-newest staged state heals on the SECOND build, which is exactly the
# trap: warm trees miss a freshly merged dependency once per config, silently.
file(REAL_PATH "${GE_RUNTIME_DEPS_DEST_DIR}" _ge_pre_refresh_dest_real)
file(GLOB _ge_pre_refresh_staged
    "${GE_RUNTIME_DEPS_DEST_DIR}/*.dll"
    "${GE_RUNTIME_DEPS_DEST_DIR}/*.dylib"
    "${GE_RUNTIME_DEPS_DEST_DIR}/*.so")
foreach(_ge_pre_staged IN LISTS _ge_pre_refresh_staged)
  get_filename_component(_ge_pre_name "${_ge_pre_staged}" NAME)
  foreach(_ge_pre_dir IN LISTS _ge_search_dirs)
    if(NOT EXISTS "${_ge_pre_dir}/${_ge_pre_name}")
      continue()
    endif()
    # The destination itself is one of the search dirs — never "refresh" a
    # staged file from itself; keep looking for a real source further down.
    file(REAL_PATH "${_ge_pre_dir}" _ge_pre_dir_real)
    if(_ge_pre_dir_real STREQUAL _ge_pre_refresh_dest_real)
      continue()
    endif()
    ge_staged_copy("${_ge_pre_dir}/${_ge_pre_name}" "${GE_RUNTIME_DEPS_DEST_DIR}" "StageRuntimeDependencies")
    break()
  endforeach()
endforeach()

# Filter out OS/system libraries; we only want app-local deps.
# Note: We intentionally do NOT exclude MSVC runtime DLLs here because that choice
# is policy-dependent; however, most dev machines have the runtime installed.
set(_ge_pre_exclude_regexes
    # Windows system forwarding DLLs
    "api-ms-win-.*"
    "ext-ms-win-.*"
    # Common Windows system DLLs by name (case-insensitive via character classes).
    # These should be provided by the OS / redistributable and should never be staged.
    "[Aa][Dd][Vv][Aa][Pp][Ii]32\\.dll"
    "[Cc][Ff][Gg][Mm][Gg][Rr]32\\.dll"
    "[Cc][Oo][Mm][Bb][Aa][Ss][Ee]\\.dll"
    "[Cc][Oo][Mm][Dd][Ll][Gg]32\\.dll"
    "[Dd][Ww][Mm][Aa][Pp][Ii]\\.dll"
    "[Gg][Dd][Ii]32\\.dll"
    "[Ii][Pp][Hh][Ll][Pp][Aa][Pp][Ii]\\.dll"
    "[Kk][Ee][Rr][Nn][Ee][Ll]32\\.dll"
    "[Kk][Ee][Rr][Nn][Ee][Ll][Bb][Aa][Ss][Ee]\\.dll"
    "[Oo][Ll][Ee]32\\.dll"
    "[Oo][Ll][Ee][Aa][Uu][Tt]32\\.dll"
    "[Oo][Pp][Ee][Nn][Gg][Ll]32\\.dll"
    "[Ss][Hh][Ee][Ll][Ll]32\\.dll"
    "[Ss][Hh][Ll][Ww][Aa][Pp][Ii]\\.dll"
    "[Uu][Ss][Ee][Rr]32\\.dll"
    "[Uu][Ss][Pp]10\\.dll"
    "[Ww][Ss]2_32\\.dll"
)

set(_ge_post_exclude_regexes
    # Explicitly exclude Windows system32 and common system DLLs by name.
    # These should be provided by the OS / redistributable.
    ".*[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
    ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
)

if(UNIX AND NOT APPLE)
  list(APPEND _ge_pre_exclude_regexes
      "^ld-linux[^/]*\\.so(\\..*)?$"
      "^lib(anl|c|dl|m|pthread|resolv|rt|util)\\.so(\\..*)?$")
  list(APPEND _ge_post_exclude_regexes
      ".*/ld-linux[^/]*\\.so(\\..*)?$"
      ".*/lib(anl|c|dl|m|pthread|resolv|rt|util)\\.so(\\..*)?$")
endif()

#
# macOS note (Vulkan):
# Historically we excluded vcpkg's Vulkan loader dylibs here. That made app bundles
# accidentally depend on shell/Xcode environment (e.g. DYLD_LIBRARY_PATH) to locate
# libvulkan at runtime, which commonly causes Finder/Dock launches to "bounce and quit"
# with a dyld error that isn't visible to users.
#
# Keep bundles self-contained: allow staging libvulkan*.dylib when it is a runtime
# dependency of the executable/module.
#

if(GE_RUNTIME_DEPS_VERBOSE)
  message(STATUS "StageRuntimeDependencies: scanning '${GE_RUNTIME_DEPS_EXECUTABLE}'")
  message(STATUS "StageRuntimeDependencies: dest '${GE_RUNTIME_DEPS_DEST_DIR}'")
  if(_ge_search_dirs)
    message(STATUS "StageRuntimeDependencies: search dirs: ${_ge_search_dirs}")
  endif()
endif()

set(_ge_resolved "")
set(_ge_unresolved "")

function(_ge_stage_from_get_runtime_deps _mode_or_libs)
  set(_resolved "")
  set(_unresolved "")
  set(_conf_prefix "_ge_conflicts")
  unset(${_conf_prefix}_FILENAMES)

  if(_mode_or_libs STREQUAL "EXECUTABLE")
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${GE_RUNTIME_DEPS_EXECUTABLE}"
        DIRECTORIES ${_ge_search_dirs}
        RESOLVED_DEPENDENCIES_VAR _resolved
        UNRESOLVED_DEPENDENCIES_VAR _unresolved
        CONFLICTING_DEPENDENCIES_PREFIX ${_conf_prefix}
        PRE_EXCLUDE_REGEXES ${_ge_pre_exclude_regexes}
        POST_EXCLUDE_REGEXES ${_ge_post_exclude_regexes}
    )
  else()
    # _mode_or_libs is expected to be a CMake list of absolute library paths.
    file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES ${_mode_or_libs}
        DIRECTORIES ${_ge_search_dirs}
        RESOLVED_DEPENDENCIES_VAR _resolved
        UNRESOLVED_DEPENDENCIES_VAR _unresolved
        CONFLICTING_DEPENDENCIES_PREFIX ${_conf_prefix}
        PRE_EXCLUDE_REGEXES ${_ge_pre_exclude_regexes}
        POST_EXCLUDE_REGEXES ${_ge_post_exclude_regexes}
    )
  endif()

  # `file(GET_RUNTIME_DEPENDENCIES)` can encounter conflicts when two different directories
  # contain a dependency with the same filename (e.g., an already-staged dylib inside a
  # bundle and the "source" dylib from a toolchain directory). Without
  # CONFLICTING_DEPENDENCIES_PREFIX, CMake errors out. Here we pick deterministically.
  #
  # Policy:
  # - Prefer candidates that are NOT already inside GE_RUNTIME_DEPS_DEST_DIR (staging output).
  # - If multiple remain, prefer candidates under GE_RUNTIME_DEPS_SEARCH_DIRS order.
  # - Otherwise, pick the lexicographically-first path for determinism.
  set(_conf_files "${${_conf_prefix}_FILENAMES}")
  if(_conf_files)
    file(REAL_PATH "${GE_RUNTIME_DEPS_DEST_DIR}" _dest_real)
    foreach(_fname IN LISTS _conf_files)
      set(_var "${_conf_prefix}_${_fname}")
      set(_candidates "${${_var}}")

      if(GE_RUNTIME_DEPS_VERBOSE)
        message(STATUS "StageRuntimeDependencies: conflict '${_fname}': ${_candidates}")
      endif()

      set(_filtered "")
      foreach(_cand IN LISTS _candidates)
        if(EXISTS "${_cand}")
          file(REAL_PATH "${_cand}" _cand_real)
          string(FIND "${_cand_real}" "${_dest_real}/" _in_dest_idx)
          if(_in_dest_idx EQUAL 0)
            continue()
          endif()
        endif()
        list(APPEND _filtered "${_cand}")
      endforeach()

      set(_chosen "")
      list(LENGTH _filtered _filtered_len)
      if(_filtered_len EQUAL 1)
        list(GET _filtered 0 _chosen)
      elseif(_filtered_len GREATER 1)
        foreach(_dir IN LISTS _ge_search_dirs)
          foreach(_cand IN LISTS _filtered)
            string(FIND "${_cand}" "${_dir}/" _idx)
            if(_idx EQUAL 0)
              set(_chosen "${_cand}")
              break()
            endif()
          endforeach()
          if(_chosen)
            break()
          endif()
        endforeach()
      endif()

      if(NOT _chosen)
        # Fall back to deterministic choice among all candidates.
        set(_sorted "${_candidates}")
        list(SORT _sorted)
        if(_sorted)
          list(GET _sorted 0 _chosen)
        endif()
      endif()

      if(_chosen)
        # Ensure the resolved list contains only our chosen path.
        foreach(_cand IN LISTS _candidates)
          list(REMOVE_ITEM _resolved "${_cand}")
        endforeach()
        list(APPEND _resolved "${_chosen}")

        if(GE_RUNTIME_DEPS_VERBOSE)
          message(STATUS "StageRuntimeDependencies: conflict '${_fname}' -> chose '${_chosen}'")
        endif()
      else()
        message(WARNING "StageRuntimeDependencies: conflict '${_fname}' had no usable candidates: ${_candidates}")
      endif()
    endforeach()
  endif()

  foreach(_dep IN LISTS _resolved)
    if(EXISTS "${_dep}")
      get_filename_component(_dep_name "${_dep}" NAME)
      set(_dep_to_copy "${_dep}")

      # App-bundle dependency scans can resolve @rpath libraries to an
      # already-staged copy in Contents/Frameworks. Prefer a fresh build output
      # with the same filename from the search dirs so incremental builds do not
      # launch against stale bundled dylibs.
      if(EXISTS "${GE_RUNTIME_DEPS_DEST_DIR}/${_dep_name}")
        file(REAL_PATH "${GE_RUNTIME_DEPS_DEST_DIR}" _dest_real)
        file(REAL_PATH "${_dep}" _dep_real)
        string(FIND "${_dep_real}" "${_dest_real}/" _dep_in_dest_idx)
        if(_dep_in_dest_idx EQUAL 0)
          foreach(_dir IN LISTS _ge_search_dirs)
            if(EXISTS "${_dir}/${_dep_name}")
              file(REAL_PATH "${_dir}/${_dep_name}" _candidate_real)
              string(FIND "${_candidate_real}" "${_dest_real}/" _candidate_in_dest_idx)
              if(NOT _candidate_in_dest_idx EQUAL 0)
                set(_dep_to_copy "${_dir}/${_dep_name}")
                break()
              endif()
            endif()
          endforeach()
        endif()
      endif()

      ge_staged_copy("${_dep_to_copy}" "${GE_RUNTIME_DEPS_DEST_DIR}" "StageRuntimeDependencies")
      if(GE_RUNTIME_DEPS_VERBOSE)
        message(STATUS "StageRuntimeDependencies: staged ${_dep_name}")
      endif()
    endif()
  endforeach()

  set(_ge_resolved "${_resolved}" PARENT_SCOPE)
  set(_ge_unresolved "${_unresolved}" PARENT_SCOPE)
endfunction()

# First pass: scan the executable itself.
_ge_stage_from_get_runtime_deps("EXECUTABLE")

# Second pass (macOS): some bundles rely on @rpath deps but only expose an rpath to
# Contents/Frameworks. In that case `file(GET_RUNTIME_DEPENDENCIES)` cannot resolve
# @rpath/<name> until we locate and copy the library once.
set(_ge_second_pass_libs "")
set(_ge_unresolved_remaining "")
foreach(_u IN LISTS _ge_unresolved)
  if(_u MATCHES "^@rpath/(.+)$")
    set(_rel "${CMAKE_MATCH_1}")
    # Only handle simple dylib basenames here.
    if(NOT _rel MATCHES "/")
      set(_found "")
      foreach(_dir IN LISTS _ge_search_dirs)
        if(EXISTS "${_dir}/${_rel}")
          set(_found "${_dir}/${_rel}")
          break()
        endif()
      endforeach()

      if(_found)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_found}" "${GE_RUNTIME_DEPS_DEST_DIR}"
            RESULT_VARIABLE _copy_rc2
        )
        if(NOT _copy_rc2 EQUAL 0)
          message(WARNING "StageRuntimeDependencies: failed copying unresolved '${_found}' -> '${GE_RUNTIME_DEPS_DEST_DIR}' (rc=${_copy_rc2})")
          list(APPEND _ge_unresolved_remaining "${_u}")
        else()
          list(APPEND _ge_second_pass_libs "${_found}")
          if(GE_RUNTIME_DEPS_VERBOSE)
            message(STATUS "StageRuntimeDependencies: resolved ${_u} via ${_found}")
          endif()
        endif()
      else()
        list(APPEND _ge_unresolved_remaining "${_u}")
      endif()
    else()
      list(APPEND _ge_unresolved_remaining "${_u}")
    endif()
  else()
    list(APPEND _ge_unresolved_remaining "${_u}")
  endif()
endforeach()

if(_ge_second_pass_libs)
  # Scan deps of the located libraries and stage them.
  _ge_stage_from_get_runtime_deps("${_ge_second_pass_libs}")

  # If we staged a library into the destination dir, treat @rpath/<name> as resolved
  # when the staged file now exists. This suppresses noisy warnings in bundle builds.
  set(_ge_pruned "")
  foreach(_u IN LISTS _ge_unresolved_remaining _ge_unresolved)
    if(_u MATCHES "^@rpath/(.+)$")
      set(_rel "${CMAKE_MATCH_1}")
      if(NOT _rel MATCHES "/" AND EXISTS "${GE_RUNTIME_DEPS_DEST_DIR}/${_rel}")
        continue()
      endif()
    endif()
    list(APPEND _ge_pruned "${_u}")
  endforeach()
  list(REMOVE_DUPLICATES _ge_pruned)
  set(_ge_unresolved "${_ge_pruned}")
else()
  set(_ge_unresolved "${_ge_unresolved_remaining}")
endif()

if(_ge_unresolved)
  # Unresolved deps are common when delay-loading or relying on system-installed components.
  # Keep this as a warning (not fatal) so builds remain productive.
  message(WARNING "StageRuntimeDependencies: unresolved deps for '${GE_RUNTIME_DEPS_EXECUTABLE}': ${_ge_unresolved}")
endif()
