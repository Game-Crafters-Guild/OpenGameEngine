# Contract test for cmake/VcpkgHostCompiler.cmake, run by ctest as
#   cmake -DSCRIPT=<module> -DWORK=<scratch dir> -P <this file>
# vcpkg finds its compiler the way any CMake project does: by detecting one in
# the environment it inherits. A scratch project holds the pair through the
# module and then starts two child projects that detect a C and a C++ compiler
# the way vcpkg's detect_compiler does: one inherits the configure's
# environment, as the vcpkg toolchain's manifest install at project() does; the
# other is started with the module's environment arguments from a process
# whose environment carries a cross compiler, as the vcpkg run after project()
# is. Each child reports what it found.
#
# Covered: a re-configure with another c++ first on PATH (what xcodebuild does
# to a configure it re-runs) hands both children the compilers the first
# configure resolved; and a CXX exported at the first configure is the one
# held afterwards, also when a re-configure no longer exports it.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "VcpkgHostCompilerTest.cmake: SCRIPT and WORK are required")
endif()

file(REMOVE_RECURSE "${WORK}")
find_program(systemCxx NAMES c++ REQUIRED NO_CACHE)
find_program(systemCc NAMES cc REQUIRED NO_CACHE)

# A working compiler at another path, as xcodebuild's toolchain directory is:
# a different file, and so a different compiler hash for vcpkg.
function(make_wrapper_toolchain dir)
    file(MAKE_DIRECTORY "${dir}")
    foreach(pair IN ITEMS "c++|${systemCxx}" "cc|${systemCc}")
        string(REPLACE "|" ";" pair "${pair}")
        list(GET pair 0 name)
        list(GET pair 1 target)
        file(WRITE "${dir}/${name}" "#!/bin/sh\nexec \"${target}\" \"$@\"\n")
        file(CHMOD "${dir}/${name}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endforeach()
endfunction()
make_wrapper_toolchain("${WORK}/xcode-toolchain")
make_wrapper_toolchain("${WORK}/exported")
make_wrapper_toolchain("${WORK}/cross")

file(WRITE "${WORK}/detect/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(DetectCompiler LANGUAGES C CXX)
file(WRITE \"\${OUT}\" \"\${CMAKE_C_COMPILER};\${CMAKE_CXX_COMPILER}\")
")

file(WRITE "${WORK}/holder/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
include(\"${SCRIPT}\")
ge_vcpkg_hold_host_compilers()
project(VcpkgHostCompilerTest NONE)
foreach(child IN ITEMS inherited explicit)
    file(REMOVE_RECURSE \"\${CMAKE_BINARY_DIR}/\${child}\")
endforeach()
execute_process(
    COMMAND \${CMAKE_COMMAND} -G \"Unix Makefiles\" -S \"${WORK}/detect\" -B \"\${CMAKE_BINARY_DIR}/inherited\"
            \"-DOUT=\${CMAKE_BINARY_DIR}/inherited.txt\"
    OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
set(ENV{CC} \"${WORK}/cross/cc\")
set(ENV{CXX} \"${WORK}/cross/c++\")
ge_vcpkg_host_compiler_env(environment)
execute_process(
    COMMAND \${CMAKE_COMMAND} -E env \${environment}
            \${CMAKE_COMMAND} -G \"Unix Makefiles\" -S \"${WORK}/detect\" -B \"\${CMAKE_BINARY_DIR}/explicit\"
            \"-DOUT=\${CMAKE_BINARY_DIR}/explicit.txt\"
    OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
")

# configure(<build dir> <env arguments...>) — configures the holder project in
# ${build} under `cmake -E env <env arguments>`, and reports what each child
# detected as "<cc>;<c++>" in inherited and explicit.
function(configure build)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env --unset=CC --unset=CXX ${ARGN}
                ${CMAKE_COMMAND} -S "${WORK}/holder" -B "${build}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "configuring ${build} failed (${rc}):\n${out}${err}")
    endif()
    file(READ "${build}/inherited.txt" found)
    set(inherited "${found}" PARENT_SCOPE)
    file(READ "${build}/explicit.txt" found)
    set(explicit "${found}" PARENT_SCOPE)
endfunction()

function(expect case found expected)
    if(NOT found STREQUAL expected)
        message(FATAL_ERROR "${case}: vcpkg would detect '${found}', expected '${expected}'")
    endif()
endfunction()

set(xcodePath "PATH=${WORK}/xcode-toolchain:$ENV{PATH}")

configure("${WORK}/from-shell")
set(shellPair "${inherited}")
if(shellPair MATCHES "xcode-toolchain|cross")
    message(FATAL_ERROR "the first configure detected a scratch compiler: ${shellPair}")
endif()
expect("first configure, the run after project()" "${explicit}" "${shellPair}")

configure("${WORK}/from-shell" "${xcodePath}")
expect("re-configure with another c++ first on PATH, the toolchain's install" "${inherited}" "${shellPair}")
expect("re-configure with another c++ first on PATH, the run after project()" "${explicit}" "${shellPair}")

set(exportedPair "${WORK}/exported/cc;${WORK}/exported/c++")
configure("${WORK}/exported-at-first" "CC=${WORK}/exported/cc" "CXX=${WORK}/exported/c++")
expect("first configure with CC and CXX exported" "${inherited}" "${exportedPair}")
configure("${WORK}/exported-at-first" "${xcodePath}")
expect("re-configure without CC and CXX, the toolchain's install" "${inherited}" "${exportedPair}")
expect("re-configure without CC and CXX, the run after project()" "${explicit}" "${exportedPair}")

message(STATUS "VcpkgHostCompilerTest: all cases passed")
