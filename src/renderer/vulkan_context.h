#pragma once
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>

namespace oh {

struct FrameData {
    VkCommandBuffer cmd;
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence in_flight;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};

class VulkanContext {
public:
    bool init(SDL_Window* window);
    void cleanup();

    bool begin_frame(uint32_t& image_index);
    void end_frame(uint32_t image_index);
    void recreate_swapchain();
    // Writes the next presented frame to `path` as binary PPM (works off-screen/locked).
    // In translucent mode it writes a PAM (RGBA, straight alpha) next to it instead.
    void request_capture(const std::string& path) { capture_path_ = path; }

    // Per-pixel window translucency. Needs a window created with SDL_WINDOW_TRANSPARENT,
    // a surface that composites alpha (PRE_ or POST_MULTIPLIED) and dualSrcBlend.
    // Frames are then drawn into an offscreen image whose alpha channel is the window
    // mask, and resolved to the swapchain (see shaders/resolve.frag.glsl).
    // Returns whether translucency is active afterwards; recreates the swapchain on change.
    bool set_translucent(bool on);
    bool translucent() const { return translucent_; }
    bool dual_source_blend() const { return dual_src_blend_; }
    // Render pass the frame's 2D pipeline must be compatible with in translucent mode.
    VkRenderPass offscreen_render_pass() const { return offscreen_pass_; }

    // Resource creation helpers
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
    void destroy_buffer(Buffer& buf);
    void upload_buffer(Buffer& dst, const void* data, VkDeviceSize size);

    Image create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage);
    void destroy_image(Image& img);
    void upload_image(Image& img, const void* data, uint32_t w, uint32_t h, VkFormat format);
    // Copies a sub-rectangle of an R8 pixel array into an image already in SHADER_READ layout.
    void update_image_region(Image& img, const uint8_t* pixels, uint32_t row_pitch,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    VkSampler create_sampler(VkFilter filter = VK_FILTER_LINEAR);
    VkShaderModule load_shader(const uint32_t* code, size_t bytes);
    VkPipeline create_graphics_pipeline(
        VkShaderModule vert, VkShaderModule frag,
        VkPipelineLayout layout,
        const std::vector<VkVertexInputBindingDescription>& bindings,
        const std::vector<VkVertexInputAttributeDescription>& attributes,
        bool alpha_blend = true,
        VkRenderPass pass = VK_NULL_HANDLE,  // default: the swapchain render pass
        bool dual_source = false             // colour/alpha weighted by output index 1's alpha
    );

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    VkCommandBuffer begin_single_command();
    void end_single_command(VkCommandBuffer cmd);

    // Accessors
    VkDevice device() const { return device_; }
    VkRenderPass render_pass() const { return render_pass_; }
    VkExtent2D extent() const { return swapchain_extent_; }
    uint32_t frame_index() const { return current_frame_; }
    FrameData& current_frame_data() { return frames_[current_frame_]; }
    VkDescriptorPool descriptor_pool() const { return descriptor_pool_; }

    static constexpr uint32_t MAX_FRAMES = 2;

private:
    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = 0;
    uint32_t present_family_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_;
    VkExtent2D swapchain_extent_;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> present_sems_;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    FrameData frames_[MAX_FRAMES];
    uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
    std::string capture_path_;
    void write_capture(const Buffer& buf) const;

    // Translucent window
    bool dual_src_blend_ = false;
    bool want_translucent_ = false;
    bool translucent_ = false;
    VkCompositeAlphaFlagBitsKHR composite_alpha_ = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    static constexpr VkFormat OFFSCREEN_FORMAT = VK_FORMAT_R8G8B8A8_SRGB;
    VkRenderPass offscreen_pass_ = VK_NULL_HANDLE;
    Image offscreen_{};
    VkFramebuffer offscreen_fb_ = VK_NULL_HANDLE;
    VkSampler resolve_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolve_dsl_ = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set_ = VK_NULL_HANDLE;
    VkPipelineLayout resolve_layout_ = VK_NULL_HANDLE;
    VkPipeline resolve_pipeline_ = VK_NULL_HANDLE;
    bool create_translucency_objects();
    bool create_offscreen_target();
    void destroy_offscreen_target();

    bool create_instance();
    bool create_surface();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain();
    bool create_render_pass();
    bool create_framebuffers();
    bool create_command_pool();
    bool create_descriptor_pool();
    bool create_sync_objects();
    void cleanup_swapchain();
};

} // namespace oh
