cmake_minimum_required(VERSION 3.20)

project(PluginAPI LANGUAGES CXX)

add_library(PluginAPI OBJECT
    Source/EnginePluginRegistry.cpp
)

target_compile_features(PluginAPI PUBLIC cxx_std_20)

target_include_directories(PluginAPI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

# Registry lifecycle transitions (replace-forward, unregister) log loudly, and
# registrations are stamped with the loader's active module + generation
# (ECS/ModuleRegistration.h — C12 reload protocol). Include-path only: both
# modules' objects reach the final binary via the Engine module splice.
# ECS is PUBLIC because EnginePlugin.h includes the stamp header.
target_link_libraries(PluginAPI PRIVATE Logger)
target_link_libraries(PluginAPI PUBLIC ECS)
