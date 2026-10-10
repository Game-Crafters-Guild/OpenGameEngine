cmake_minimum_required(VERSION 3.20)

project(Text LANGUAGES CXX)

add_library(Text OBJECT
    Source/FTUtils.cpp
    Source/FontAtlas.cpp
    Source/FontAtlas_Slug.cpp
    Source/FontAtlas_Measure.cpp
    Source/TextLayout.cpp
)

target_include_directories(Text PUBLIC Include)

# Keep FreeType/HarfBuzz available for the text stack (public so consumers see defines)
find_package(Freetype CONFIG QUIET)
if (TARGET Freetype::Freetype)
    target_link_libraries(Text PUBLIC Freetype::Freetype)
    target_compile_definitions(Text PUBLIC GE_HAVE_FREETYPE=1)
    message(STATUS "Text module: FreeType found and linked")
endif()

find_package(harfbuzz CONFIG QUIET)
if (TARGET harfbuzz::harfbuzz)
    target_link_libraries(Text PUBLIC harfbuzz::harfbuzz)
    target_compile_definitions(Text PUBLIC GE_HAVE_HARFBUZZ=1)
    message(STATUS "Text module: HarfBuzz found and linked")
endif()

# C++ standard
set_target_properties(Text PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

