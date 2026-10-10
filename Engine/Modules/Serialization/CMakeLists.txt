cmake_minimum_required(VERSION 3.20)
project(Serialization LANGUAGES CXX)

add_library(Serialization INTERFACE)
target_include_directories(Serialization INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)
target_compile_features(Serialization INTERFACE cxx_std_20)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(Serialization INTERFACE Mathematics nlohmann_json::nlohmann_json)
