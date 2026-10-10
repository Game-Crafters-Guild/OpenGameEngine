# Logger Async Tests

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest_main)
    endif()
    if(NOT GTEST_FOUND)
        message(STATUS "GTest not found, skipping Logger tests")
        return()
    endif()
endif()

add_executable(LoggerAsyncTests
    LoggerAsyncTests.cpp
    LoggerOwnershipTests.cpp
    DeferredSinkTests.cpp
    FileLogLevelTests.cpp
    FileSinkFormatTests.cpp
    FileSinkRotationTests.cpp
    FileSinkOpenFailureTests.cpp
    LoggerCrashFlushTests.cpp
    SourceCStringTests.cpp
)

target_link_libraries(LoggerAsyncTests
    PRIVATE
        Logger
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(LoggerAsyncTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

ge_set_output_tests(LoggerAsyncTests)
ge_stage_runtime_dependencies(LoggerAsyncTests)

add_test(NAME LoggerAsyncTests COMMAND LoggerAsyncTests)

set_tests_properties(LoggerAsyncTests PROPERTIES
    TIMEOUT 60
    LABELS "Logger;Unit;Async"
)

if(WIN32)
    # A persistent logger DLL and an independently unloadable producer use the
    # same arrangement as Engine.dll and a generated native game module.
    # Compile the authoritative source list directly: Visual Studio's automatic
    # export scan does not walk objects supplied through TARGET_OBJECTS.
    get_target_property(_logger_source_dir Logger SOURCE_DIR)
    get_target_property(_logger_test_host_sources Logger SOURCES)
    list(TRANSFORM _logger_test_host_sources PREPEND "${_logger_source_dir}/")
    add_library(LoggerModuleTestHost SHARED ${_logger_test_host_sources})
    set_target_properties(LoggerModuleTestHost PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_include_directories(LoggerModuleTestHost PUBLIC ../Include)
    target_include_directories(LoggerModuleTestHost PRIVATE $<TARGET_PROPERTY:Logger,INCLUDE_DIRECTORIES>)
    target_compile_definitions(LoggerModuleTestHost PRIVATE $<TARGET_PROPERTY:Logger,COMPILE_DEFINITIONS>)
    target_compile_options(LoggerModuleTestHost PRIVATE $<TARGET_PROPERTY:Logger,COMPILE_OPTIONS>)
    target_compile_features(LoggerModuleTestHost PUBLIC cxx_std_20)
    target_link_libraries(LoggerModuleTestHost PRIVATE dbghelp)
    ge_set_output_tests(LoggerModuleTestHost)

    add_library(LoggerModuleProbe SHARED LoggerModuleProbe.cpp)
    target_link_libraries(LoggerModuleProbe PRIVATE LoggerModuleTestHost)
    ge_set_output_tests(LoggerModuleProbe)

    add_executable(LoggerModuleLifetimeTests LoggerModuleLifetimeTests.cpp)
    target_compile_definitions(LoggerModuleLifetimeTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(LoggerModuleLifetimeTests PRIVATE LoggerModuleTestHost GTest::gtest_main)
    add_dependencies(LoggerModuleLifetimeTests LoggerModuleProbe)
    ge_set_output_tests(LoggerModuleLifetimeTests)
    ge_stage_runtime_dependencies(LoggerModuleLifetimeTests)
    add_test(NAME LoggerModuleLifetimeTests COMMAND LoggerModuleLifetimeTests)
    set_tests_properties(LoggerModuleLifetimeTests PROPERTIES TIMEOUT 60 LABELS "Logger;Unit;DLL")
endif()

add_executable(LoggerSourceLocationBenchmark SourceLocationBenchmark.cpp)
target_link_libraries(LoggerSourceLocationBenchmark PRIVATE Logger)
target_compile_features(LoggerSourceLocationBenchmark PRIVATE cxx_std_20)
ge_set_output_tests(LoggerSourceLocationBenchmark)
ge_stage_runtime_dependencies(LoggerSourceLocationBenchmark)
