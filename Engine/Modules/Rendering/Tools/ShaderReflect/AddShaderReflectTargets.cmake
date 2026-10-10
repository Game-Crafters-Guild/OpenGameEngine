# Builds ShaderReflectCore + ShaderReflect without Engine, Vulkan, or GLFW.
# Used by the main Rendering module and Tools/ShaderReflectHost cross-builds.
function(ge_add_shader_reflect_targets)
    cmake_parse_arguments(_GE_SR "" "RENDERING_ROOT" "" ${ARGN})

    if(NOT _GE_SR_RENDERING_ROOT)
        message(FATAL_ERROR "ge_add_shader_reflect_targets requires RENDERING_ROOT")
    endif()
    set(_GE_RENDERING_ROOT "${_GE_SR_RENDERING_ROOT}")

    find_package(nlohmann_json CONFIG REQUIRED)

    set(_GE_SR_SPIRV_TARGET "")
    find_package(SPIRV-Reflect CONFIG QUIET)
    if(TARGET SPIRV-Reflect::SPIRV-Reflect)
        set(_GE_SR_SPIRV_TARGET SPIRV-Reflect::SPIRV-Reflect)
    else()
        find_package(unofficial-spirv-reflect CONFIG REQUIRED)
        set(_GE_SR_SPIRV_TARGET unofficial::spirv-reflect)
    endif()

    if(TARGET ShaderReflectCore)
        message(STATUS "ShaderReflectCore already defined")
    else()
        add_library(ShaderReflectCore STATIC
            ${_GE_RENDERING_ROOT}/Source/Materials/ShaderReflection.cpp
            ${_GE_RENDERING_ROOT}/Source/Materials/ShaderMetaJson.cpp
            ${_GE_RENDERING_ROOT}/Source/Materials/ShaderMetaValidation.cpp
            # The package container and its meta payload codec. Standard
            # library and ShaderMeta.h only, which is what lets the cook tool
            # and the engine share one reader and one writer.
            ${_GE_RENDERING_ROOT}/Source/ShaderCache/ShaderMetaBinary.cpp
            ${_GE_RENDERING_ROOT}/Source/ShaderCache/ShaderPackageContainer.cpp
        )
        target_include_directories(ShaderReflectCore
            PUBLIC
                ${_GE_RENDERING_ROOT}/Include
                # ShaderMeta.h includes Types/StringId.h (binding NameId); the
                # header-only Types module is pathed directly so the standalone
                # ShaderReflectHost cross-build works without engine targets.
                ${_GE_RENDERING_ROOT}/../Types/Include
        )
        target_link_libraries(ShaderReflectCore
            PUBLIC
                nlohmann_json::nlohmann_json
                ${_GE_SR_SPIRV_TARGET}
        )
        find_package(SPIRV-Headers CONFIG QUIET)
        if(TARGET SPIRV-Headers::SPIRV-Headers)
            target_link_libraries(ShaderReflectCore PUBLIC SPIRV-Headers::SPIRV-Headers)
        endif()
        target_compile_definitions(ShaderReflectCore
            PUBLIC
                RENDERING_ENABLE_SPIRV_REFLECTION=1
                SPIRV_REFLECT_USE_SYSTEM_SPIRV_H=1
        )
        set_target_properties(ShaderReflectCore PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tools"
        )
    endif()

    if(TARGET ShaderReflect)
        message(STATUS "ShaderReflect executable already defined")
    else()
        add_executable(ShaderReflect
            ${_GE_RENDERING_ROOT}/Tools/ShaderReflect/main.cpp
        )
        target_link_libraries(ShaderReflect PRIVATE ShaderReflectCore)
        target_compile_definitions(ShaderReflect PRIVATE RENDERING_ENABLE_SPIRV_REFLECTION=1)
        set_target_properties(ShaderReflect PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tools"
            VCPKG_APPLOCAL_DEPS OFF
        )
        # Tool exes land in the canonical bin/<config>/Tools/ layout (see
        # cmake/OutputLayout.cmake; the Steam Deck cross-build scripts consume
        # this path). Multi-config generators initialize the per-config
        # RUNTIME_OUTPUT_DIRECTORY_<CONFIG> properties from the global
        # CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG> variables at target creation,
        # and those OVERRIDE the generic property — a generic-only setting is
        # silently ignored there. This helper is standalone (cross-compile host
        # builds use it without engine cmake modules), so it mirrors
        # OutputLayout.cmake's per-config loop instead of including it.
        if(CMAKE_CONFIGURATION_TYPES)
            foreach(_ge_sr_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
                string(TOUPPER "${_ge_sr_cfg}" _ge_sr_cfg_upper)
                set_target_properties(ShaderReflect PROPERTIES
                    "RUNTIME_OUTPUT_DIRECTORY_${_ge_sr_cfg_upper}"
                    "${CMAKE_BINARY_DIR}/bin/${_ge_sr_cfg}/Tools")
            endforeach()
        else()
            set_target_properties(ShaderReflect PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Tools")
        endif()
    endif()
endfunction()
