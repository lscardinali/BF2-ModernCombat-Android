// Android's libvulkan.so exports Vulkan 1.1's commands only: the 1.3 ones the backend uses (dynamic rendering,
// extended dynamic state) are defined here, under their own names, forwarding to what vkGetDeviceProcAddr returns.
#include <vulkan/vulkan.h>

namespace {

#define VK13_FUNCS(X)                  \
    X(vkCmdBeginRendering)             \
    X(vkCmdEndRendering)               \
    X(vkCmdSetCullMode)                \
    X(vkCmdSetFrontFace)               \
    X(vkCmdSetPrimitiveTopology)       \
    X(vkCmdSetDepthTestEnable)         \
    X(vkCmdSetDepthWriteEnable)        \
    X(vkCmdSetDepthCompareOp)          \
    X(vkCmdSetStencilTestEnable)       \
    X(vkCmdSetStencilOp)               \
    X(vkCmdBindVertexBuffers2)

#define DECLARE(name) PFN_##name p_##name = nullptr;
VK13_FUNCS(DECLARE)
#undef DECLARE

}  // namespace

namespace vk {

// false when the driver lacks one of them
bool LoadVulkan13(VkDevice device) {
    bool ok = true;
#define LOAD(name)                                                                       \
    p_##name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));          \
    ok = ok && p_##name;
    VK13_FUNCS(LOAD)
#undef LOAD
    return ok;
}

}  // namespace vk

extern "C" {

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) { p_vkCmdBeginRendering(cb, ri); }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cb) { p_vkCmdEndRendering(cb); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) { p_vkCmdSetCullMode(cb, m); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer cb, VkFrontFace f) { p_vkCmdSetFrontFace(cb, f); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) {
    p_vkCmdSetPrimitiveTopology(cb, t);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 e) { p_vkCmdSetDepthTestEnable(cb, e); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 e) { p_vkCmdSetDepthWriteEnable(cb, e); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp o) { p_vkCmdSetDepthCompareOp(cb, o); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 e) { p_vkCmdSetStencilTestEnable(cb, e); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass,
                                             VkStencilOp zfail, VkCompareOp cmp) {
    p_vkCmdSetStencilOp(cb, f, fail, pass, zfail, cmp);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b,
                                                   const VkDeviceSize* o, const VkDeviceSize* sz, const VkDeviceSize* st) {
    p_vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st);
}

}  // extern "C"
