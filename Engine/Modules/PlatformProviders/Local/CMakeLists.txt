cmake_minimum_required(VERSION 3.20)
project(PlatformProviderLocal LANGUAGES CXX)

# Local fallback provider for PlatformServices: offline account, file/slot
# saves, mock achievements/entitlements. Selected via GE_PLATFORM_BACKEND=Local.
# Provider modules are the only place vendor SDK headers/libs may appear;
# Local has none by design.
add_library(PlatformProviderLocal OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/LocalProvider.cpp
)

target_link_libraries(PlatformProviderLocal PUBLIC PlatformServices)
target_link_libraries(PlatformProviderLocal PRIVATE FileSystem)

# Core/StandardPaths.h for the default save root; resolved inside Engine.dll.
target_include_directories(PlatformProviderLocal PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)

set_target_properties(PlatformProviderLocal PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "PlatformServices"
)
