cmake_minimum_required(VERSION 3.20)

project(HeightFog LANGUAGES CXX)

add_library(HeightFog OBJECT
    Include/Components/Rendering/PostProcessEffects/HeightFogEffect.h
    Include/Components/Rendering/PostProcessEffects/HeightFogEffectPresets.h
    Source/HeightFogModule.cpp
    Source/HeightFogEffectPresets.cpp
)

target_compile_features(HeightFog PUBLIC cxx_std_20)

target_include_directories(HeightFog
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Include
)

target_link_libraries(HeightFog
    PUBLIC
        Types
)

if(MSVC)
    target_compile_options(HeightFog PRIVATE /W4 /bigobj)
else()
    target_compile_options(HeightFog PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
