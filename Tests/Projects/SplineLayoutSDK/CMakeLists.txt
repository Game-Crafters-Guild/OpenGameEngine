cmake_minimum_required(VERSION 3.25)
project(SplineLayoutSDKConsumer LANGUAGES CXX)

# Configure with -DGameEngine_DIR=<staged SDK>/cmake and a matching build config.
# This consumer deliberately has no engine-source or editor include paths.
find_package(GameEngine CONFIG REQUIRED)
add_executable(SplineLayoutSDKConsumer main.cpp)
target_compile_features(SplineLayoutSDKConsumer PRIVATE cxx_std_20)
target_link_libraries(SplineLayoutSDKConsumer PRIVATE GameEngine::Engine)
enable_testing()
add_test(NAME SplineLayoutSDKConsumer COMMAND SplineLayoutSDKConsumer)
