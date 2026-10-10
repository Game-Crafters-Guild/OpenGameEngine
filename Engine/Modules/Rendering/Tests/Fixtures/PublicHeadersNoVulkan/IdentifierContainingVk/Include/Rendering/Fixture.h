#pragma once

// An identifier that merely contains "VK_" or "Vk" is not a Vulkan symbol:
// GE_VK_FILL_NEW_TARGETS_NAN, MyVkLikeName.
#define GE_VK_FILL_NEW_TARGETS_NAN 0
struct MyVkLikeName
{
};
