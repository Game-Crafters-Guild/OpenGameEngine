cmake_minimum_required(VERSION 3.20)
project(Audio LANGUAGES CXX C)

# Public headers
file(GLOB_RECURSE AUDIO_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/Include/Audio/*.h
)

set(AUDIO_SOURCES
    Source/AudioSystem.cpp
    Source/MiniaudioBackend.cpp
    Source/miniaudio_impl.c
)

add_library(Audio OBJECT ${AUDIO_PUBLIC_HEADERS} ${AUDIO_SOURCES})

set_target_properties(Audio PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Audio"
)

target_compile_features(Audio PUBLIC cxx_std_20)

target_include_directories(Audio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        Source
        ${CMAKE_SOURCE_DIR}/Engine/Include
)

target_link_libraries(Audio
    PUBLIC
        Logger
        Types
        Mathematics
        AssetCore
        AssetRuntime
)

# macOS: Link CoreAudio frameworks for miniaudio
# According to miniaudio docs, we should link these frameworks explicitly
# to avoid runtime linking issues that can cause notarization problems
if(APPLE)
    target_link_libraries(Audio
        PRIVATE
            "-framework CoreFoundation"
            "-framework CoreAudio"
            "-framework AudioToolbox"
    )
endif()


