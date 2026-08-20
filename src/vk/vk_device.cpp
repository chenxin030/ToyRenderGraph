#include "vk/vk_device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// 检查实例扩展/层是否可用
bool HasExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

bool HasLayer(const std::vector<VkLayerProperties>& props, const char* name) {
    for (const auto& p : props) {
        if (std::strcmp(p.layerName, name) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::printf("[Validation] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats[0];
}

}  // namespace

namespace vk {

bool CreateInstance(Device& d) {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    const bool validationAvailable = HasLayer(layers, "VK_LAYER_KHRONOS_validation");
    if (!validationAvailable) {
        std::printf("[Device] WARNING: validation layer not available\n");
    }

    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());

    std::vector<const char*> enabledExts = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface"};
    if (validationAvailable) enabledExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ToyRenderGraph";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "ToyRenderGraph";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    ci.ppEnabledExtensionNames = enabledExts.data();
    if (validationAvailable) {
        ci.enabledLayerCount = 1;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        ci.ppEnabledLayerNames = &layer;
    }

    if (vkCreateInstance(&ci, nullptr, &d.instance) != VK_SUCCESS) {
        std::printf("[Device] failed to create instance\n");
        return false;
    }

    // Debug messenger（validation 消息回调）
    if (validationAvailable) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(d.instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = DebugCallback;
        createFn(d.instance, &mi, nullptr, &d.debugMessenger);
    }
    return true;
}

bool CreateSurface(Device& d, GLFWwindow* window) {
    return glfwCreateWindowSurface(d.instance, window, nullptr, &d.surface) == VK_SUCCESS;
}

bool PickPhysicalDevice(Device& d) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
    if (count == 0) {
        std::printf("[Device] no physical device found\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d.instance, &count, devices.data());

    for (const auto& pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        // 找一个同时支持 graphics 与 present 的队列族（单队列设计）
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
        for (uint32_t i = 0; i < qfCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, d.surface, &present);
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                d.physicalDevice = pd;
                d.graphicsFamily = i;
                return true;
            }
        }
    }
    std::printf("[Device] no suitable device (requires Vulkan 1.3 + graphics/present queue)\n");
    return false;
}

bool CreateDevice(Device& d) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d.graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    // Vulkan 1.3 特性：dynamic rendering + synchronization2
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(d.physicalDevice, &features2);

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &features13;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    const char* swapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = &swapchainExt;

    if (vkCreateDevice(d.physicalDevice, &ci, nullptr, &d.device) != VK_SUCCESS) {
        std::printf("[Device] failed to create logical device\n");
        return false;
    }
    vkGetDeviceQueue(d.device, d.graphicsFamily, 0, &d.queue);
    return true;
}

bool CreateSwapchain(Device& d, GLFWwindow* window) {
    // 查询表面能力与格式
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physicalDevice, d.surface, &caps);
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physicalDevice, d.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physicalDevice, d.surface, &fmtCount, formats.data());
    const VkSurfaceFormatKHR format = ChooseSurfaceFormat(formats);

    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    const SwapExtentChoice choice = ChooseSwapExtent(caps, fbWidth, fbHeight);
    if (!choice.ok) {
        std::printf("[Device] swapchain recreate skipped: framebuffer %dx%d "
                    "(window minimized?)\n", fbWidth, fbHeight);
        return false;
    }
    d.extent = choice.extent;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = d.surface;
    ci.minImageCount = 3;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = d.extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(d.device, &ci, nullptr, &d.swapchain) != VK_SUCCESS) {
        std::printf("[Device] failed to create swapchain\n");
        return false;
    }
    d.swapFormat = format.format;

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(d.device, d.swapchain, &imageCount, nullptr);
    d.swapImages.resize(imageCount);
    vkGetSwapchainImagesKHR(d.device, d.swapchain, &imageCount, d.swapImages.data());

    d.swapViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = d.swapImages[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = d.swapFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(d.device, &vi, nullptr, &d.swapViews[i]);
    }
    std::printf("[Device] swapchain: %ux%u, %u images, format=%d\n",
                d.extent.width, d.extent.height, imageCount, static_cast<int>(d.swapFormat));
    return true;
}

void DestroySwapchain(Device& d) {
    if (d.device == VK_NULL_HANDLE) return;
    for (const auto view : d.swapViews) vkDestroyImageView(d.device, view, nullptr);
    d.swapViews.clear();
    d.swapImages.clear();
    if (d.swapchain) vkDestroySwapchainKHR(d.device, d.swapchain, nullptr);
    d.swapchain = VK_NULL_HANDLE;
}

bool RecreateSwapchain(Device& d, GLFWwindow* window) {
    vkDeviceWaitIdle(d.device);
    DestroySwapchain(d);
    return CreateSwapchain(d, window);
}

SwapExtentChoice ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, int fbWidth,
                                  int fbHeight) {
    // 最小化/隐藏时 GLFW 报告的 framebuffer 为 0x0，此时创建交换链要么失败、
    // 要么得到非法尺寸，必须让调用方跳过重建并等待窗口恢复。
    if (fbWidth <= 0 || fbHeight <= 0) return {};

    uint32_t w = std::clamp(static_cast<uint32_t>(fbWidth), caps.minImageExtent.width,
                            caps.maxImageExtent.width);
    uint32_t h = std::clamp(static_cast<uint32_t>(fbHeight), caps.minImageExtent.height,
                            caps.maxImageExtent.height);
    if (w == 0 || h == 0) return {};
    return {true, {w, h}};
}

void DestroyDevice(Device& d) {
    if (d.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(d.device);
    DestroySwapchain(d);
    vkDestroyDevice(d.device, nullptr);
    d.device = VK_NULL_HANDLE;
    if (d.surface) vkDestroySurfaceKHR(d.instance, d.surface, nullptr);
    d.surface = VK_NULL_HANDLE;
    if (d.debugMessenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(d.instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroyFn(d.instance, d.debugMessenger, nullptr);
    }
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    d.instance = VK_NULL_HANDLE;
}

}  // namespace vk
