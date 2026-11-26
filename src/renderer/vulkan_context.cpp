#include "renderer/vulkan_context.h"
#include "shaders/spirv.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <vector>
#include <set>

namespace oh {

// ---------- Debug messenger callback ----------
#ifdef OVERHEAT_DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}
#endif

// ---------- Public interface ----------

bool VulkanContext::init(SDL_Window* window) {
    window_ = window;
    if (!create_instance())       return false;
    if (!create_surface())        return false;
    if (!pick_physical_device())  return false;
    if (!create_logical_device()) return false;
    if (!create_swapchain())      return false;
    if (!create_render_pass())    return false;
    if (!create_framebuffers())   return false;
    if (!create_command_pool())   return false;
    if (!create_descriptor_pool())return false;
    if (!create_sync_objects())   return false;
    if (!create_translucency_objects()) {
        fprintf(stderr, "Translucent window support unavailable\n");
        dual_src_blend_ = false;
    }
    return true;
}

void VulkanContext::cleanup() {
    if (device_) {
        vkDeviceWaitIdle(device_);

        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            if (frames_[i].image_available) vkDestroySemaphore(device_, frames_[i].image_available, nullptr);
            if (frames_[i].render_finished) vkDestroySemaphore(device_, frames_[i].render_finished, nullptr);
            if (frames_[i].in_flight)       vkDestroyFence(device_, frames_[i].in_flight, nullptr);
        }

        if (resolve_pipeline_) vkDestroyPipeline(device_, resolve_pipeline_, nullptr);
        if (resolve_layout_)   vkDestroyPipelineLayout(device_, resolve_layout_, nullptr);
        if (resolve_dsl_)      vkDestroyDescriptorSetLayout(device_, resolve_dsl_, nullptr);
        if (resolve_sampler_)  vkDestroySampler(device_, resolve_sampler_, nullptr);
        if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (command_pool_)    vkDestroyCommandPool(device_, command_pool_, nullptr);
        cleanup_swapchain();
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        if (offscreen_pass_) vkDestroyRenderPass(device_, offscreen_pass_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_)     vkDestroySurfaceKHR(instance_, surface_, nullptr);

#ifdef OVERHEAT_DEBUG
    if (debug_messenger_) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance_, debug_messenger_, nullptr);
    }
#endif

    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanContext::begin_frame(uint32_t& image_index) {
    auto& frame = frames_[current_frame_];
    vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
        frame.image_available, VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    vkResetFences(device_, 1, &frame.in_flight);
    vkResetCommandBuffer(frame.cmd, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmd, &begin_info);

    VkRenderPassBeginInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass = translucent_ ? offscreen_pass_ : render_pass_;
    rp_info.framebuffer = translucent_ ? offscreen_fb_ : framebuffers_[image_index];
    rp_info.renderArea.extent = swapchain_extent_;
    VkClearValue clear{};
    // Translucent: rgb is overwritten by the full-window background, alpha starts empty.
    clear.color = {{0.0f, 0.0f, 0.0f, translucent_ ? 0.0f : 1.0f}};
    rp_info.clearValueCount = 1;
    rp_info.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width  = static_cast<float>(swapchain_extent_.width);
    viewport.height = static_cast<float>(swapchain_extent_.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = swapchain_extent_;
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    return true;
}

void VulkanContext::end_frame(uint32_t image_index) {
    auto& frame = frames_[current_frame_];
    vkCmdEndRenderPass(frame.cmd);

    if (translucent_) {
        // Resolve the offscreen frame (straight colour + window alpha) into the swapchain.
        VkRenderPassBeginInfo rp_info{};
        rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass = render_pass_;
        rp_info.framebuffer = framebuffers_[image_index];
        rp_info.renderArea.extent = swapchain_extent_;
        VkClearValue clear{};
        rp_info.clearValueCount = 1;
        rp_info.pClearValues = &clear;
        vkCmdBeginRenderPass(frame.cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout_, 0, 1,
                                &resolve_set_, 0, nullptr);
        uint32_t pc[2] = {
            composite_alpha_ == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR ? 1u : 0u,
            (swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB ||
             swapchain_format_ == VK_FORMAT_A8B8G8R8_SRGB_PACK32) ? 1u : 0u,
        };
        vkCmdPushConstants(frame.cmd, resolve_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.cmd);
    }

    Buffer capture{};
    if (!capture_path_.empty()) {
        capture = create_buffer(VkDeviceSize(swapchain_extent_.width) * swapchain_extent_.height * 4,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = swapchain_images_[image_index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {swapchain_extent_.width, swapchain_extent_.height, 1};
        vkCmdCopyImageToBuffer(frame.cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture.buffer, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(frame.cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_sems[]   = {frame.image_available};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores    = wait_sems;
    submit.pWaitDstStageMask  = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &frame.cmd;
    VkSemaphore signal_sems[] = {present_sems_[image_index]};
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = signal_sems;
    vkQueueSubmit(graphics_queue_, 1, &submit, frame.in_flight);

    if (capture.buffer) {
        vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
        write_capture(capture);
        destroy_buffer(capture);
        capture_path_.clear();
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = signal_sems;
    present.swapchainCount  = 1;
    present.pSwapchains     = &swapchain_;
    present.pImageIndices   = &image_index;

    VkResult result = vkQueuePresentKHR(present_queue_, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebuffer_resized_) {
        framebuffer_resized_ = false;
        recreate_swapchain();
    }

    current_frame_ = (current_frame_ + 1) % MAX_FRAMES;
}

void VulkanContext::write_capture(const Buffer& buf) const {
    // Swapchain is B8G8R8A8 (sRGB-encoded); PPM wants RGB bytes. A translucent frame
    // keeps its alpha: PAM (RGBA, straight alpha, so it converts to a normal PNG).
    std::string path = capture_path_;
    bool alpha = translucent_;
    if (alpha && path.size() > 4 && path.compare(path.size() - 4, 4, ".ppm") == 0)
        path.replace(path.size() - 4, 4, ".pam");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "capture: cannot write %s\n", path.c_str());
        return;
    }
    uint32_t w = swapchain_extent_.width, h = swapchain_extent_.height;
    size_t ch = alpha ? 4 : 3;
    if (alpha) std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
    else       std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    const auto* px = static_cast<const uint8_t*>(buf.mapped);
    std::vector<uint8_t> row(size_t(w) * ch);
    bool bgr = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM;
    bool premul = composite_alpha_ == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    std::vector<uint8_t> src(size_t(w) * 4);  // mapped memory may be uncached: read each row once
    for (uint32_t y = 0; y < h; y++) {
        std::memcpy(src.data(), px + size_t(y) * w * 4, src.size());
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = &src[size_t(x) * 4];
            uint8_t* o = &row[x * ch];
            o[0] = bgr ? p[2] : p[0];
            o[1] = p[1];
            o[2] = bgr ? p[0] : p[2];
            if (!alpha) continue;
            o[3] = p[3];
            if (premul && p[3] > 0 && p[3] < 255)
                for (int k = 0; k < 3; k++)
                    o[k] = static_cast<uint8_t>(std::min(255, (o[k] * 255 + p[3] / 2) / p[3]));
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

void VulkanContext::recreate_swapchain() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    while (w == 0 || h == 0) {
        SDL_WaitEvent(nullptr);
        SDL_GetWindowSize(window_, &w, &h);
    }
    vkDeviceWaitIdle(device_);
    cleanup_swapchain();
    create_swapchain();
    create_framebuffers();
}

bool VulkanContext::set_translucent(bool on) {
    want_translucent_ = on;
    if (on == translucent_) return translucent_;
    if (on) {
        if (!dual_src_blend_ || !resolve_pipeline_ || !(SDL_GetWindowFlags(window_) & SDL_WINDOW_TRANSPARENT))
            return false;
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
        if (!(caps.supportedCompositeAlpha & (VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR |
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)))
            return false;
    }
    recreate_swapchain();
    fprintf(stderr, "Swapchain composite alpha: %s\n",
            composite_alpha_ == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR  ? "pre-multiplied" :
            composite_alpha_ == VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR ? "post-multiplied" : "opaque");
    return translucent_;
}

bool VulkanContext::create_translucency_objects() {
    if (!dual_src_blend_) return true;  // nothing to build; translucency stays off

    // Offscreen pass: same shape as the swapchain pass, ends ready for sampling.
    VkAttachmentDescription att{};
    att.format         = OFFSCREEN_FORMAT;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;
    VkSubpassDependency deps[2]{};
    // The previous frame's resolve must finish reading before this frame writes.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &att;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies   = deps;
    if (vkCreateRenderPass(device_, &rp, nullptr, &offscreen_pass_) != VK_SUCCESS) return false;

    resolve_sampler_ = create_sampler(VK_FILTER_NEAREST);
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1;
    dl.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(device_, &dl, nullptr, &resolve_dsl_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &resolve_dsl_;
    if (vkAllocateDescriptorSets(device_, &ai, &resolve_set_) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &resolve_dsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &resolve_layout_) != VK_SUCCESS) return false;

    VkShaderModule vert = load_shader(spirv_resolve_vert, sizeof spirv_resolve_vert);
    VkShaderModule frag = load_shader(spirv_resolve_frag, sizeof spirv_resolve_frag);
    if (vert && frag) resolve_pipeline_ = create_graphics_pipeline(vert, frag, resolve_layout_, {}, {}, false);
    if (vert) vkDestroyShaderModule(device_, vert, nullptr);
    if (frag) vkDestroyShaderModule(device_, frag, nullptr);
    return resolve_pipeline_ != VK_NULL_HANDLE;
}

bool VulkanContext::create_offscreen_target() {
    offscreen_ = create_image(swapchain_extent_.width, swapchain_extent_.height, OFFSCREEN_FORMAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkFramebufferCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass      = offscreen_pass_;
    ci.attachmentCount = 1;
    ci.pAttachments    = &offscreen_.view;
    ci.width           = swapchain_extent_.width;
    ci.height          = swapchain_extent_.height;
    ci.layers          = 1;
    if (vkCreateFramebuffer(device_, &ci, nullptr, &offscreen_fb_) != VK_SUCCESS) return false;

    VkDescriptorImageInfo img{resolve_sampler_, offscreen_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = resolve_set_;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &img;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return true;
}

void VulkanContext::destroy_offscreen_target() {
    if (offscreen_fb_) vkDestroyFramebuffer(device_, offscreen_fb_, nullptr);
    offscreen_fb_ = VK_NULL_HANDLE;
    destroy_image(offscreen_);
}

// ---------- Resource helpers ----------

Buffer VulkanContext::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    Buffer buf{};
    buf.size = size;

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size  = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device_, &ci, nullptr, &buf.buffer);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf.buffer, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
    vkAllocateMemory(device_, &ai, nullptr, &buf.memory);
    vkBindBufferMemory(device_, buf.buffer, buf.memory, 0);

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vkMapMemory(device_, buf.memory, 0, size, 0, &buf.mapped);
    }

    return buf;
}

void VulkanContext::destroy_buffer(Buffer& buf) {
    if (buf.mapped) {
        vkUnmapMemory(device_, buf.memory);
        buf.mapped = nullptr;
    }
    if (buf.buffer) vkDestroyBuffer(device_, buf.buffer, nullptr);
    if (buf.memory) vkFreeMemory(device_, buf.memory, nullptr);
    buf.buffer = VK_NULL_HANDLE;
    buf.memory = VK_NULL_HANDLE;
}

void VulkanContext::upload_buffer(Buffer& dst, const void* data, VkDeviceSize size) {
    if (dst.mapped) {
        memcpy(dst.mapped, data, static_cast<size_t>(size));
    }
}

Image VulkanContext::create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    Image img{};
    img.width  = w;
    img.height = h;

    VkImageCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = format;
    ci.extent        = {w, h, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device_, &ci, nullptr, &img.image);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &ai, nullptr, &img.memory);
    vkBindImageMemory(device_, img.image, img.memory, 0);

    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &vci, nullptr, &img.view);

    return img;
}

void VulkanContext::destroy_image(Image& img) {
    if (img.view)   vkDestroyImageView(device_, img.view, nullptr);
    if (img.image)  vkDestroyImage(device_, img.image, nullptr);
    if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    img = {};
}

void VulkanContext::upload_image(Image& img, const void* data, uint32_t w, uint32_t h, VkFormat format) {
    VkDeviceSize pixel_size = 1;
    if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_R8G8B8A8_SRGB  || format == VK_FORMAT_B8G8R8A8_SRGB)
        pixel_size = 4;
    VkDeviceSize image_size = w * h * pixel_size;

    Buffer staging = create_buffer(image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(staging.mapped, data, static_cast<size_t>(image_size));

    VkCommandBuffer cmd = begin_single_command();

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    end_single_command(cmd);
    destroy_buffer(staging);
}

void VulkanContext::update_image_region(Image& img, const uint8_t* pixels, uint32_t row_pitch,
                                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    Buffer staging = create_buffer(VkDeviceSize(w) * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto* dst = static_cast<uint8_t*>(staging.mapped);
    for (uint32_t row = 0; row < h; row++)
        memcpy(dst + size_t(row) * w, pixels + size_t(y + row) * row_pitch + x, w);

    // Frames in flight may still sample the image; this path is rare (new glyphs only).
    vkDeviceWaitIdle(device_);
    VkCommandBuffer cmd = begin_single_command();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    end_single_command(cmd);
    destroy_buffer(staging);
}

VkSampler VulkanContext::create_sampler(VkFilter filter) {
    VkSamplerCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter    = filter;
    ci.minFilter    = filter;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod       = 1.0f;
    VkSampler sampler;
    vkCreateSampler(device_, &ci, nullptr, &sampler);
    return sampler;
}

VkShaderModule VulkanContext::load_shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode    = code;
    VkShaderModule mod;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create shader module\n");
        return VK_NULL_HANDLE;
    }
    return mod;
}

VkPipeline VulkanContext::create_graphics_pipeline(
    VkShaderModule vert, VkShaderModule frag,
    VkPipelineLayout layout,
    const std::vector<VkVertexInputBindingDescription>& bindings,
    const std::vector<VkVertexInputAttributeDescription>& attributes,
    bool alpha_blend,
    VkRenderPass pass,
    bool dual_source)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = static_cast<uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions      = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions    = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth   = 1.0f;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (alpha_blend) {
        VkBlendFactor inv = dual_source ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
                                        : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_att.blendEnable         = VK_TRUE;
        blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;  // premultiplied
        blend_att.dstColorBlendFactor = inv;
        blend_att.colorBlendOp        = VK_BLEND_OP_ADD;
        blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_att.dstAlphaBlendFactor = inv;
        blend_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vertex_input;
    ci.pInputAssemblyState = &input_asm;
    ci.pViewportState      = &viewport_state;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState   = &ms;
    ci.pColorBlendState    = &blend;
    ci.pDynamicState       = &dynamic;
    ci.layout              = layout;
    ci.renderPass          = pass ? pass : render_pass_;
    ci.subpass             = 0;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipeline\n");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

uint32_t VulkanContext::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    fprintf(stderr, "Failed to find suitable memory type\n");
    return 0;
}

VkCommandBuffer VulkanContext::begin_single_command() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = command_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void VulkanContext::end_single_command(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

// ---------- Private init steps ----------

bool VulkanContext::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "Overheat";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName        = "Overheat Engine";
    app_info.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion         = VK_API_VERSION_1_2;

    // Get SDL required extensions
    uint32_t sdl_ext_count = 0;
    char const* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    std::vector<const char*> extensions(sdl_exts, sdl_exts + sdl_ext_count);

    std::vector<const char*> layers;

#ifdef OVERHEAT_DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    {
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> available(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, available.data());
        for (auto& l : available) {
            if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                break;
            }
        }
    }
#endif

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app_info;
    ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        return false;
    }

#ifdef OVERHEAT_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT dbg{};
    dbg.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbg.messageSeverity  = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType      = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback  = debug_callback;
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
    if (fn) fn(instance_, &dbg, nullptr, &debug_messenger_);
#endif

    return true;
}

bool VulkanContext::create_surface() {
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        fprintf(stderr, "Failed to create Vulkan surface\n");
        return false;
    }
    return true;
}

bool VulkanContext::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "No Vulkan physical devices found\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Score each device, prefer discrete GPU
    int best_score = -1;
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        // Check queue families
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf_props(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, qf_props.data());

        bool has_graphics = false, has_present = false;
        uint32_t gf = 0, pf = 0;
        for (uint32_t i = 0; i < qf_count; i++) {
            if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gf = i;
                has_graphics = true;
            }
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present_support);
            if (present_support) {
                pf = i;
                has_present = true;
            }
        }
        if (!has_graphics || !has_present) continue;

        // Check swapchain extension support
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, exts.data());
        bool has_swapchain = false;
        for (auto& e : exts) {
            if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swapchain = true;
                break;
            }
        }
        if (!has_swapchain) continue;

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
        score += static_cast<int>(props.limits.maxImageDimension2D / 1000);

        if (score > best_score) {
            best_score = score;
            physical_device_ = dev;
            graphics_family_ = gf;
            present_family_  = pf;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        fprintf(stderr, "No suitable GPU found\n");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    fprintf(stderr, "Selected GPU: %s\n", props.deviceName);
    return true;
}

bool VulkanContext::create_logical_device() {
    std::set<uint32_t> unique_families = {graphics_family_, present_family_};
    std::vector<VkDeviceQueueCreateInfo> queue_cis;
    float priority = 1.0f;
    for (uint32_t fam : unique_families) {
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = fam;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &priority;
        queue_cis.push_back(qci);
    }

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_device_, &supported);
    VkPhysicalDeviceFeatures features{};
    // Translucent windows keep a separate alpha channel via dual-source blending.
    features.dualSrcBlend = supported.dualSrcBlend;
    dual_src_blend_ = supported.dualSrcBlend == VK_TRUE;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queue_cis.size());
    ci.pQueueCreateInfos       = queue_cis.data();
    ci.pEnabledFeatures        = &features;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = dev_exts;

    if (vkCreateDevice(physical_device_, &ci, nullptr, &device_) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        return false;
    }
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
    return true;
}

bool VulkanContext::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    uint32_t fmt_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    uint32_t pm_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, modes.data());

    // Choose surface format: prefer B8G8R8A8_SRGB
    VkSurfaceFormatKHR chosen_format = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = f;
            break;
        }
    }
    swapchain_format_ = chosen_format.format;

    // Choose present mode: prefer MAILBOX, fallback to FIFO
    VkPresentModeKHR chosen_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosen_mode = m;
            break;
        }
    }

    // Choose extent
    if (caps.currentExtent.width != UINT32_MAX) {
        swapchain_extent_ = caps.currentExtent;
    } else {
        int w, h;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        swapchain_extent_.width  = std::clamp(static_cast<uint32_t>(w),
            caps.minImageExtent.width, caps.maxImageExtent.width);
        swapchain_extent_.height = std::clamp(static_cast<uint32_t>(h),
            caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface_;
    ci.minImageCount    = image_count;
    ci.imageFormat      = chosen_format.format;
    ci.imageColorSpace  = chosen_format.colorSpace;
    ci.imageExtent      = swapchain_extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // in-app frame capture

    uint32_t families[] = {graphics_family_, present_family_};
    if (graphics_family_ != present_family_) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    // Translucent windows need a surface that composites alpha (Wayland, X11 with an
    // ARGB visual, some Windows drivers). Everything else stays opaque.
    composite_alpha_ = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (want_translucent_ && dual_src_blend_ && resolve_pipeline_ &&
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_TRANSPARENT)) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            composite_alpha_ = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            composite_alpha_ = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }
    translucent_ = composite_alpha_ != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    ci.preTransform   = caps.currentTransform;
    ci.compositeAlpha = composite_alpha_;
    ci.presentMode    = chosen_mode;
    ci.clipped        = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swapchain\n");
        return false;
    }

    uint32_t sc_count;
    vkGetSwapchainImagesKHR(device_, swapchain_, &sc_count, nullptr);
    swapchain_images_.resize(sc_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &sc_count, swapchain_images_.data());

    // One present semaphore per image: a semaphore handed to present can only be
    // reused once that same image is re-acquired.
    present_sems_.resize(sc_count);
    for (auto& sem : present_sems_) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &sci, nullptr, &sem);
    }

    swapchain_views_.resize(sc_count);
    for (uint32_t i = 0; i < sc_count; i++) {
        VkImageViewCreateInfo vci{};
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = swapchain_images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = swapchain_format_;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &vci, nullptr, &swapchain_views_[i]);
    }

    return true;
}

bool VulkanContext::create_render_pass() {
    VkAttachmentDescription color_att{};
    color_att.format         = swapchain_format_;
    color_att.samples        = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments    = &color_att;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    if (vkCreateRenderPass(device_, &ci, nullptr, &render_pass_) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create render pass\n");
        return false;
    }
    return true;
}

bool VulkanContext::create_framebuffers() {
    if (translucent_ && !create_offscreen_target()) {
        fprintf(stderr, "Failed to create offscreen target\n");
        return false;
    }
    framebuffers_.resize(swapchain_views_.size());
    for (size_t i = 0; i < swapchain_views_.size(); i++) {
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = render_pass_;
        ci.attachmentCount = 1;
        ci.pAttachments    = &swapchain_views_[i];
        ci.width           = swapchain_extent_.width;
        ci.height          = swapchain_extent_.height;
        ci.layers          = 1;
        if (vkCreateFramebuffer(device_, &ci, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create framebuffer\n");
            return false;
        }
    }
    return true;
}

bool VulkanContext::create_command_pool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = graphics_family_;
    if (vkCreateCommandPool(device_, &ci, nullptr, &command_pool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = command_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES;
    VkCommandBuffer cmds[MAX_FRAMES];
    vkAllocateCommandBuffers(device_, &ai, cmds);
    for (uint32_t i = 0; i < MAX_FRAMES; i++)
        frames_[i].cmd = cmds[i];

    return true;
}

bool VulkanContext::create_descriptor_pool() {
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000},
    };

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 1000;
    ci.poolSizeCount = 3;
    ci.pPoolSizes    = pool_sizes;
    if (vkCreateDescriptorPool(device_, &ci, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create descriptor pool\n");
        return false;
    }
    return true;
}

bool VulkanContext::create_sync_objects() {
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (vkCreateSemaphore(device_, &sci, nullptr, &frames_[i].image_available) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sci, nullptr, &frames_[i].render_finished) != VK_SUCCESS ||
            vkCreateFence(device_, &fci, nullptr, &frames_[i].in_flight) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create sync objects\n");
            return false;
        }
    }
    return true;
}

void VulkanContext::cleanup_swapchain() {
    destroy_offscreen_target();
    for (auto sem : present_sems_) vkDestroySemaphore(device_, sem, nullptr);
    present_sems_.clear();
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    for (auto iv : swapchain_views_) vkDestroyImageView(device_, iv, nullptr);
    swapchain_views_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

} // namespace oh
