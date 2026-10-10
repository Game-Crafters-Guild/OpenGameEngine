# StageManagedAssemblies.cmake
#
# Shared helper for staging managed (.NET) assemblies into a macOS .app bundle's
# Contents/Resources/Managed directory.
#
# On macOS both the Editor and Player keep Contents/MacOS code-only and stage
# their CoreBridge/HotReload/ABI assemblies as bundle *resources*. Keeping
# Contents/MacOS code-only avoids codesign treating non-code artifacts
# (*.deps.json, *.runtimeconfig.json, managed *.dll) as unsigned subcomponents.
#
# This function captures the common pattern: make the destination, whole-directory
# copy each source into it (copy_directory_if_different), and strip *.pdb from the
# bundle (those are not code-signable and surface as unsigned subcomponents).
# App-specific staging (individual-file copies, source-generator subdirs, cache
# pruning) stays at the call site as separate POST_BUILD commands.

# ge_stage_managed_assemblies_macos(<target> <dest_dir> SOURCE_DIRS <dir>...)
#
#   <target>     runnable target whose bundle receives the assemblies
#   <dest_dir>   destination directory (e.g. $<TARGET_FILE_DIR:T>/../Resources/Managed)
#   SOURCE_DIRS  one or more source directories copied whole into <dest_dir>
#
# Emits a single POST_BUILD add_custom_command on <target>. No-op off macOS.
function(ge_stage_managed_assemblies_macos target dest_dir)
  if(NOT APPLE)
    return()
  endif()

  if(NOT TARGET ${target})
    message(FATAL_ERROR "ge_stage_managed_assemblies_macos: target not found: ${target}")
  endif()

  cmake_parse_arguments(_GE_SMA "" "" "SOURCE_DIRS" ${ARGN})
  if(NOT _GE_SMA_SOURCE_DIRS)
    message(FATAL_ERROR "ge_stage_managed_assemblies_macos: SOURCE_DIRS must list at least one directory")
  endif()

  set(_commands
      COMMAND ${CMAKE_COMMAND} -E make_directory "${dest_dir}"
  )
  foreach(_src IN LISTS _GE_SMA_SOURCE_DIRS)
    list(APPEND _commands
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_src}" "${dest_dir}"
    )
  endforeach()
  # *.pdb files are not code-signable and surface as unsigned subcomponents;
  # strip them from the bundle before codesign runs.
  list(APPEND _commands
      COMMAND /bin/sh -c "find \"$<TARGET_FILE_DIR:${target}>/..\" -name '*.pdb' -delete || true"
  )

  add_custom_command(TARGET ${target} POST_BUILD
      ${_commands}
      COMMENT "Staging managed assemblies into ${target}.app Resources (macOS)"
      VERBATIM
  )
endfunction()
