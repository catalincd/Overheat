#include "renderer/ui_renderer.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace oh {

struct RectUBO {
    glm::vec2 screen_size;
    glm::vec2 scroll_offset;
    float time;
    float _pad[3]; // pad to 32 bytes (std140 alignment)
};

bool UIRenderer::init(VulkanContext& vk) {
    // --- Descriptor set layout: binding 0 = uniform buffer only ---
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(vk.device(), &layout_ci, nullptr, &desc_layout_) != VK_SUCCESS) {
        fprintf(stderr, "UIRenderer: failed to create descriptor set layout\n");
        return false;
    }

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts    = &desc_layout_;
    if (vkCreatePipelineLayout(vk.device(), &pl_ci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        fprintf(stderr, "UIRenderer: failed to create pipeline layout\n");
        return false;
    }

    // --- Shaders ---
    std::string shader_dir = SHADER_DIR;
    VkShaderModule vert = vk.load_shader(shader_dir + "/rect.vert.spv");
    VkShaderModule frag = vk.load_shader(shader_dir + "/rect.frag.spv");
    if (!vert || !frag) {
        fprintf(stderr, "UIRenderer: failed to load shaders\n");
        return false;
    }

    // --- Vertex input: instance buffer only ---
    VkVertexInputBindingDescription binding_desc{};
    binding_desc.binding   = 0;
    binding_desc.stride    = sizeof(RectInstance);
    binding_desc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrs[6]{};
    // location 0: pos (vec2)
    attrs[0].binding  = 0;
    attrs[0].location = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(RectInstance, pos);
    // location 1: size (vec2)
    attrs[1].binding  = 0;
    attrs[1].location = 1;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(RectInstance, size);
    // location 2: color (vec4)
    attrs[2].binding  = 0;
    attrs[2].location = 2;
    attrs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset   = offsetof(RectInstance, color);
    // location 3: corner_radius (float)
    attrs[3].binding  = 0;
    attrs[3].location = 3;
    attrs[3].format   = VK_FORMAT_R32_SFLOAT;
    attrs[3].offset   = offsetof(RectInstance, corner_radius);
    // location 4: border_width (float)
    attrs[4].binding  = 0;
    attrs[4].location = 4;
    attrs[4].format   = VK_FORMAT_R32_SFLOAT;
    attrs[4].offset   = offsetof(RectInstance, border_width);
    // location 5: border_color (vec4)
    attrs[5].binding  = 0;
    attrs[5].location = 5;
    attrs[5].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[5].offset   = offsetof(RectInstance, border_color);

    pipeline_ = vk.create_graphics_pipeline(
        vert, frag, pipeline_layout_,
        {binding_desc},
        {attrs[0], attrs[1], attrs[2], attrs[3], attrs[4], attrs[5]},
        true // alpha blending
    );

    vkDestroyShaderModule(vk.device(), vert, nullptr);
    vkDestroyShaderModule(vk.device(), frag, nullptr);

    if (!pipeline_) {
        fprintf(stderr, "UIRenderer: failed to create pipeline\n");
        return false;
    }

    // --- Per-frame resources ---
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        instance_buffers_[i] = vk.create_buffer(
            sizeof(RectInstance) * MAX_INSTANCES,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        uniform_buffers_[i] = vk.create_buffer(
            sizeof(RectUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
    }

    // --- Descriptor sets ---
    VkDescriptorSetLayout layouts[VulkanContext::MAX_FRAMES];
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++)
        layouts[i] = desc_layout_;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = vk.descriptor_pool();
    alloc_info.descriptorSetCount = VulkanContext::MAX_FRAMES;
    alloc_info.pSetLayouts        = layouts;
    vkAllocateDescriptorSets(vk.device(), &alloc_info, desc_sets_);

    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        VkDescriptorBufferInfo buf_info{};
        buf_info.buffer = uniform_buffers_[i].buffer;
        buf_info.offset = 0;
        buf_info.range  = sizeof(RectUBO);

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = desc_sets_[i];
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo     = &buf_info;

        vkUpdateDescriptorSets(vk.device(), 1, &write, 0, nullptr);
    }

    instances_.reserve(MAX_INSTANCES);
    return true;
}

void UIRenderer::cleanup(VulkanContext& vk) {
    vkDeviceWaitIdle(vk.device());
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        vk.destroy_buffer(instance_buffers_[i]);
        vk.destroy_buffer(uniform_buffers_[i]);
    }
    if (pipeline_)        vkDestroyPipeline(vk.device(), pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(vk.device(), pipeline_layout_, nullptr);
    if (desc_layout_)     vkDestroyDescriptorSetLayout(vk.device(), desc_layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    desc_layout_ = VK_NULL_HANDLE;
}

void UIRenderer::begin_batch() {
    instances_.clear();
}

void UIRenderer::add_rect(Rect rect, Color color, float corner_radius,
                           float border_width, Color border_color) {
    if (instances_.size() >= MAX_INSTANCES) return;

    RectInstance inst;
    inst.pos          = {rect.x, rect.y};
    inst.size         = {rect.w, rect.h};
    inst.color        = color.vec4();
    inst.corner_radius = corner_radius;
    inst.border_width  = border_width;
    inst.border_color  = border_color.vec4();
    instances_.push_back(inst);
}

void UIRenderer::flush(VulkanContext& vk, VkCommandBuffer cmd, VkExtent2D extent) {
    if (instances_.empty()) return;

    uint32_t fi = vk.frame_index();

    // Update uniform buffer
    RectUBO ubo{};
    ubo.screen_size   = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    ubo.scroll_offset = {0.0f, 0.0f};
    ubo.time          = 0.0f;
    memcpy(uniform_buffers_[fi].mapped, &ubo, sizeof(RectUBO));

    // Upload instances
    size_t copy_size = instances_.size() * sizeof(RectInstance);
    memcpy(instance_buffers_[fi].mapped, instances_.data(), copy_size);

    // Bind pipeline and descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout_, 0, 1, &desc_sets_[fi], 0, nullptr);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instance_buffers_[fi].buffer, &offset);

    // 6 vertices per instance (2 triangles), instanced rendering
    vkCmdDraw(cmd, 6, static_cast<uint32_t>(instances_.size()), 0, 0);
}

} // namespace oh
