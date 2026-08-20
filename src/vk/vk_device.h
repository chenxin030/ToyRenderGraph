#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <vector>

namespace vk {

// 设备与交换链的薄封装：实例、物理设备、逻辑设备、交换链。
// 本项目只使用单一队列（graphics + present），不做多队列。
struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
};

bool CreateInstance(Device& d);
bool CreateSurface(Device& d, GLFWwindow* window);
bool PickPhysicalDevice(Device& d);
bool CreateDevice(Device& d);
bool CreateSwapchain(Device& d, GLFWwindow* window);

// 交换链尺寸决策（纯函数，便于单元测试）：
// 窗口最小化/隐藏时 framebuffer 为 0x0，此时必须拒绝重建，
// 否则渲染路径会以 0x0 尺寸运行（aspect = 0/0 = NaN，触发 glm 断言）。
struct SwapExtentChoice {
    bool ok = false;
    VkExtent2D extent{};
};
SwapExtentChoice ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, int fbWidth, int fbHeight);

void DestroySwapchain(Device& d);
void DestroyDevice(Device& d);

bool RecreateSwapchain(Device& d, GLFWwindow* window);

}  // namespace vk
