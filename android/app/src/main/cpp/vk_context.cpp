#ifdef _WIN32
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vk_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

namespace vk {

void Fatal(const char* what, VkResult r, const char* file, int line) {
    REXLOG_ERROR("vulkan: {} failed ({}) at {}:{}", what, int(r), file, line);
    rex::FlushLogging();
    std::abort();
}

Context& Get() {
    static Context c;
    return c;
}

bool Context::Init(SDL_Window* window) {
    window_ = window;
#ifdef _WIN32
    // the SDK's Windows package has SDL built without Vulkan: the surface comes from the window's HWND
    std::vector<const char*> ext = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#else
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        REXLOG_ERROR("vulkan: no loader: {}", SDL_GetError());
        return false;
    }
    Uint32 n_ext = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_ext);
    std::vector<const char*> ext(sdl_ext, sdl_ext + n_ext);
#endif

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Battlefield 2: Modern Combat";
    app.apiVersion = force_vulkan_11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(ext.size());
    ici.ppEnabledExtensionNames = ext.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
    if (!CreateSurface()) return false;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance, &n, gpus.data());
    for (VkPhysicalDevice g : gpus) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &nq, qs.data());
        for (uint32_t i = 0; i < nq; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                gpu = g;
                queue_family = i;
                break;
            }
        }
        if (gpu) break;
    }
    if (!gpu) {
        REXLOG_ERROR("vulkan: no GPU with a graphics queue that presents");
        return false;
    }
    vkGetPhysicalDeviceProperties(gpu, &props);
    vkGetPhysicalDeviceFeatures(gpu, &features);
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    REXLOG_INFO("vulkan: {}, API {}.{}.{}, driver {:08X}", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // only what the game needs, when the GPU has it
    VkPhysicalDeviceFeatures want{};
    want.samplerAnisotropy = features.samplerAnisotropy;
    want.fillModeNonSolid = features.fillModeNonSolid;
    want.independentBlend = features.independentBlend;
    want.depthClamp = features.depthClamp;
    want.textureCompressionBC = features.textureCompressionBC;
    if (props.apiVersion < VK_API_VERSION_1_1) {
        REXLOG_ERROR("vulkan: {} has Vulkan {}.{}, the renderer needs 1.1", props.deviceName,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
        return false;
    }
    const uint32_t api = force_vulkan_11 ? VK_API_VERSION_1_1 : props.apiVersion;
    uint32_t n_dev_ext = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n_dev_ext, nullptr);
    std::vector<VkExtensionProperties> have_ext(n_dev_ext);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n_dev_ext, have_ext.data());
    auto has_ext = [&](const char* name) {
        for (const auto& e : have_ext)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    std::vector<const char*> dev_ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const bool core13 = api >= VK_API_VERSION_1_3;
    if (!core13) {
        // VK_KHR_dynamic_rendering and what it depends on, for drivers older than 1.3
        for (const char* e : {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
                              VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME,
                              VK_KHR_MAINTENANCE_2_EXTENSION_NAME}) {
            if (!has_ext(e)) {
                REXLOG_ERROR("vulkan: {} has Vulkan {}.{} without {}: dynamic rendering is required", props.deviceName,
                             VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), e);
                return false;
            }
            dev_ext.push_back(e);
        }
    }
    const bool core12 = api >= VK_API_VERSION_1_2;
    format_list = core12 || has_ext(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    if (!core12 && format_list) dev_ext.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    if (!core12 && has_ext(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) {
        dev_ext.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        mirror_clamp = VK_TRUE;
    }
    REXLOG_INFO("vulkan: rendering through {}", core13 ? "Vulkan 1.3" : "VK_KHR_dynamic_rendering");
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    if (core12) {
        VkPhysicalDeviceVulkan12Features f12_have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12_have;
        vkGetPhysicalDeviceFeatures2(gpu, &f2);
        mirror_clamp = f12_have.samplerMirrorClampToEdge;
        f12.samplerMirrorClampToEdge = mirror_clamp;
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &f12;
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    fdr.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = core13 ? static_cast<void*>(&f13) : core12 ? static_cast<void*>(&f12) : nullptr;
    if (!core13) {
        fdr.pNext = const_cast<void*>(dci.pNext);
        dci.pNext = &fdr;
    }
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    dci.pEnabledFeatures = &want;
    VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    features = want;
    if (!LoadDynamicRendering(device, core13)) {
        REXLOG_ERROR("vulkan: the driver lacks the dynamic rendering commands");
        return false;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    for (Frame& f : frames) {
        // a pool per frame: the present thread finishes one frame's commands while the render thread records the next,
        // and a command pool may only be used by one thread at a time
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &f.cmd));
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &f.pre));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fi, nullptr, &f.done));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &f.acquired));
        VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &f.rendered));
    }
    return CreateSwapchain();
}

bool Context::CreateSurface() {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    return vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface) == VK_SUCCESS;
#else
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        REXLOG_ERROR("vulkan: no surface: {}", SDL_GetError());
        surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
#endif
}

void Context::Pause() {
    std::lock_guard lock(pause_mutex_);
    paused_ = true;
}

void Context::Resume() {
    {
        std::lock_guard lock(pause_mutex_);
        paused_ = false;
        surface_stale_ = true;  // Android gives the activity a new window when it comes back
    }
    pause_cv_.notify_all();
}

// Present thread. Holds the frame while the app is in the background: the game thread then waits for it in
// BeginFrame, so the game stops instead of running on behind the home screen.
bool Context::ReadyToPresent() {
    {
        std::unique_lock lock(pause_mutex_);
        if (paused_) {
            REXLOG_INFO("vulkan: paused in the background");
            pause_cv_.wait(lock, [&] { return !paused_; });
            REXLOG_INFO("vulkan: resumed");
        }
    }
    if (!surface_stale_) return true;
    // a new surface and swapchain for the window SDL has now. Until the activity has one, skip the present.
    vkDeviceWaitIdle(device);
    DestroySwapchain();
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (!CreateSurface() || !CreateSwapchain()) {
        SDL_Delay(20);
        return false;
    }
    surface_stale_ = false;
    return true;
}

bool Context::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
    VkSurfaceFormatKHR fmt = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            fmt = f;
            break;
        }
    swap_extent = caps.currentExtent;
    if (swap_extent.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        swap_extent = {uint32_t(w), uint32_t(h)};
    }
    if (!swap_extent.width || !swap_extent.height) return false;
    swap_format = fmt.format;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = swap_extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Android rotates the image itself when the transform is not the identity; the activity is landscape only
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR old = swapchain;
    sci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
    if (old) {
        for (VkImageView v : swap_views) vkDestroyImageView(device, v, nullptr);
        vkDestroySwapchainKHR(device, old, nullptr);
    }
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    swap_images.resize(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, swap_images.data());
    swap_views.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &swap_views[i]));
    }
    REXLOG_INFO("vulkan: swapchain {}x{}, {} images, format {}", swap_extent.width, swap_extent.height, n, int(swap_format));
    return true;
}

void Context::DestroySwapchain() {
    for (VkImageView v : swap_views) vkDestroyImageView(device, v, nullptr);
    swap_views.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

VkCommandBuffer Context::Begin() {
    Frame& f = Current();
    VK_CHECK(vkWaitForFences(device, 1, &f.done, VK_TRUE, UINT64_MAX));
    for (auto& r : f.release) r();
    f.release.clear();
    VK_CHECK(vkResetCommandBuffer(f.cmd, 0));
    VK_CHECK(vkResetCommandBuffer(f.pre, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    VK_CHECK(vkBeginCommandBuffer(f.pre, &bi));
    frame_serial++;
    return f.cmd;
}

bool Context::Acquire(uint32_t slot, uint32_t& image) {
    Frame& f = frames[slot];
    acquired_ = false;
    if (!ReadyToPresent() || !swapchain) return false;
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(device);
        CreateSwapchain();
        return false;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {  // the window went away before the pause reached us
        surface_stale_ = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    acquired_ = true;
    return true;
}

void Context::Submit(uint32_t slot, bool present, uint32_t image) {
    Frame& f = frames[slot];
    VK_CHECK(vkEndCommandBuffer(f.pre));
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    VK_CHECK(vkResetFences(device, 1, &f.done));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkCommandBuffer cmds[2] = {f.pre, f.cmd};
    si.commandBufferCount = 2;
    si.pCommandBuffers = cmds;
    bool presenting = present && acquired_;
    if (presenting) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.rendered;
    }
    VK_CHECK(vkQueueSubmit(queue, 1, &si, f.done));
    if (presenting) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &f.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &image;
        VkResult r = vkQueuePresentKHR(queue, &pi);
        // SUBOPTIMAL is every present on Android when the panel is rotated (identity transform, the compositor
        // rotates): only OUT_OF_DATE needs a new swapchain
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(device);
            CreateSwapchain();
        } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
            surface_stale_ = true;
        } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            VK_CHECK(r);
        }
    }
}

VkCommandBuffer Context::BeginOneTime() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void Context::EndOneTime(VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &fi, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

void Context::WaitIdle() {
    if (device) vkDeviceWaitIdle(device);
}

uint32_t Context::MemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) const {
    for (VkMemoryPropertyFlags flags : {want, fallback}) {
        if (!flags) continue;
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    Fatal("MemoryType", VK_ERROR_OUT_OF_DEVICE_MEMORY, __FILE__, __LINE__);
}

void Context::Shutdown() {
    WaitIdle();
    for (Frame& f : frames) {
        for (auto& r : f.release) r();
        f.release.clear();
        vkDestroyCommandPool(device, f.pool, nullptr);
        vkDestroyFence(device, f.done, nullptr);
        vkDestroySemaphore(device, f.acquired, nullptr);
        vkDestroySemaphore(device, f.rendered, nullptr);
    }
    DestroySwapchain();
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

}  // namespace vk
