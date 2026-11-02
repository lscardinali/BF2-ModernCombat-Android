#pragma once
#include <filesystem>

struct SDL_Window;

namespace bf2n {

// before the game starts: where the shader cache goes. The Vulkan context (vk::Get) must already be up.
void VulkanRendererSetup(SDL_Window* window, const std::filesystem::path& user_dir);

}  // namespace bf2n
