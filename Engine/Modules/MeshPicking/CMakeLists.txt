cmake_minimum_required(VERSION 3.20)

project(MeshPicking LANGUAGES CXX)

add_library(MeshPicking OBJECT
    Source/MeshBvh.cpp
)

target_compile_features(MeshPicking PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" MESHPICKING_SUPPORTS_AVX2)
    check_cxx_compiler_flag("-mfma"  MESHPICKING_SUPPORTS_FMA)
    if(MESHPICKING_SUPPORTS_AVX2 AND MESHPICKING_SUPPORTS_FMA)
        target_compile_options(MeshPicking PRIVATE -mavx2 -mfma)
    endif()
endif()

target_include_directories(MeshPicking
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        # Core/CpuProfiler.h. NOT header-only: ScopedCpuProfile embeds a
        # ScopedNvtxRange whose ctor/dtor are defined in Engine, so anything
        # that links MeshPicking's objects must also link Engine (see
        # MeshPickingTests below).
        ${CMAKE_SOURCE_DIR}/Engine/Include
)

# Core/CpuProfiler.h gates ScopedCpuProfile's layout on GE_ENABLE_CPU_PROFILING
# and defaults it to 1 when undefined. MeshPicking cannot link Engine, so
# replay Engine's INTERFACE definitions the way TerrainECS does — otherwise a
# -DENABLE_CPU_PROFILING=OFF build compiles the stub inside Engine and the full
# layout here, which is an ODR mismatch rather than a missing optimisation.
target_compile_definitions(MeshPicking PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
)

target_link_libraries(MeshPicking
    PUBLIC
        Types
        Mathematics
        Logger
        # Usage requirements only (include dirs): both OBJECT libraries link
        # into Engine, where SceneBvh's BinnedSahSplit objects already live.
        SceneBvh
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(MeshPickingTests
        Tests/MeshBvhTests.cpp
    )
    # Engine, not just MeshPicking: MeshBvh.cpp's GE_CPU_PROFILE_SCOPE pulls in
    # ScopedNvtxRange, which is defined in Engine.dll. MeshPicking is an
    # editor-only OBJECT lib and is deliberately not spliced into Engine.dll,
    # so its objects arrive here unresolved without this. EditorTests links the
    # same pair for the same reason (Tests/Editor/CMakeLists.txt).
    target_link_libraries(MeshPickingTests PRIVATE
        Engine
        MeshPicking
        GTest::gtest
        GTest::gtest_main
    )
    ge_set_output_tests(MeshPickingTests)
    ge_stage_runtime_dependencies(MeshPickingTests)
    add_test(NAME MeshPickingTests COMMAND MeshPickingTests)
endif()
