// Android's libvulkan.so exports Vulkan 1.1's commands only: dynamic rendering is defined here, under its core names,
// forwarding to what vkGetDeviceProcAddr returns: the 1.3 commands, or VK_KHR_dynamic_rendering's on an older driver.
#include <vulkan/vulkan.h>

namespace {

PFN_vkCmdBeginRendering p_begin = nullptr;
PFN_vkCmdEndRendering p_end = nullptr;

}  // namespace

namespace vk {

// false when the driver has neither
bool LoadDynamicRendering(VkDevice device, bool core) {
    p_begin = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device, core ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
    p_end = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device, core ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
    return p_begin && p_end;
}

}  // namespace vk

extern "C" {

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) { p_begin(cb, ri); }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cb) { p_end(cb); }

}  // extern "C"
