cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
  message(FATAL_ERROR "StageShaderPkgs.cmake requires -DSRC_DIR=... -DDST_DIR=...")
endif()

file(MAKE_DIRECTORY "${DST_DIR}")

# Copy .shaderpkg files (packaged shader bundles)
file(GLOB _PKGS RELATIVE "${SRC_DIR}" "${SRC_DIR}/*.shaderpkg")
foreach(_F IN LISTS _PKGS)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${SRC_DIR}/${_F}"
    "${DST_DIR}/${_F}")
endforeach()

# Copy individual .spv files (compiled SPIR-V shaders used by UI, etc.)
file(GLOB _SPVS RELATIVE "${SRC_DIR}" "${SRC_DIR}/*.spv")
foreach(_F IN LISTS _SPVS)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${SRC_DIR}/${_F}"
    "${DST_DIR}/${_F}")
endforeach()

