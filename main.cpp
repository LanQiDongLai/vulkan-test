// Vulkan + SDL2 绘制一个三角形的完整最小样例
//
// 初始化顺序:
//   SDL 窗口 -> Instance -> Surface -> 物理设备 -> 逻辑设备
//   -> Swapchain -> RenderPass -> Pipeline -> Framebuffer
//   -> 命令缓冲 + 同步对象 -> 顶点缓冲 -> 渲染循环
//
// 顶点数据 (位置/颜色) 由 CPU 端在 kVertices 里定义，通过暂存缓冲上传到
// 设备本地的顶点缓冲，再在命令缓冲里绑定给管线。
//
// 多重采样: 渲染先写进 4x MSAA 图像，render pass 结束时自动 resolve
// 到交换链图像上，从而消掉三角形边缘的锯齿。
//
// 编译: cmake -B build && cmake --build build
// 运行: ./build/main

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// 由 CMake 传入，指向编译好的 .spv 所在目录
#ifndef SHADER_DIR
#define SHADER_DIR "shaders"
#endif

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
// 同时可以有多少帧处于"飞行中"(已提交但未完成)，2 表示双缓冲
constexpr uint32_t kMaxFramesInFlight = 2;

// 出错就抛异常，避免到处写 if (result != VK_SUCCESS)
#define VK_CHECK(expr)                                                       \
  do {                                                                       \
    VkResult vkResult_ = (expr);                                             \
    if (vkResult_ != VK_SUCCESS) {                                           \
      throw std::runtime_error(std::string(#expr) + " 失败, VkResult = " +   \
                               std::to_string(static_cast<int>(vkResult_))); \
    }                                                                        \
  } while (false)

// ---------------------------------------------------------------------------
// 顶点数据: 由 CPU 端提供，再通过顶点缓冲上传给 GPU
// ---------------------------------------------------------------------------

struct Vertex {
  float pos[2];
  float color[3];

  // 顶点缓冲的绑定描述: 一个 vertex buffer, 每个顶点 sizeof(Vertex) 字节
  static VkVertexInputBindingDescription bindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
  }

  // 属性描述: 告诉管线 Vertex 里哪几个 float 是位置、哪几个是颜色
  static std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    // location 0 -> vec2 位置
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, pos);
    // location 1 -> vec3 颜色
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, color);
    return attributes;
  }
};

const std::array<Vertex, 3> kVertices = {{
    {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},  // 上, 红
    {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},   // 右下, 绿
    {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},  // 左下, 蓝
}};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::vector<char> readFile(const std::string &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("无法打开文件: " + path +
                             " (是否已用 cmake --build build 编译着色器?)");
  }
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), static_cast<std::streamsize>(size));
  return buffer;
}

bool hasValidationLayer() {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const auto &layer : layers) {
    if (std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation") {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// 所有 Vulkan 对象都放在这里，方便传参和统一清理
// ---------------------------------------------------------------------------

struct App {
  SDL_Window *window = nullptr;
  bool quitRequested = false;

  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  VkQueue presentQueue = VK_NULL_HANDLE;
  uint32_t graphicsFamily = UINT32_MAX;
  uint32_t presentFamily = UINT32_MAX;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D swapchainExtent{};
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainImageViews;
  std::vector<VkFramebuffer> framebuffers;

  // 多重采样: 渲染先画到这张 MSAA 图像，再由 render pass resolve 到交换链图像。
  // 它的尺寸依赖交换链，所以要跟着交换链一起重建。
  VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
  VkImage msaaImage = VK_NULL_HANDLE;
  VkDeviceMemory msaaImageMemory = VK_NULL_HANDLE;
  VkImageView msaaImageView = VK_NULL_HANDLE;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  // 顶点缓冲，存在设备本地内存里
  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
  uint32_t vertexCount = static_cast<uint32_t>(kVertices.size());

  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers{};
  std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable{};
  std::array<VkFence, kMaxFramesInFlight> inFlightFences{};
  // 注意: 这个信号量按 swapchain image 索引，不是按帧槽位。
  // 呈现操作绑定在 image 上，若按帧槽位复用，可能在该 image 尚未被重新获取时
  // 就重复使用同一个信号量而被校验层报 VUID-vkQueueSubmit-pSignalSemaphores-00067
  std::vector<VkSemaphore> renderFinished;
  uint32_t currentFrame = 0;
  bool framebufferResized = false;
};

// ---------------------------------------------------------------------------
// Instance / Surface
// ---------------------------------------------------------------------------

void createInstance(App &app) {
  // SDL 会告诉我们当前视频后端 (X11/Wayland) 需要哪些 instance 扩展
  uint32_t extensionCount = 0;
  SDL_Vulkan_GetInstanceExtensions(app.window, &extensionCount, nullptr);
  std::vector<const char *> extensions(extensionCount);
  SDL_Vulkan_GetInstanceExtensions(app.window, &extensionCount, extensions.data());

  std::vector<const char *> layers;
  if (hasValidationLayer()) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
  } else {
    std::cout << "[提示] 未安装 VK_LAYER_KHRONOS_validation，跳过校验层\n";
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "vulkan-triangle";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "none";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();
  createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
  createInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

  VK_CHECK(vkCreateInstance(&createInfo, nullptr, &app.instance));
}

void createSurface(App &app) {
  if (!SDL_Vulkan_CreateSurface(app.window, app.instance, &app.surface)) {
    throw std::runtime_error(std::string("创建 Vulkan Surface 失败: ") +
                             SDL_GetError());
  }
}

// ---------------------------------------------------------------------------
// 物理设备 / 逻辑设备
// ---------------------------------------------------------------------------

void pickPhysicalDevice(App &app) {
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(app.instance, &deviceCount, nullptr);
  if (deviceCount == 0) {
    throw std::runtime_error("没有找到支持 Vulkan 的显卡");
  }
  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(app.instance, &deviceCount, devices.data());

  int bestScore = -1;
  for (VkPhysicalDevice device : devices) {
    // 找图形队列，以及能向当前 surface 呈现的队列
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          graphics == UINT32_MAX) {
        graphics = i;
      }
      VkBool32 presentSupport = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, app.surface, &presentSupport);
      if (presentSupport && present == UINT32_MAX) {
        present = i;
      }
    }
    if (graphics == UINT32_MAX || present == UINT32_MAX) {
      continue;
    }

    // 必须支持 swapchain 扩展
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, exts.data());
    bool hasSwapchain = false;
    for (const auto &ext : exts) {
      if (std::string(ext.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
        hasSwapchain = true;
      }
    }
    if (!hasSwapchain) {
      continue;
    }

    // surface 必须至少有可用的格式和呈现模式
    uint32_t formatCount = 0;
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, app.surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, app.surface, &modeCount, nullptr);
    if (formatCount == 0 || modeCount == 0) {
      continue;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);

    // 独立显卡优先
    int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 100;
    score += static_cast<int>(props.limits.maxImageDimension2D / 1024);

    if (score > bestScore) {
      bestScore = score;
      app.physicalDevice = device;
      app.graphicsFamily = graphics;
      app.presentFamily = present;
      std::cout << "使用显卡: " << props.deviceName << "\n";
    }
  }

  if (app.physicalDevice == VK_NULL_HANDLE) {
    throw std::runtime_error("没有找到满足要求的显卡");
  }
}

void createLogicalDevice(App &app) {
  // 图形队列和呈现队列可能属于不同的 queue family
  std::vector<uint32_t> uniqueFamilies = {app.graphicsFamily};
  if (app.presentFamily != app.graphicsFamily) {
    uniqueFamilies.push_back(app.presentFamily);
  }

  const float priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queueInfos;
  for (uint32_t family : uniqueFamilies) {
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    queueInfos.push_back(queueInfo);
  }

  const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkPhysicalDeviceFeatures features{};

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
  createInfo.pQueueCreateInfos = queueInfos.data();
  createInfo.pEnabledFeatures = &features;
  createInfo.enabledExtensionCount = 1;
  createInfo.ppEnabledExtensionNames = deviceExtensions;

  VK_CHECK(vkCreateDevice(app.physicalDevice, &createInfo, nullptr, &app.device));

  vkGetDeviceQueue(app.device, app.graphicsFamily, 0, &app.graphicsQueue);
  vkGetDeviceQueue(app.device, app.presentFamily, 0, &app.presentQueue);
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) {
  // 优先 SRGB，颜色显示才正确
  for (const auto &format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return format;
    }
  }
  return formats[0];
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR> &modes) {
  // MAILBOX 延迟更低，没有就退回 FIFO(垂直同步)
  for (VkPresentModeKHR mode : modes) {
    if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return mode;
    }
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
  const VkCompositeAlphaFlagBitsKHR candidates[] = {
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
  for (VkCompositeAlphaFlagBitsKHR flag : candidates) {
    if (supported & flag) {
      return flag;
    }
  }
  return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

void createSwapchain(App &app) {
  VkSurfaceCapabilitiesKHR caps{};
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app.physicalDevice, app.surface,
                                                     &caps));

  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(app.physicalDevice, app.surface, &formatCount,
                                       nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(app.physicalDevice, app.surface, &formatCount,
                                       formats.data());

  uint32_t modeCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(app.physicalDevice, app.surface, &modeCount,
                                           nullptr);
  std::vector<VkPresentModeKHR> modes(modeCount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(app.physicalDevice, app.surface, &modeCount,
                                           modes.data());

  const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
  const VkPresentModeKHR presentMode = choosePresentMode(modes);

  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    // currentExtent 为 UINT32_MAX 表示大小由我们自己决定
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_Vulkan_GetDrawableSize(app.window, &drawableWidth, &drawableHeight);
    extent.width = std::clamp(static_cast<uint32_t>(drawableWidth),
                              caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(static_cast<uint32_t>(drawableHeight),
                               caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    imageCount = caps.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = app.surface;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  const uint32_t familyIndices[] = {app.graphicsFamily, app.presentFamily};
  if (app.graphicsFamily != app.presentFamily) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = familyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = caps.currentTransform;
  createInfo.compositeAlpha = chooseCompositeAlpha(caps.supportedCompositeAlpha);
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = VK_NULL_HANDLE;

  VK_CHECK(vkCreateSwapchainKHR(app.device, &createInfo, nullptr, &app.swapchain));

  uint32_t actualCount = 0;
  vkGetSwapchainImagesKHR(app.device, app.swapchain, &actualCount, nullptr);
  app.swapchainImages.resize(actualCount);
  vkGetSwapchainImagesKHR(app.device, app.swapchain, &actualCount,
                          app.swapchainImages.data());

  app.swapchainFormat = surfaceFormat.format;
  app.swapchainExtent = extent;
}

void createImageViews(App &app) {
  app.swapchainImageViews.resize(app.swapchainImages.size());
  for (size_t i = 0; i < app.swapchainImages.size(); ++i) {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = app.swapchainImages[i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = app.swapchainFormat;
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    VK_CHECK(vkCreateImageView(app.device, &createInfo, nullptr,
                               &app.swapchainImageViews[i]));
  }
}

// ---------------------------------------------------------------------------
// RenderPass / Pipeline
// ---------------------------------------------------------------------------

void createRenderPass(App &app) {
  // 附件 0: 多重采样颜色附件，片元着色器先写到这里
  VkAttachmentDescription msaaAttachment{};
  msaaAttachment.format = app.swapchainFormat;
  msaaAttachment.samples = app.msaaSamples;
  msaaAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // 每帧先清屏
  msaaAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  msaaAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  msaaAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  msaaAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  msaaAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  // 附件 1: 普通的 1x 附件，也就是交换链图像，作为 resolve 的目标
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = app.swapchainFormat;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 内容会被 resolve 覆盖
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference msaaRef{};
  msaaRef.attachment = 0;
  msaaRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference resolveRef{};
  resolveRef.attachment = 1;
  resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &msaaRef;       // 着色器写的是 MSAA 附件
  subpass.pResolveAttachments = &resolveRef;  // 子通道结束时自动 resolve 到这里

  // 让颜色附件的写入发生在正确的阶段
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  const VkAttachmentDescription attachments[] = {msaaAttachment, colorAttachment};

  VkRenderPassCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  createInfo.attachmentCount = 2;
  createInfo.pAttachments = attachments;
  createInfo.subpassCount = 1;
  createInfo.pSubpasses = &subpass;
  createInfo.dependencyCount = 1;
  createInfo.pDependencies = &dependency;

  VK_CHECK(vkCreateRenderPass(app.device, &createInfo, nullptr, &app.renderPass));
}

VkShaderModule createShaderModule(VkDevice device, const std::string &path) {
  const std::vector<char> code = readFile(path);
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

  VkShaderModule module = VK_NULL_HANDLE;
  VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &module));
  return module;
}

void createGraphicsPipeline(App &app) {
  const std::string shaderDir = SHADER_DIR;
  VkShaderModule vertModule =
      createShaderModule(app.device, shaderDir + "/triangle.vert.spv");
  VkShaderModule fragModule =
      createShaderModule(app.device, shaderDir + "/triangle.frag.spv");

  VkPipelineShaderStageCreateInfo vertStage{};
  vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertStage.module = vertModule;
  vertStage.pName = "main";

  VkPipelineShaderStageCreateInfo fragStage{};
  fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragStage.module = fragModule;
  fragStage.pName = "main";

  const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

  // 顶点数据来自顶点缓冲，这里描述缓冲的绑定方式和属性布局
  const VkVertexInputBindingDescription binding = Vertex::bindingDescription();
  const std::array<VkVertexInputAttributeDescription, 2> attributes =
      Vertex::attributeDescriptions();

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attributes.size());
  vertexInput.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // viewport / scissor 用动态状态，改窗口大小时无需重建管线
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.cullMode = VK_CULL_MODE_NONE;  // 样例不剔除任何面
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  // 必须和 render pass 里颜色附件的采样数一致，否则创建管线会失败
  multisampling.rasterizationSamples = app.msaaSamples;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &blendAttachment;

  const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamicStates;

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VK_CHECK(vkCreatePipelineLayout(app.device, &layoutInfo, nullptr, &app.pipelineLayout));

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = app.pipelineLayout;
  pipelineInfo.renderPass = app.renderPass;
  pipelineInfo.subpass = 0;

  VK_CHECK(vkCreateGraphicsPipelines(app.device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                     nullptr, &app.pipeline));

  // 管线创建完成后着色器模块就可以销毁了
  vkDestroyShaderModule(app.device, fragModule, nullptr);
  vkDestroyShaderModule(app.device, vertModule, nullptr);
}

void createFramebuffers(App &app) {
  app.framebuffers.resize(app.swapchainImageViews.size());
  for (size_t i = 0; i < app.swapchainImageViews.size(); ++i) {
    // 顺序必须和 render pass 里的附件索引一致: 0 = MSAA, 1 = resolve 目标
    const VkImageView attachments[] = {app.msaaImageView,
                                       app.swapchainImageViews[i]};

    VkFramebufferCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    createInfo.renderPass = app.renderPass;
    createInfo.attachmentCount = 2;
    createInfo.pAttachments = attachments;
    createInfo.width = app.swapchainExtent.width;
    createInfo.height = app.swapchainExtent.height;
    createInfo.layers = 1;

    VK_CHECK(vkCreateFramebuffer(app.device, &createInfo, nullptr, &app.framebuffers[i]));
  }
}

// ---------------------------------------------------------------------------
// 命令缓冲 / 同步对象
// ---------------------------------------------------------------------------

void createCommandBuffers(App &app) {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = app.graphicsFamily;
  VK_CHECK(vkCreateCommandPool(app.device, &poolInfo, nullptr, &app.commandPool));

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = app.commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = kMaxFramesInFlight;
  VK_CHECK(vkAllocateCommandBuffers(app.device, &allocInfo, app.commandBuffers.data()));
}

void createSyncObjects(App &app) {
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 第一帧无需等待

  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    VK_CHECK(vkCreateSemaphore(app.device, &semaphoreInfo, nullptr,
                               &app.imageAvailable[i]));
    VK_CHECK(vkCreateFence(app.device, &fenceInfo, nullptr, &app.inFlightFences[i]));
  }
}

// 每个 swapchain image 一个"渲染完成"信号量，跟随 swapchain 一起重建
void createRenderFinishedSemaphores(App &app) {
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  app.renderFinished.assign(app.swapchainImages.size(), VK_NULL_HANDLE);
  for (VkSemaphore &semaphore : app.renderFinished) {
    VK_CHECK(vkCreateSemaphore(app.device, &semaphoreInfo, nullptr, &semaphore));
  }
}

// ---------------------------------------------------------------------------
// 顶点缓冲
// ---------------------------------------------------------------------------

// 在显存类型里挑一个同时满足 typeFilter 和所需属性(可见/本地等)的
uint32_t findMemoryType(App &app, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  vkGetPhysicalDeviceMemoryProperties(app.physicalDevice, &memoryProperties);

  for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
    const bool typeMatches = (typeFilter & (1u << i)) != 0;
    const bool propertiesMatch =
        (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
    if (typeMatches && propertiesMatch) {
      return i;
    }
  }
  throw std::runtime_error("找不到满足要求的内存类型");
}

void createBuffer(App &app, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkBuffer &buffer,
                  VkDeviceMemory &memory) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateBuffer(app.device, &bufferInfo, nullptr, &buffer));

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(app.device, buffer, &requirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = requirements.size;
  allocInfo.memoryTypeIndex =
      findMemoryType(app, requirements.memoryTypeBits, properties);
  VK_CHECK(vkAllocateMemory(app.device, &allocInfo, nullptr, &memory));
  VK_CHECK(vkBindBufferMemory(app.device, buffer, memory, 0));
}

// 用一次性命令缓冲把数据从 src 拷到 dst (上传完就同步等待)
void copyBuffer(App &app, VkBuffer src, VkBuffer dst, VkDeviceSize size) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = app.commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(app.device, &allocInfo, &commandBuffer));

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

  VkBufferCopy copyRegion{};
  copyRegion.srcOffset = 0;
  copyRegion.dstOffset = 0;
  copyRegion.size = size;
  vkCmdCopyBuffer(commandBuffer, src, dst, 1, &copyRegion);

  VK_CHECK(vkEndCommandBuffer(commandBuffer));

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  VK_CHECK(vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(app.graphicsQueue));

  vkFreeCommandBuffers(app.device, app.commandPool, 1, &commandBuffer);
}

// 把 kVertices 上传到设备本地的顶点缓冲
void createVertexBuffer(App &app) {
  const VkDeviceSize bufferSize = sizeof(Vertex) * kVertices.size();

  // 1) 先在 CPU 可见的暂存缓冲里填数据
  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  createBuffer(app, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               stagingBuffer, stagingMemory);

  void *mapped = nullptr;
  VK_CHECK(vkMapMemory(app.device, stagingMemory, 0, bufferSize, 0, &mapped));
  std::memcpy(mapped, kVertices.data(), static_cast<size_t>(bufferSize));
  vkUnmapMemory(app.device, stagingMemory);

  // 2) 真正的顶点缓冲放设备本地内存，GPU 读取最快
  createBuffer(app, bufferSize,
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, app.vertexBuffer,
               app.vertexBufferMemory);

  // 3) 拷贝过去，然后释放暂存缓冲
  copyBuffer(app, stagingBuffer, app.vertexBuffer, bufferSize);

  vkDestroyBuffer(app.device, stagingBuffer, nullptr);
  vkFreeMemory(app.device, stagingMemory, nullptr);
}

// ---------------------------------------------------------------------------
// 多重采样 (MSAA)
// ---------------------------------------------------------------------------

// 挑一个可用采样数。目标 4x，不支持就逐级降。
// 需要同时满足: 设备队列的 framebufferColorSampleCounts 和该格式的 sampleCounts
VkSampleCountFlagBits chooseSampleCount(App &app) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(app.physicalDevice, &props);

  VkImageFormatProperties formatProps{};
  const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
      app.physicalDevice, app.swapchainFormat, VK_IMAGE_TYPE_2D,
      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
      &formatProps);

  VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts;
  if (result == VK_SUCCESS) {
    counts &= formatProps.sampleCounts;
  }

  if (counts & VK_SAMPLE_COUNT_4_BIT) {
    std::cout << "MSAA: 4x\n";
    return VK_SAMPLE_COUNT_4_BIT;
  }
  if (counts & VK_SAMPLE_COUNT_2_BIT) {
    std::cout << "MSAA: 2x (不支持 4x)\n";
    return VK_SAMPLE_COUNT_2_BIT;
  }
  std::cout << "MSAA: 不可用，回退到 1x\n";
  return VK_SAMPLE_COUNT_1_BIT;
}

// 创建多重采样颜色图像 (只当渲染目标用，不需要 CPU 访问)
void createMsaaImage(App &app) {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = app.swapchainExtent.width;
  imageInfo.extent.height = app.swapchainExtent.height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = app.swapchainFormat;  // 必须和交换链格式一致，resolve 才能做
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  imageInfo.samples = app.msaaSamples;  // 关键: 采样数
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateImage(app.device, &imageInfo, nullptr, &app.msaaImage));

  // 图像也要显式分配并绑定显存 (和 buffer 一样的两步)
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(app.device, app.msaaImage, &requirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = requirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(
      app, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(app.device, &allocInfo, nullptr, &app.msaaImageMemory));
  VK_CHECK(vkBindImageMemory(app.device, app.msaaImage, app.msaaImageMemory, 0));

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = app.msaaImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = app.swapchainFormat;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;
  VK_CHECK(vkCreateImageView(app.device, &viewInfo, nullptr, &app.msaaImageView));
}

void destroyMsaaImage(App &app) {
  if (app.msaaImageView != VK_NULL_HANDLE) {
    vkDestroyImageView(app.device, app.msaaImageView, nullptr);
    app.msaaImageView = VK_NULL_HANDLE;
  }
  if (app.msaaImage != VK_NULL_HANDLE) {
    vkDestroyImage(app.device, app.msaaImage, nullptr);
    app.msaaImage = VK_NULL_HANDLE;
  }
  if (app.msaaImageMemory != VK_NULL_HANDLE) {
    vkFreeMemory(app.device, app.msaaImageMemory, nullptr);
    app.msaaImageMemory = VK_NULL_HANDLE;
  }
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void recordCommandBuffer(App &app, VkCommandBuffer commandBuffer, uint32_t imageIndex) {
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

  VkClearValue clearColor{};
  clearColor.color = {{0.05f, 0.05f, 0.08f, 1.0f}};

  VkRenderPassBeginInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  renderPassInfo.renderPass = app.renderPass;
  renderPassInfo.framebuffer = app.framebuffers[imageIndex];
  renderPassInfo.renderArea.offset = {0, 0};
  renderPassInfo.renderArea.extent = app.swapchainExtent;
  renderPassInfo.clearValueCount = 1;
  renderPassInfo.pClearValues = &clearColor;

  vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipeline);

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(app.swapchainExtent.width);
  viewport.height = static_cast<float>(app.swapchainExtent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = app.swapchainExtent;
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  // 绑定顶点缓冲: binding 0, 从偏移 0 开始读
  const VkDeviceSize vertexBufferOffset = 0;
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, &app.vertexBuffer,
                         &vertexBufferOffset);

  // 绘制顶点缓冲里的所有顶点
  vkCmdDraw(commandBuffer, app.vertexCount, 1, 0, 0);

  vkCmdEndRenderPass(commandBuffer);
  VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

void cleanupSwapchain(App &app) {
  for (VkFramebuffer framebuffer : app.framebuffers) {
    vkDestroyFramebuffer(app.device, framebuffer, nullptr);
  }
  app.framebuffers.clear();

  // MSAA 图像尺寸跟着交换链走，所以也在这里销毁
  destroyMsaaImage(app);

  for (VkImageView view : app.swapchainImageViews) {
    vkDestroyImageView(app.device, view, nullptr);
  }
  app.swapchainImageViews.clear();

  for (VkSemaphore semaphore : app.renderFinished) {
    vkDestroySemaphore(app.device, semaphore, nullptr);
  }
  app.renderFinished.clear();

  if (app.swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(app.device, app.swapchain, nullptr);
    app.swapchain = VK_NULL_HANDLE;
  }
}

void recreateSwapchain(App &app) {
  // 窗口最小化时绘制区大小为 0，先等它恢复
  int width = 0;
  int height = 0;
  SDL_Vulkan_GetDrawableSize(app.window, &width, &height);
  while ((width == 0 || height == 0) && !app.quitRequested) {
    SDL_Event event;
    SDL_WaitEvent(&event);
    if (event.type == SDL_QUIT) {
      app.quitRequested = true;
      return;
    }
    SDL_Vulkan_GetDrawableSize(app.window, &width, &height);
  }

  vkDeviceWaitIdle(app.device);

  cleanupSwapchain(app);
  createSwapchain(app);
  createMsaaImage(app);  // 尺寸变了，MSAA 图像要按新尺寸重建
  createRenderFinishedSemaphores(app);
  createImageViews(app);
  createFramebuffers(app);
}

void drawFrame(App &app) {
  // 等待上一轮使用该槽位的帧结束
  VK_CHECK(vkWaitForFences(app.device, 1, &app.inFlightFences[app.currentFrame],
                           VK_TRUE, UINT64_MAX));

  uint32_t imageIndex = 0;
  const VkResult acquireResult = vkAcquireNextImageKHR(
      app.device, app.swapchain, UINT64_MAX, app.imageAvailable[app.currentFrame],
      VK_NULL_HANDLE, &imageIndex);
  if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain(app);
    return;
  }
  if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("vkAcquireNextImageKHR 失败: " +
                             std::to_string(static_cast<int>(acquireResult)));
  }

  VK_CHECK(vkResetFences(app.device, 1, &app.inFlightFences[app.currentFrame]));

  VkCommandBuffer commandBuffer = app.commandBuffers[app.currentFrame];
  VK_CHECK(vkResetCommandBuffer(commandBuffer, 0));
  recordCommandBuffer(app, commandBuffer, imageIndex);

  const VkSemaphore waitSemaphores[] = {app.imageAvailable[app.currentFrame]};
  const VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  const VkSemaphore signalSemaphores[] = {app.renderFinished[imageIndex]};

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  VK_CHECK(vkQueueSubmit(app.graphicsQueue, 1, &submitInfo,
                         app.inFlightFences[app.currentFrame]));

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = signalSemaphores;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &app.swapchain;
  presentInfo.pImageIndices = &imageIndex;

  const VkResult presentResult = vkQueuePresentKHR(app.presentQueue, &presentInfo);
  if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
      presentResult == VK_SUBOPTIMAL_KHR || app.framebufferResized) {
    app.framebufferResized = false;
    recreateSwapchain(app);
  } else if (presentResult != VK_SUCCESS) {
    throw std::runtime_error("vkQueuePresentKHR 失败: " +
                             std::to_string(static_cast<int>(presentResult)));
  }

  app.currentFrame = (app.currentFrame + 1) % kMaxFramesInFlight;
}

// ---------------------------------------------------------------------------
// 清理
// ---------------------------------------------------------------------------

void cleanup(App &app) {
  if (app.device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(app.device);

    cleanupSwapchain(app);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
      vkDestroySemaphore(app.device, app.imageAvailable[i], nullptr);
      vkDestroyFence(app.device, app.inFlightFences[i], nullptr);
    }

    if (app.vertexBuffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(app.device, app.vertexBuffer, nullptr);
    }
    if (app.vertexBufferMemory != VK_NULL_HANDLE) {
      vkFreeMemory(app.device, app.vertexBufferMemory, nullptr);
    }

    if (app.commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(app.device, app.commandPool, nullptr);
    }
    if (app.pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(app.device, app.pipeline, nullptr);
    }
    if (app.pipelineLayout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(app.device, app.pipelineLayout, nullptr);
    }
    if (app.renderPass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(app.device, app.renderPass, nullptr);
    }

    vkDestroyDevice(app.device, nullptr);
  }

  if (app.surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(app.instance, app.surface, nullptr);
  }
  if (app.instance != VK_NULL_HANDLE) {
    vkDestroyInstance(app.instance, nullptr);
  }
  if (app.window != nullptr) {
    SDL_DestroyWindow(app.window);
  }
  SDL_Quit();
}

void run(App &app) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    throw std::runtime_error(std::string("SDL_Init 失败: ") + SDL_GetError());
  }

  app.window = SDL_CreateWindow("Vulkan Triangle", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_ALLOW_HIGHDPI);
  if (app.window == nullptr) {
    throw std::runtime_error(std::string("创建窗口失败: ") + SDL_GetError());
  }

  createInstance(app);
  createSurface(app);
  pickPhysicalDevice(app);
  createLogicalDevice(app);
  createSwapchain(app);
  // 采样数要用交换链的格式去查，所以必须放在 createSwapchain 之后
  app.msaaSamples = chooseSampleCount(app);
  createMsaaImage(app);
  createImageViews(app);
  createRenderPass(app);
  createGraphicsPipeline(app);
  createFramebuffers(app);
  createCommandBuffers(app);
  createSyncObjects(app);
  createRenderFinishedSemaphores(app);
  createVertexBuffer(app);  // 需要用到 commandPool，所以要放在 createCommandBuffers 之后

  std::cout << "渲染中，按 ESC 或关闭窗口退出\n";

  while (!app.quitRequested) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        app.quitRequested = true;
      } else if (event.type == SDL_WINDOWEVENT &&
                 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        app.framebufferResized = true;
      } else if (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_ESCAPE) {
        app.quitRequested = true;
      }
    }

    drawFrame(app);
  }

  vkDeviceWaitIdle(app.device);
  std::cout << "退出\n";
}

}  // namespace

int main() {
  App app;
  try {
    run(app);
  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << "\n";
    cleanup(app);
    return 1;
  }
  cleanup(app);
  return 0;
}
