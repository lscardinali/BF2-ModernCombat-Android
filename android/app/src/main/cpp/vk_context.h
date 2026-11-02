#pragma once
// The Vulkan device the renderer draws with: instance, the window's surface, one graphics+present queue, the swapchain
// and the frames in flight. It renders with dynamic rendering: core in Vulkan 1.3, VK_KHR_dynamic_rendering from 1.1 up,
// which is what older Adreno drivers report.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

struct SDL_Window;

namespace vk {

#define VK_CHECK(x)                                                         \
    do {                                                                    \
        VkResult r_ = (x);                                                  \
        if (r_ != VK_SUCCESS) vk::Fatal(#x, r_, __FILE__, __LINE__);        \
    } while (0)
[[noreturn]] void Fatal(const char* what, VkResult r, const char* file, int line);

constexpr uint32_t kFramesInFlight = 2;

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBuffer pre = VK_NULL_HANDLE;    // runs before cmd: uploads and first layouts, so they need no pass to end
    VkFence done = VK_NULL_HANDLE;           // signalled when this frame's commands finished
    VkSemaphore acquired = VK_NULL_HANDLE;   // swapchain image ready
    VkSemaphore rendered = VK_NULL_HANDLE;   // ready to present
    std::vector<std::function<void()>> release;  // destroyed once `done` is known signalled
};

class Context {
public:
    bool Init(SDL_Window* window);
    void Shutdown();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBool32 mirror_clamp = VK_FALSE;  // samplerMirrorClampToEdge
    bool format_list = false;           // VkImageFormatListCreateInfo can be used
    bool force_vulkan_11 = false;       // tests the older drivers' path on a newer one; set before Init

    // swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    bool CreateSwapchain();
    void DestroySwapchain();

    // frames in flight: Begin waits for the frame's last use and runs its releases, then opens its command buffer
    Frame frames[kFramesInFlight];
    uint32_t frame_index = 0;
    uint64_t frame_serial = 0;  // frames begun
    Frame& Current() { return frames[frame_index]; }
    VkCommandBuffer Begin();
    // The presenting thread's side, for a frame the game thread has finished recording:
    // acquires a swapchain image (false and nothing to present when the window is gone or resizing)
    bool Acquire(uint32_t slot, uint32_t& image);
    // submits the frame; presents image when present is true
    void Submit(uint32_t slot, bool present, uint32_t image);
    // the game thread moves on to the next frame
    void Advance() { frame_index = (frame_index + 1) % kFramesInFlight; }
    // destroyed after the GPU is past the current frame
    void Defer(std::function<void()> f) { Current().release.push_back(std::move(f)); }
    // one-off commands, waited for (uploads at init, read-backs)
    VkCommandBuffer BeginOneTime();
    void EndOneTime(VkCommandBuffer cmd);
    void WaitIdle();

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback = 0) const;

private:
    SDL_Window* window_ = nullptr;
    bool acquired_ = false;
};

Context& Get();

// vk_loader13.cpp: the dynamic rendering commands Android's loader does not export
bool LoadDynamicRendering(VkDevice device, bool core);

}  // namespace vk
