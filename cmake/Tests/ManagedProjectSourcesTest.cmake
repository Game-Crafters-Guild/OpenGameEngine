# Contract test for cmake/ManagedProjectSources.cmake, run by ctest as
#   cmake -DSCRIPT=<module> -DWORK=<scratch dir> -DGENERATOR=<generator>
#         [-DMAKE_PROGRAM=<make program>] -P <this file>
# A scratch CMake project lists a scratch managed project through the module
# and is built with the generator under test, so the build system's own
# re-glob decides whether CMake re-runs, exactly as in a real tree.
#
# Covered: the module included first from a subdirectory and then called from
# the parent, as the engine does; files dotnet writes under the project's obj/
# and bin/ do not re-run CMake at the next build; a source added in a new
# subdirectory does, and the list then holds it; the list holds every source
# at any depth (a nested bin/ included) and nothing under the top-level obj/,
# bin/ or a dot directory.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK OR NOT DEFINED GENERATOR)
    message(FATAL_ERROR "ManagedProjectSourcesTest.cmake: SCRIPT, WORK and GENERATOR are required")
endif()

file(REMOVE_RECURSE "${WORK}")
set(managed "${WORK}/Managed/Project")
set(lister "${WORK}/lister")
set(build "${WORK}/build")

foreach(source IN ITEMS
        Project.csproj Directory.Build.props Program.cs
        Internal/Deep/Nested.cs Internal/bin/NestedBin.cs Binding/Binding.cs Options/Options.cs
        .hidden/Hidden.cs)
    file(WRITE "${managed}/${source}" "")
endforeach()

# The engine includes the module from a subdirectory (Engine/) before the top
# level includes it again and calls it; the function must work from both.
file(WRITE "${lister}/first/CMakeLists.txt" "include(\"${SCRIPT}\")\n")
file(WRITE "${lister}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(ManagedProjectSourcesTest NONE)
add_subdirectory(first)
include(\"${SCRIPT}\")
ge_managed_project_sources(sources \"${managed}\")
file(APPEND \"\${CMAKE_BINARY_DIR}/configure-runs.txt\" \"run\\n\")
list(TRANSFORM sources REPLACE \"^${managed}/\" \"\")
list(SORT sources)
file(WRITE \"\${CMAKE_BINARY_DIR}/sources.txt\" \"\${sources}\")
add_custom_target(Touch ALL COMMAND \${CMAKE_COMMAND} -E touch \"\${CMAKE_BINARY_DIR}/built.txt\")
")

set(makeProgram "")
if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
    set(makeProgram "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} -G "${GENERATOR}" ${makeProgram} -S "${lister}" -B "${build}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "configuring the scratch project failed (${rc}):\n${out}${err}")
endif()

# build(<case>) — builds the scratch project and reports how many times CMake
# has configured it so far, in configureRuns.
function(build case)
    execute_process(COMMAND ${CMAKE_COMMAND} --build "${build}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${case}: the build failed (${rc}):\n${out}${err}")
    endif()
    file(STRINGS "${build}/configure-runs.txt" runs)
    list(LENGTH runs count)
    set(configureRuns ${count} PARENT_SCOPE)
endfunction()

build("first build")
if(NOT configureRuns EQUAL 1)
    message(FATAL_ERROR "first build: CMake ran ${configureRuns} times, expected only the configure")
endif()

# What `dotnet build` leaves in a project directory: generated sources and the
# NuGet imports under obj/, and the outputs under bin/.
foreach(output IN ITEMS
        obj/Debug/net9.0/Project.AssemblyInfo.cs obj/Debug/net9.0/Generated/Bindings.g.cs
        obj/Project.csproj.nuget.g.props obj/Project.csproj.nuget.g.targets
        bin/Debug/net9.0/Project.dll bin/Debug/net9.0/Analyzers.targets)
    file(WRITE "${managed}/${output}" "")
endforeach()
build("build after dotnet wrote obj/ and bin/")
if(NOT configureRuns EQUAL 1)
    message(FATAL_ERROR
        "files written under the managed project's obj/ and bin/ re-ran CMake at the next build "
        "(${configureRuns} configures): the source globs see build outputs, so every build after a "
        "dotnet build re-runs CMake, and inside an Xcode build that re-run resolves vcpkg's compiler "
        "afresh")
endif()

file(WRITE "${managed}/Feature/Added.cs" "")
build("build after a source was added in a new subdirectory")
if(NOT configureRuns EQUAL 2)
    message(FATAL_ERROR
        "a source added in a new subdirectory did not re-run CMake (${configureRuns} configures): "
        "the managed build would not see it until an unrelated reconfigure")
endif()

file(READ "${build}/sources.txt" sources)
set(expected
    Binding/Binding.cs Directory.Build.props Feature/Added.cs Internal/Deep/Nested.cs
    Internal/bin/NestedBin.cs Options/Options.cs Program.cs Project.csproj)
list(SORT expected)
if(NOT sources STREQUAL expected)
    message(FATAL_ERROR
        "the managed project's sources are wrong.\n  expected: ${expected}\n  listed:   ${sources}")
endif()
message(STATUS "ManagedProjectSourcesTest: all cases passed")
