cmake_minimum_required(VERSION 3.20)

# ECSComponents: shared component headers used by ECS modules.
# This target intentionally breaks the dependency cycle where:
#   Engine depends on PhysicsECS (module),
# but PhysicsECS needs to include Components/* headers.
#
# By providing the include root as an INTERFACE library, modules can depend on
# components without depending on the Engine target itself.

project(ECSComponents LANGUAGES CXX)

add_library(ECSComponents INTERFACE)

# Expose Engine public headers so includes like "Components/Transform.h" resolve.
target_include_directories(ECSComponents INTERFACE
    ${CMAKE_SOURCE_DIR}/Engine/Include
)

# Components headers depend on Types/Types.h. ECS is needed by
# Components/TransformDirtyFeed.h (the dirty-feed emission helper references
# ECS::World) — previously an undeclared dependency that only compiled by
# luck of consumer include paths (P2 seam review, A3 wiring nit).
target_link_libraries(ECSComponents INTERFACE Types ECS)

