# AllocationHook.cmake
#
# Where the allocation counter's hook (Engine/Modules/Memory/Include/Memory/AllocationHook.inl,
# the global operator new/delete replacements) is compiled:
#
#   - every test executable, in every configuration (tests do not ship), through
#     AllocationHook.cpp, together with the counter itself (AllocationCounter.cpp, which
#     compiles empty in a target that links Engine or the Memory module and so already has
#     the one process counter);
#   - the production images Engine, Editor, Player, GameEngine.Native and EditorSDK through
#     ProductionAllocationHook.cpp, compiled in every configuration and empty unless
#     GE_DEBUG_INSTRUMENTATION is 1 (Debug, DebugFast): no Release production image carries
#     a replacement. The switch is in that source because the Xcode generator rejects
#     sources that vary by configuration.
#
# A test executable is an executable that ge_set_output_tests marks or that links a
# GoogleTest target directly. The pass runs once, deferred to the end of the top-level
# directory, so it sees executables from every subdirectory without each one opting in.

set(GE_ALLOCATION_HOOK_SOURCE "${CMAKE_SOURCE_DIR}/Engine/Modules/Memory/Source/AllocationHook.cpp")
set(GE_PRODUCTION_ALLOCATION_HOOK_SOURCE "${CMAKE_SOURCE_DIR}/Engine/Modules/Memory/Source/ProductionAllocationHook.cpp")
set(GE_ALLOCATION_COUNTER_SOURCE "${CMAKE_SOURCE_DIR}/Engine/Modules/Memory/Source/AllocationCounter.cpp")
set(GE_ALLOCATION_COUNTER_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/Engine/Modules/Memory/Include"
    "${CMAKE_SOURCE_DIR}/Engine/Modules/Memory/Testing"
    "${CMAKE_SOURCE_DIR}/Engine/Modules/Types/Include")

# The hook and counter sources define file-scope names (the replacements, the counter
# state) that a unity batch would merge with the target's own sources, so they always
# compile alone.
function(_ge_allocation_hook_compile_alone target_name)
    set_source_files_properties(${GE_ALLOCATION_HOOK_SOURCE} ${GE_PRODUCTION_ALLOCATION_HOOK_SOURCE}
        ${GE_ALLOCATION_COUNTER_SOURCE}
        TARGET_DIRECTORY ${target_name}
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endfunction()

# ge_add_production_allocation_hook(<target>)
#
# Compiles the hook into a production image in the configurations where
# GE_DEBUG_INSTRUMENTATION is 1. The image must link Engine (or be Engine).
function(ge_add_production_allocation_hook target_name)
    target_sources(${target_name} PRIVATE ${GE_PRODUCTION_ALLOCATION_HOOK_SOURCE})
    _ge_allocation_hook_compile_alone(${target_name})
endfunction()

function(_ge_collect_buildsystem_targets directory out_var)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _ge_collect_buildsystem_targets("${_subdirectory}" _nested)
        list(APPEND _targets ${_nested})
    endforeach()
    set(${out_var} ${_targets} PARENT_SCOPE)
endfunction()

function(_ge_is_test_executable target_name out_var)
    set(${out_var} FALSE PARENT_SCOPE)
    get_target_property(_type ${target_name} TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        return()
    endif()
    get_target_property(_marked ${target_name} GE_TEST_EXECUTABLE)
    get_target_property(_libraries ${target_name} LINK_LIBRARIES)
    if(_marked OR "${_libraries}" MATCHES "(^|;)(GTest::|gtest|gmock)")
        set(${out_var} TRUE PARENT_SCOPE)
    endif()
endfunction()

function(ge_add_allocation_hook_to_test_executables)
    _ge_collect_buildsystem_targets("${CMAKE_SOURCE_DIR}" _targets)
    set(_count 0)
    foreach(_target IN LISTS _targets)
        _ge_is_test_executable(${_target} _is_test)
        if(NOT _is_test)
            continue()
        endif()
        target_sources(${_target} PRIVATE ${GE_ALLOCATION_HOOK_SOURCE} ${GE_ALLOCATION_COUNTER_SOURCE})
        target_include_directories(${_target} PRIVATE ${GE_ALLOCATION_COUNTER_INCLUDE_DIRS})
        _ge_allocation_hook_compile_alone(${_target})
        math(EXPR _count "${_count} + 1")
    endforeach()
    message(STATUS "Allocation counter: hook compiled into ${_count} test executables")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL ge_add_allocation_hook_to_test_executables)

# The hook replaces the global operator new in every test executable and, under
# GE_DEBUG_INSTRUMENTATION, in the engine's images; mimalloc's override would be a second
# definition in the same image.
get_property(_ge_allocation_hook_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(ENABLE_MIMALLOC AND (BUILD_TESTING OR _ge_allocation_hook_multi_config OR
                        CMAKE_BUILD_TYPE MATCHES "^(Debug|DebugFast)$"))
    message(FATAL_ERROR
        "ENABLE_MIMALLOC conflicts with the allocation counter's hook "
        "(Engine/Modules/Memory/Include/Memory/AllocationHook.inl), which replaces operator new in every "
        "test executable and in Debug and DebugFast engine images. Configure with "
        "-DENABLE_MIMALLOC=OFF, or use mimalloc only in a single-configuration Release or "
        "RelWithDebInfo build with BUILD_TESTING=OFF.")
endif()
