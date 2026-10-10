cmake_minimum_required(VERSION 3.25)

# Minimal manifest: nlohmann-json + spirv-reflect only (no Engine / ffmpeg / Vulkan).
set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)

project(ShaderReflectHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_GE_RENDERING_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Modules/Rendering")
include(${_GE_RENDERING_ROOT}/Tools/ShaderReflect/AddShaderReflectTargets.cmake)
ge_add_shader_reflect_targets(RENDERING_ROOT "${_GE_RENDERING_ROOT}")
