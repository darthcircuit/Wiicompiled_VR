// Included only by the pinned Dawn source build.
#pragma once
#include "dawn/native/DawnNative.h"
#include "aurora_dawn_vulkan_abi.h"
namespace dawn::native::vulkan {
PFN_vkCreateInstance AuroraCreateInstance(PFN_vkGetInstanceProcAddr proc);
PFN_vkCreateDevice AuroraCreateDevice(PFN_vkGetInstanceProcAddr proc, VkInstance instance);
PFN_vkEnumeratePhysicalDevices AuroraEnumeratePhysicalDevices(PFN_vkGetInstanceProcAddr proc, VkInstance instance);
}
