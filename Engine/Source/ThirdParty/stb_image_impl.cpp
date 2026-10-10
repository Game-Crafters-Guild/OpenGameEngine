#include "Logger/Logger.h"

// Single translation unit implementation for stb_image
// Ensure this is added to the Engine target when GE_HAVE_STB is defined
#if defined(GE_HAVE_STB)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#endif

