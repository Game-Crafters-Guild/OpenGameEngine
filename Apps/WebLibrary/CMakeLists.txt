# The engine as a library a web page imports (design: workbench designs/web/web-api.md 3.1):
# one static binary over the Engine graph, instantiated by the page through
# opengine-core-binding.js and driven through the C ABI that ts/src/abi.ts specifies. The
# configure tree decides the build: a threaded one writes opengine-core.mt.{js,wasm}, a
# single-threaded one (GE_WASM_SINGLE_THREAD) opengine-core.st.{js,wasm}. The engine's own
# content is not linked in: ge_create fetches opengine-core.gepak from beside the glue.
if(NOT EMSCRIPTEN)
    return()
endif()

find_package(nlohmann_json CONFIG REQUIRED)

# The exports, shared with the node smoke (Tests/Wasm), which links them with a headless boot
# in place of a page.
add_library(WebLibraryAbi OBJECT
    Source/AbiAnimation.cpp
    Source/AbiAssets.cpp
    Source/AbiAssets.h
    Source/AbiComponents.cpp
    Source/AbiEntities.cpp
    Source/AbiEntities.h
    Source/AbiErrors.cpp
    Source/AbiErrors.h
    Source/AbiLifecycle.cpp
    Source/AbiLifecycle.h
    Source/AbiQuery.cpp
    Source/AbiQuery.h
    Source/AbiReflection.cpp
    Source/PageReflection.cpp
    Source/PageReflection.h
    Source/UrlAssetLoads.cpp
    Source/UrlAssetLoads.h
    Source/WebLibraryApplication.cpp
    Source/WebLibraryApplication.h
    # The .gepak reader the web Player unpacks its dist with; the format is Tools/Web/gepak.py.
    ${CMAKE_SOURCE_DIR}/Apps/Player/Source/WebPackReader.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Player/Source/WebPackReader.h)
target_include_directories(WebLibraryAbi PRIVATE Source ${CMAKE_SOURCE_DIR}/Apps/Player/Source)
target_compile_features(WebLibraryAbi PRIVATE cxx_std_20)
target_link_libraries(WebLibraryAbi PUBLIC Engine Types nlohmann_json::nlohmann_json)

set(GE_WEB_LIBRARY_EXPORTS
    _malloc _free
    _ge_create _ge_tick _ge_update_assets _ge_resize _ge_shutdown
    _ge_load_asset _ge_asset_status _ge_instantiate_model _ge_model_emissive_strength
    _ge_entity_create _ge_entity_bounds _ge_entity_destroy _ge_entity_parent _ge_entity_children _ge_entity_set_mesh
    _ge_component_add _ge_component_remove _ge_component_has
    _ge_field_get _ge_field_set
    _ge_query_begin _ge_query_next_chunk _ge_query_end
    _ge_animation_clips _ge_animation_play _ge_animation_pause
    _ge_reflection_json _ge_last_error)
list(JOIN GE_WEB_LIBRARY_EXPORTS "," _ge_web_library_exports)
# What every module over these exports links with: the export list, the two runtime methods the
# binding reads, and classic ASYNCIFY with the Player's asyncify stack, because ge_create pumps
# the browser's promises (the engine pack's fetch, the WebGPU adapter and device).
target_link_options(WebLibraryAbi INTERFACE
    "SHELL:-s EXPORTED_FUNCTIONS=${_ge_web_library_exports}"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,HEAPU8"
    "SHELL:-s ASYNCIFY"
    "SHELL:-s ASYNCIFY_STACK_SIZE=1048576"
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createOpenEngine")

if(GE_WASM_SINGLE_THREAD)
    set(_ge_web_library_build st)
    set(_ge_web_library_env "SHELL:-s ENVIRONMENT=web")
else()
    set(_ge_web_library_build mt)
    set(_ge_web_library_env "SHELL:-s ENVIRONMENT=web,worker")
endif()

add_executable(WebLibrary)
target_link_libraries(WebLibrary PRIVATE WebLibraryAbi)
set_target_properties(WebLibrary PROPERTIES
    OUTPUT_NAME opengine-core.${_ge_web_library_build}
    SUFFIX ".js")
target_link_options(WebLibrary PRIVATE
    ${_ge_web_library_env}
    "SHELL:--no-entry"
    "SHELL:-s INITIAL_MEMORY=256MB")
# The binding the facade imports sits beside the glue it instantiates.
add_custom_command(TARGET WebLibrary POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/opengine-core-binding.js
        $<TARGET_FILE_DIR:WebLibrary>/opengine-core-binding.js
    COMMENT "Staging opengine-core-binding.js beside the engine module")
if(NOT GE_WASM_SINGLE_THREAD)
    # Classic asyncify's rewind window must not run the pthread mailbox pump (the WebEditor's
    # patch; Tools/Web/patch_check_mailbox.py says why). ge_create is that window here.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set_property(TARGET WebLibrary APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_SOURCE_DIR}/Tools/Web/patch_check_mailbox.py)
    add_custom_command(TARGET WebLibrary POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/patch_check_mailbox.py
            $<TARGET_FILE:WebLibrary>
        COMMENT "Patching checkMailbox against the asyncify rewind race")
endif()
