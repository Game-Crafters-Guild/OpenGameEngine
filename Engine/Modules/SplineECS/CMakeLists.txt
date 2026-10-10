cmake_minimum_required(VERSION 3.20)

project(SplineECS LANGUAGES CXX)

add_library(SplineECS OBJECT
    Source/SplineService.cpp
    Source/Systems/SplineExtractionSystem.cpp
    Source/Systems/RegisterSplineSystems.cpp
)

target_compile_features(SplineECS PUBLIC cxx_std_20)

target_include_directories(SplineECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(SplineECS
    PUBLIC
        Spline
        ECS
        ECSComponents
        GenerationalVector
        Types
        Mathematics
        Logger
)
