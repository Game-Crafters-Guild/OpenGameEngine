# GenerationalVector Examples

# Basic example
add_executable(GenerationalVectorBasicExample
    BasicExample.cpp
)

target_link_libraries(GenerationalVectorBasicExample
    PRIVATE
        GenerationalVector
)

# Set output directory
set_target_properties(GenerationalVectorBasicExample PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/Debug
)
