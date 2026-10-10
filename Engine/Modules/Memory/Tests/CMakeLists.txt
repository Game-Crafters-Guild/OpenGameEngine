# The allocation counter's own suites. Both receive the allocation hook and the Testing headers
# as test executables (cmake/AllocationHook.cmake).

find_package(GTest REQUIRED)

# The counter and its windows, over the Memory module's own copy of the counter. JobSystem gives
# the Process window a real worker to fork onto.
add_executable(MemoryTests
    AllocationCountScopeTests.cpp
    AllocationSiteTagTests.cpp
    ScopedAllocationFaultTests.cpp
)
target_compile_features(MemoryTests PRIVATE cxx_std_20)
target_link_libraries(MemoryTests PRIVATE Memory JobSystem Logger GTest::gtest GTest::gtest_main)
ge_set_output_tests(MemoryTests)
ge_stage_runtime_dependencies(MemoryTests)
add_test(NAME MemoryTests COMMAND $<TARGET_FILE:MemoryTests>)

# Engine's allocations, counted from a test executable. Registered only where they are counted:
# on Windows Engine.dll carries the hook only where GE_DEBUG_INSTRUMENTATION is 1, so a Release
# run would read a false zero; on macOS and Linux the replacements coalesce in every
# configuration.
add_executable(EngineAllocationHookTests EngineAllocationHookTests.cpp)
target_compile_features(EngineAllocationHookTests PRIVATE cxx_std_20)
target_link_libraries(EngineAllocationHookTests PRIVATE Engine GTest::gtest GTest::gtest_main)
ge_set_output_tests(EngineAllocationHookTests)
ge_stage_runtime_dependencies(EngineAllocationHookTests)
if(WIN32)
    add_test(NAME EngineAllocationHookTests CONFIGURATIONS Debug DebugFast
        COMMAND $<TARGET_FILE:EngineAllocationHookTests>)
else()
    add_test(NAME EngineAllocationHookTests COMMAND $<TARGET_FILE:EngineAllocationHookTests>)
endif()
