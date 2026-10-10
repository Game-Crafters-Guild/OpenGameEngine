# Fails if any Vulkan types or headers are referenced from public Rendering headers
cmake_minimum_required(VERSION 3.20)

# Root of the Rendering module (passed in via -DRENDERING_ROOT)
if(NOT DEFINED RENDERING_ROOT)
    message(FATAL_ERROR "RENDERING_ROOT must be provided")
endif()

file(GLOB_RECURSE RENDERING_PUBLIC_HEADERS
    "${RENDERING_ROOT}/Include/Rendering/*.h"
    "${RENDERING_ROOT}/Include/Rendering/*.hpp"
)

# The Vk and VK_ patterns start at an identifier boundary (CMake regex has no word-boundary anchor):
# VkDevice and VK_FORMAT_R8_UNORM match, GE_VK_FILL_NEW_TARGETS_NAN and MyVkLikeName do not.
set(FORBIDDEN_PATTERNS
    "vulkan.h"
    "(^|[^A-Za-z0-9_])Vk[A-Z]"     # Matches VkInstance, VkDevice, etc.
    "(^|[^A-Za-z0-9_])VK_[A-Z_]"   # Matches VK_KHR_*, VK_PIPELINE_STAGE_*, etc.
)

set(FOUND_FORBIDDEN FALSE)

foreach(HDR IN LISTS RENDERING_PUBLIC_HEADERS)
    file(READ "${HDR}" HDR_CONTENT)
    foreach(PATTERN IN LISTS FORBIDDEN_PATTERNS)
        if(HDR_CONTENT MATCHES "${PATTERN}")
            message(SEND_ERROR "Forbidden Vulkan symbol '${PATTERN}' found in public header: ${HDR}")
            set(FOUND_FORBIDDEN TRUE)
        endif()
    endforeach()
endforeach()

if(FOUND_FORBIDDEN)
    message(FATAL_ERROR "Public Rendering headers must not reference Vulkan types or headers.")
endif()

