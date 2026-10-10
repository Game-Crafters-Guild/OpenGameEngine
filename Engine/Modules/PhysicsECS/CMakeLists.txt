cmake_minimum_required(VERSION 3.20)

project(PhysicsECS LANGUAGES CXX)

add_library(PhysicsECS OBJECT
    Source/PhysicsComponentInstantiations.cpp
    Source/PhysicsECSModule.cpp
    Source/PhysicsEntityEventCallbacks.cpp
    Source/PhysicsWorldService.cpp
    Source/HeightFieldDataProvider.cpp
    Source/HeightFieldCollisionReadiness.cpp
    Source/PhysicsWorldSettingsHelpers.cpp
    Source/Systems/PhysicsWorldBootstrapSystem.cpp
    Source/Systems/PhysicsEventsSystem.cpp
    Source/Systems/PhysicsInitSystem.cpp
    Source/Systems/CharacterControllerSystem.cpp
    Source/Systems/CharacterControllerWritebackSystem.cpp
    Source/Systems/PhysicsStepSystem.cpp
    Source/Systems/PhysicsPoseWriteback.cpp
    Source/Systems/PhysicsWritebackSystem.cpp
    Source/Systems/PhysicsWorldHooks.cpp
    Source/Systems/RegisterPhysicsSystems.cpp
)

target_compile_features(PhysicsECS PUBLIC cxx_std_20)

target_include_directories(PhysicsECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(PhysicsECS
    PUBLIC
        Physics
        ECS
        ECSComponents
        Types
        Mathematics
        Logger
)

