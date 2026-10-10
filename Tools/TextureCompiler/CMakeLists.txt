# TextureCompiler — offline PNG/TGA/JPG -> UASTC KTX2 encoder (see TextureCompiler.cpp).
# General-purpose texture cooking CLI; the Unity scene converter shells out to it.
add_executable(TextureCompiler
    TextureCompiler.cpp
)

set_target_properties(TextureCompiler PROPERTIES FOLDER "Tools")
target_compile_features(TextureCompiler PRIVATE cxx_std_20)

# libktx: encoder (UASTC + Zstd) and container writer.
find_package(Ktx CONFIG REQUIRED)
target_link_libraries(TextureCompiler PRIVATE KTX::ktx)

# stb_image / stb_image_resize2 (header-only; implementations compiled in-TU).
find_path(GE_TEXC_STB_INCLUDE_DIRS "stb_image_resize2.h" REQUIRED)
target_include_directories(TextureCompiler PRIVATE ${GE_TEXC_STB_INCLUDE_DIRS})

# VkFormat constants for the KTX2 createInfo (header-only usage).
find_path(GE_TEXC_VULKAN_INCLUDE_DIRS "vulkan/vulkan_core.h" REQUIRED)
target_include_directories(TextureCompiler PRIVATE ${GE_TEXC_VULKAN_INCLUDE_DIRS})

# Output layout: bin/<Config>/Tools
ge_set_output_tools(TextureCompiler)

# Stage runtime DLLs (ktx.dll, zstd.dll) next to the tool.
ge_stage_runtime_dependencies(TextureCompiler)
