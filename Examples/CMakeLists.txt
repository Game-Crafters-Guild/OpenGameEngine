# Standalone demos: built with BUILD_EXAMPLES=ON, staged under bin/<Config>/Demos.

# Physics + Rendering demo: falling spheres onto a plane (R to reset, Esc to quit).
add_executable(PhysicsFallingSpheresDemo
    PhysicsFallingSpheresDemo.cpp
)

target_link_libraries(PhysicsFallingSpheresDemo
    PRIVATE
    Engine
    ${CMAKE_THREAD_LIBS_INIT}
)

if(TARGET GameEngine.Native)
    target_link_libraries(PhysicsFallingSpheresDemo PRIVATE GameEngine.Native)
endif()

target_include_directories(PhysicsFallingSpheresDemo
    PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include
)

ge_stage_runtime_dependencies(PhysicsFallingSpheresDemo)
ge_set_output_demos(PhysicsFallingSpheresDemo)
set_target_properties(PhysicsFallingSpheresDemo PROPERTIES FOLDER "Demos")
