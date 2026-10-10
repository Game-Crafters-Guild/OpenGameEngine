# GeStagedCopy.cmake
#
# Shared copy helper for the runtime-staging scripts, robust to a destination
# locked by a running process. On Windows a loaded PE image cannot be
# overwritten or deleted, but it CAN be renamed. `cmake -E copy_if_different`
# short-circuits to success when the destination is already identical, so a
# copy failure means the staged file is BOTH stale AND locked — exactly the
# case where letting the build exit 0 ships a stale runtime: the relaunched
# process silently runs old code while the developer debugs a phantom.
#
# Strategy: rename the locked stale file aside (the running process keeps its
# old image mapped) and copy the fresh one into place — the build stays green
# AND the staged output is current for the next launch. If even the rename
# fails, FAIL the build loudly; a silently stale staged runtime is never OK.

# ge_staged_copy(<src-file> <dest-dir> <label>)
function(ge_staged_copy _src _dest_dir _label)
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${_dest_dir}"
      RESULT_VARIABLE _rc
  )
  if(_rc EQUAL 0)
    return()
  endif()

  get_filename_component(_name "${_src}" NAME)
  set(_dest "${_dest_dir}/${_name}")
  string(RANDOM LENGTH 8 _tag)
  file(RENAME "${_dest}" "${_dest}.stale-${_tag}" RESULT _rename_err)
  if(_rename_err)
    message(FATAL_ERROR
        "${_label}: staged '${_name}' in '${_dest_dir}' is STALE and cannot be "
        "replaced (copy rc=${_rc}; rename-aside failed: ${_rename_err}). A "
        "running process (Editor/Player?) is holding it — close it and rebuild. "
        "Failing the build instead of leaving a stale runtime staged.")
  endif()

  execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${_dest_dir}"
      RESULT_VARIABLE _rc2
  )
  if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR
        "${_label}: failed staging '${_src}' -> '${_dest_dir}' (rc=${_rc2}) even "
        "after renaming the locked destination aside. The staged runtime is "
        "STALE — failing the build instead of shipping it.")
  endif()

  message(STATUS
      "${_label}: '${_name}' was locked by a running process — staged the fresh "
      "copy via rename-aside ('${_name}.stale-${_tag}' will be cleaned up by a "
      "later build).")
endfunction()

# Best-effort cleanup of rename-aside leftovers. Files still mapped by a live
# process survive REMOVE silently and get another chance next build.
function(ge_staged_copy_cleanup _dest_dir)
  file(GLOB _ge_stale_leftovers "${_dest_dir}/*.stale-*")
  if(_ge_stale_leftovers)
    file(REMOVE ${_ge_stale_leftovers})
  endif()
endfunction()
