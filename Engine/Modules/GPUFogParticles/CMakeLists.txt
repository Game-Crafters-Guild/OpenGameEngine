cmake_minimum_required(VERSION 3.20)

project(GPUFogParticles LANGUAGES CXX)

add_library(GPUFogParticles OBJECT
    Include/GPUFogParticles/GPUFogParticlesMaterial.h
    Source/GPUFogParticlesMaterial.cpp
)

target_compile_features(GPUFogParticles PUBLIC cxx_std_20)

target_include_directories(GPUFogParticles
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
)

target_link_libraries(GPUFogParticles
    PUBLIC
        GameEngineRendering
)

if(MSVC)
    target_compile_options(GPUFogParticles PRIVATE /W4 /bigobj)
else()
    target_compile_options(GPUFogParticles PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
