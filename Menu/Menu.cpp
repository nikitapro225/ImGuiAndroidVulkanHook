#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>

#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_vulkan.h"

#include "../Dobby/dobby.h"

#define LOG_TAG "ImGuiVulkanHook"
#define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define MAX_FRAMES_IN_FLIGHT 3

ANativeWindow* g_NativeWindow = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
VkQueue g_Queue = VK_NULL_HANDLE;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkCommandPool g_CommandPool = VK_NULL_HANDLE;
VkRenderPass g_RenderPass = VK_NULL_HANDLE;
VkSurfaceKHR g_Surface = VK_NULL_HANDLE;
VkSwapchainKHR g_Swapchain = VK_NULL_HANDLE;
VkExtent2D g_SwapChainExtent = {0, 0};
std::vector<VkImage> g_SwapChainImages;
std::vector<VkFramebuffer> g_Framebuffers;
std::vector<VkImageView> g_SwapChainImageViews;

bool g_ImGuiInitialized = false;
bool g_InitInProgress = false;
static bool g_InSubmit = false;
bool g_MenuVisible = false;
int g_MenuAutoEnabler = 0;

struct RenderContext {
    VkCommandBuffer commandBuffer;
    VkFence fence;
    bool inUse;
    
    RenderContext() : commandBuffer(VK_NULL_HANDLE), fence(VK_NULL_HANDLE), inUse(false) {}
};

static RenderContext g_RenderContexts[2];
static uint32_t g_CurrentContext = 0;


uint32_t findGraphicsQueueFamily() {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkCommandPool createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = findGraphicsQueueFamily();

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(g_Device, &poolInfo, nullptr, &commandPool);
    if (result == VK_SUCCESS) {
        LOGD("Command pool created successfully");
    } else {
        LOGD("Failed to create command pool: %d", result);
    }
    return commandPool;
}

VkCommandBuffer createCommandBuffer() {
    if (g_Device == VK_NULL_HANDLE || g_CommandPool == VK_NULL_HANDLE) {
        LOGD("Device or CommandPool is null");
        return VK_NULL_HANDLE;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    VkResult result = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (result == VK_SUCCESS) {
        LOGD("Command buffer created successfully");
    } else {
        LOGD("Failed to create command buffer: %d", result);
        return VK_NULL_HANDLE;
    }
    return commandBuffer;
}

VkRenderPass createImGuiRenderPass() {
    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // Important: LOAD to preserve Unity rendering
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment = {};
    color_attachment.attachment = 0;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;

    VkRenderPass renderPass;
    VkResult result = vkCreateRenderPass(g_Device, &info, nullptr, &renderPass);
    if (result != VK_SUCCESS) {
        LOGD("Failed to create render pass: %d", result);
        return VK_NULL_HANDLE;
    }

    return renderPass;
}

void createImGuiFramebuffers() {
    g_Framebuffers.resize(g_SwapChainImages.size());
    g_SwapChainImageViews.resize(g_SwapChainImages.size());

    for (size_t i = 0; i < g_SwapChainImages.size(); i++) {
        VkImageViewCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = g_SwapChainImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(g_Device, &createInfo, nullptr, &g_SwapChainImageViews[i]) != VK_SUCCESS) {
            LOGD("Failed to create image view %zu", i);
            continue;
        }

        VkImageView attachments[] = { g_SwapChainImageViews[i] };
        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = g_RenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = g_SwapChainExtent.width;
        framebufferInfo.height = g_SwapChainExtent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(g_Device, &framebufferInfo, nullptr, &g_Framebuffers[i]) != VK_SUCCESS) {
            LOGD("Failed to create framebuffer %zu", i);
        }
    }
}

bool createDescriptorPool() {
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(g_Device, &pool_info, nullptr, &g_DescriptorPool) != VK_SUCCESS) {
        LOGD("Failed to create descriptor pool");
        return false;
    }

    return true;
}

void initRenderContexts() {
    LOGD("Initializing render contexts...");
    for (int i = 0; i < 2; i++) {
        g_RenderContexts[i].commandBuffer = createCommandBuffer();
        if (g_RenderContexts[i].commandBuffer == VK_NULL_HANDLE) {
            LOGD("Failed to create command buffer for context %d", i);
            continue;
        }
        
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        
        if (vkCreateFence(g_Device, &fenceInfo, nullptr, &g_RenderContexts[i].fence) != VK_SUCCESS) {
            LOGD("Failed to create fence for context %d", i);
            vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &g_RenderContexts[i].commandBuffer);
            g_RenderContexts[i].commandBuffer = VK_NULL_HANDLE;
            continue;
        }
        
        g_RenderContexts[i].inUse = false;
        LOGD("Context %d initialized successfully", i);
    }
}

bool uploadFonts() {
    VkCommandBuffer commandBuffer = createCommandBuffer();
    if (commandBuffer == VK_NULL_HANDLE) return false;

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }

    ImGui_ImplVulkan_CreateFontsTexture();

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VkFence fence;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    
    if (vkCreateFence(g_Device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }

    if (vkQueueSubmit(g_Queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        vkDestroyFence(g_Device, fence, nullptr);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }

    vkWaitForFences(g_Device, 1, &fence, VK_TRUE, UINT64_MAX);
    
    vkDestroyFence(g_Device, fence, nullptr);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    return true;
}

bool initializeImGui() {
    if (!createDescriptorPool()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize = ImVec2(g_SwapChainExtent.width, g_SwapChainExtent.height);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    if (!ImGui_ImplAndroid_Init(g_NativeWindow)) {
        LOGD("Failed to initialize Android backend");
        return false;
    }

    g_RenderPass = createImGuiRenderPass();
    if (g_RenderPass == VK_NULL_HANDLE) return false;

    createImGuiFramebuffers();
    if (g_Framebuffers.empty()) return false;

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = findGraphicsQueueFamily();
    init_info.Queue = g_Queue;
    init_info.RenderPass = g_RenderPass;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.MinImageCount = 2;
    init_info.ImageCount = g_SwapChainImages.size();
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = nullptr;

    if (!ImGui_ImplVulkan_Init(&init_info)) return false;
    
    if (!uploadFonts()) return false;

    return true;
}

void DesignAndDrawMenu() {
    if (!g_MenuVisible) return;

    static bool firstTime = true;
    static ImVec2 pos(10, 10);
    
    if (firstTime) {
        ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
        firstTime = false;
    }

    ImGui::Begin("Mod Menu", &g_MenuVisible, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
    ImGui::Text("Menu Test");
    
    if (ImGui::Button("Test Button", ImVec2(280, 40))) {
        LOGD("Button clicked!");
    }

    ImGui::PopStyleColor();
    ImGui::End();
}

// ----------------------------- HOOKS -----------------------------

VkResult (*original_vkQueueSubmit)(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
VkResult hooked_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    if (g_InSubmit) { // Avoid recursion
        return original_vkQueueSubmit(queue, submitCount, pSubmits, fence);
    }
    
    g_InSubmit = true;

    // First execute Unity rendering
    VkResult result = original_vkQueueSubmit(queue, submitCount, pSubmits, fence);

    if (result != VK_SUCCESS) {
        LOGD("Original vkQueueSubmit failed");
        g_InSubmit = false;
        return result;
    }

    if (!g_ImGuiInitialized) {
        LOGD("ImGui not initialized yet");
        g_InSubmit = false;
        return result;
    }

    // Auto enable menu after 20 seconds (at 60 FPS) (only for fast debugging)
    if(!g_MenuVisible) {
        if (g_MenuAutoEnabler < 1200) {
            g_MenuAutoEnabler++;
        } else {
            g_MenuVisible = true;
        }
    }

    // Wait for the queue to be idle before rendering ImGui
    vkQueueWaitIdle(queue);

    try {
        RenderContext& currentContext = g_RenderContexts[g_CurrentContext];
        
        if (currentContext.commandBuffer == VK_NULL_HANDLE) {
            LOGD("Current context command buffer is null");
            return result;
        }

        if (currentContext.inUse) {
            if (vkWaitForFences(g_Device, 1, &currentContext.fence, VK_TRUE, 1000000) != VK_SUCCESS) {
                return result;
            }
            vkResetFences(g_Device, 1, &currentContext.fence);
        }

        vkResetCommandBuffer(currentContext.commandBuffer, 0);
        
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        if (vkBeginCommandBuffer(currentContext.commandBuffer, &beginInfo) != VK_SUCCESS) {
            LOGD("Failed to begin command buffer from current context");
            return result;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        
        DesignAndDrawMenu();
        
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();

        if (draw_data && g_MenuVisible) {
            //LOGD("Drawing ImGui frame");

            uint32_t imageIndex = 0;
            // Get index of current image from command buffer, and use modulo to ensure it's within bounds
            if (pSubmits && pSubmits->commandBufferCount > 0) {
                VkCommandBuffer cmdBuffer = pSubmits->pCommandBuffers[0];
                imageIndex = reinterpret_cast<uintptr_t>(cmdBuffer) % g_Framebuffers.size();
            }

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = g_RenderPass;
            renderPassInfo.framebuffer = g_Framebuffers[imageIndex];
            renderPassInfo.renderArea.offset = {0, 0};
            renderPassInfo.renderArea.extent = g_SwapChainExtent;

            vkCmdBeginRenderPass(currentContext.commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(draw_data, currentContext.commandBuffer);
            vkCmdEndRenderPass(currentContext.commandBuffer);
        }

        if (vkEndCommandBuffer(currentContext.commandBuffer) != VK_SUCCESS) {
            LOGD("Failed to end command buffer from current context");
            return result;
        }

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &currentContext.commandBuffer;

        if (vkQueueSubmit(queue, 1, &submitInfo, currentContext.fence) != VK_SUCCESS) {
            LOGD("Failed to submit queue for ImGui rendering");
            //return result;
        }

        currentContext.inUse = true;
        g_CurrentContext = (g_CurrentContext + 1) % 2;

    } catch (const std::exception& e) {
        LOGD("ImGui rendering exception: %s", e.what());
    } catch (...) {
        LOGD("Unknown exception in ImGui rendering");
    }

    g_InSubmit = false;
    return result;
}

VkResult (*vkCreateInstanceOrigin)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VkResult vkCreateInstanceReplace(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    VkResult result = vkCreateInstanceOrigin(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS) {
        g_Instance = *pInstance;
        LOGD("[CALLED] vkCreateInstance");
    }
    return result;
}

VkResult (*vkCreateDeviceOrigin)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VkResult vkCreateDeviceReplace(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    VkResult result = vkCreateDeviceOrigin(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS) {
        g_PhysicalDevice = physicalDevice;
        g_Device = *pDevice;
        
        // Get the graphics queue after device creation and create command pool
        uint32_t queueFamilyIndex = findGraphicsQueueFamily();
        vkGetDeviceQueue(g_Device, queueFamilyIndex, 0, &g_Queue);
        
        g_CommandPool = createCommandPool();
        if (g_CommandPool == VK_NULL_HANDLE) {
            LOGD("Failed to create command pool");
        }
        
        LOGD("Device and command pool created successfully");
    }
    return result;
}

VkResult (*vkCreateAndroidSurfaceKHROrigin)(VkInstance, const VkAndroidSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
VkResult vkCreateAndroidSurfaceKHRReplace(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    if (pCreateInfo && g_NativeWindow == nullptr) {
        g_NativeWindow = pCreateInfo->window;
        LOGD("[CALLED] Captured Android native window: %p", g_NativeWindow);
    }
    return vkCreateAndroidSurfaceKHROrigin(instance, pCreateInfo, pAllocator, pSurface);
}

VkResult (*vkCreateSwapchainKHROrigin)(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
VkResult vkCreateSwapchainKHRReplace(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    VkResult result = vkCreateSwapchainKHROrigin(device, pCreateInfo, pAllocator, pSwapchain);
    if (result == VK_SUCCESS) {
        g_Swapchain = *pSwapchain;
        g_SwapChainExtent = pCreateInfo->imageExtent;
        g_Surface = pCreateInfo->surface;

        uint32_t imageCount;
        vkGetSwapchainImagesKHR(device, g_Swapchain, &imageCount, nullptr);
        g_SwapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, g_Swapchain, &imageCount, g_SwapChainImages.data());
        
        LOGD("Swapchain created successfully: %dx%d", g_SwapChainExtent.width, g_SwapChainExtent.height);

        if (
            !g_ImGuiInitialized && 
            !g_InitInProgress && 
            g_Device != VK_NULL_HANDLE && 
            g_Queue != VK_NULL_HANDLE &&
            g_NativeWindow != nullptr
        ) {
            g_InitInProgress = true;
            
            if (initializeImGui()) {
                initRenderContexts();
                LOGD("Render contexts initialized successfully");

                g_ImGuiInitialized = true;
                LOGD("ImGui initialized successfully");
            } else {
                LOGD("Failed to initialize ImGui");
            }
            
            g_InitInProgress = false;
        }
    }
    return result;
}

// Hook via unity touch events (1st method)
extern "C" {
    JNIEXPORT void JNICALL Java_com_unity3d_player_UnityPlayer_nativeOnTouchEvent(JNIEnv*, jobject, jint action, jfloat x, jfloat y) {
        if (g_ImGuiInitialized) {
            ImGuiIO& io = ImGui::GetIO();

            LOGD("[CALLED] nativeOnTouchEvent | Touch event: %d, %.2f, %.2f", action, x, y);
            
            switch (action) {
                case 0: // ACTION_DOWN
                    io.MouseDown[0] = true;
                    g_MenuVisible = true; // Afficher le menu au toucher
                    break;
                case 1: // ACTION_UP
                    io.MouseDown[0] = false;
                    break;
                case 2: // ACTION_MOVE
                    io.MousePos = ImVec2(x, y);
                    break;
            }
        }
    }
}

// Hook directly "libinput.so" to handle touch events (2nd method)
// initializeMotionEvent(MotionEvent* event, const InputMessage* msg)
void (*initializeMotionEventOrigin)(void* thiz, void* event, void* msg);
void initializeMotionEventReplace(void *thiz, void *event, void *msg) {
    initializeMotionEventOrigin(thiz, event, msg);

    ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    LOGD("[CALLED] initializeMotionEvent");
}

bool isLibraryLoaded(const char *libraryName) {

    char line[512] = {0};
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, libraryName)) {
                return true;
            }
        }
        fclose(fp);
    }
    return false;
}

void initializeHooks() {
    do {
        usleep(100);
    } while (!isLibraryLoaded("libvulkan.so"));

    void* vkCreateInstanceAddr = DobbySymbolResolver("libvulkan.so", "vkCreateInstance");
    DobbyHook(vkCreateInstanceAddr, (void*)vkCreateInstanceReplace, (void**)&vkCreateInstanceOrigin);

    void* vkCreateDeviceAddr = DobbySymbolResolver("libvulkan.so", "vkCreateDevice");
    DobbyHook(vkCreateDeviceAddr, (void*)vkCreateDeviceReplace, (void**)&vkCreateDeviceOrigin);

    void* vkCreateAndroidSurfaceKHRAddr = DobbySymbolResolver("libvulkan.so", "vkCreateAndroidSurfaceKHR");
    DobbyHook(vkCreateAndroidSurfaceKHRAddr, (void*)vkCreateAndroidSurfaceKHRReplace, (void**)&vkCreateAndroidSurfaceKHROrigin);

    void* vkCreateSwapchainKHRAddr = DobbySymbolResolver("libvulkan.so", "vkCreateSwapchainKHR");
    DobbyHook(vkCreateSwapchainKHRAddr, (void*)vkCreateSwapchainKHRReplace, (void**)&vkCreateSwapchainKHROrigin);

    void* vkQueueSubmitAddr = DobbySymbolResolver("libvulkan.so", "vkQueueSubmit");
    DobbyHook(vkQueueSubmitAddr, (void*)hooked_vkQueueSubmit, (void**)&original_vkQueueSubmit);

    auto initializeMotionEventAddr = DobbySymbolResolver("libinput.so", "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE");
    DobbyHook((void *)initializeMotionEventAddr, (void *)initializeMotionEventReplace, (void **)&initializeMotionEventOrigin);

    LOGD("Vulkan hooks initialized successfully");
}

void* menuThread(void*) {
    initializeHooks();
    return NULL;
}

// Entry point
__attribute__((constructor))
void init() {
    LOGD("Library loaded successfully");
    
    pthread_t pthread;
    pthread_create(&pthread, NULL, menuThread, NULL);
}