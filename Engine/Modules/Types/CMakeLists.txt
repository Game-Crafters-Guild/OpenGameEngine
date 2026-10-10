# Types module: header-only interface for cross-cutting basic types

cmake_minimum_required(VERSION 3.20)

project(Types LANGUAGES CXX)

# Header-only target to propagate include path transitively
add_library(Types INTERFACE)

# Public include: exposes "Types/Types.h" to dependents
# Keep the include structure stable for existing includes like: #include "Types/Types.h"
target_include_directories(Types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)

# unordered_dense — header-only flat hash map used by FastHashMap / FastHashSet
# in Types.h. Propagated transitively to every consumer of the Types module
# so they don't all have to find_package(unordered_dense) separately.
find_package(unordered_dense CONFIG REQUIRED)
target_link_libraries(Types INTERFACE unordered_dense::unordered_dense)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()

