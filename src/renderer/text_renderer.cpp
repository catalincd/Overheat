#include "renderer/text_renderer.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace oh {

struct TextUBO {
    glm::vec2 screen_size;
    glm::vec2 scroll_offset;
    float time;
    float _pad[3]; // pad to 32 bytes (std140 alignment)
};

bool TextRenderer::init(VulkanContext& vk, FontAtlas& font) {
    // --- Descriptor set layout ---
    VkDescriptorSetLayoutBinding bindings[2]{};
    // Binding 0: uniform buffer
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    // Binding 1: font atlas sampler
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 2;
    layout_ci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(vk.device(), &layout_ci, nullptr, &desc_layout_) != VK_SUCCESS) {
        fprintf(stderr, "TextRenderer: failed to create descriptor set layout\n");
        return false;
    }

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts    = &desc_layout_;
    if (vkCreatePipelineLayout(vk.device(), &pl_ci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        fprintf(stderr, "TextRenderer: failed to create pipeline layout\n");
        return false;
    }

    // --- Shaders ---
    std::string shader_dir = SHADER_DIR;
    VkShaderModule vert = vk.load_shader(shader_dir + "/text.vert.spv");
    VkShaderModule frag = vk.load_shader(shader_dir + "/text.frag.spv");
    if (!vert || !frag) {
        fprintf(stderr, "TextRenderer: failed to load shaders\n");
        return false;
    }

    // --- Vertex input: instance buffer only ---
    VkVertexInputBindingDescription binding_desc{};
    binding_desc.binding   = 0;
    binding_desc.stride    = sizeof(TextInstance);
    binding_desc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrs[5]{};
    // location 0: pos (vec2)
    attrs[0].binding  = 0;
    attrs[0].location = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(TextInstance, pos);
    // location 1: size (vec2)
    attrs[1].binding  = 0;
    attrs[1].location = 1;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(TextInstance, size);
    // location 2: uv_min (vec2)
    attrs[2].binding  = 0;
    attrs[2].location = 2;
    attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset   = offsetof(TextInstance, uv_min);
    // location 3: uv_max (vec2)
    attrs[3].binding  = 0;
    attrs[3].location = 3;
    attrs[3].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[3].offset   = offsetof(TextInstance, uv_max);
    // location 4: color (vec4)
    attrs[4].binding  = 0;
    attrs[4].location = 4;
    attrs[4].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[4].offset   = offsetof(TextInstance, color);

    pipeline_ = vk.create_graphics_pipeline(
        vert, frag, pipeline_layout_,
        {binding_desc},
        {attrs[0], attrs[1], attrs[2], attrs[3], attrs[4]},
        true // alpha blending
    );

    vkDestroyShaderModule(vk.device(), vert, nullptr);
    vkDestroyShaderModule(vk.device(), frag, nullptr);

    if (!pipeline_) {
        fprintf(stderr, "TextRenderer: failed to create pipeline\n");
        return false;
    }

    // --- Per-frame resources ---
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        // Instance buffer (host-visible, persistent mapped)
        instance_buffers_[i] = vk.create_buffer(
            sizeof(TextInstance) * MAX_INSTANCES,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        // Uniform buffer
        uniform_buffers_[i] = vk.create_buffer(
            sizeof(TextUBO),
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
        buf_info.range  = sizeof(TextUBO);

        VkDescriptorImageInfo img_info{};
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        img_info.imageView   = font.image_view();
        img_info.sampler     = font.sampler();

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = desc_sets_[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &buf_info;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = desc_sets_[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &img_info;

        vkUpdateDescriptorSets(vk.device(), 2, writes, 0, nullptr);
    }

    instances_.reserve(MAX_INSTANCES);
    return true;
}

void TextRenderer::cleanup(VulkanContext& vk) {
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

void TextRenderer::begin_batch() {
    instances_.clear();
}

void TextRenderer::add_text(float x, float y, const std::string& text, Color color, const FontAtlas& font) {
    float cursor_x = x;
    glm::vec4 col = color.vec4();

    for (unsigned char ch : text) {
        if (ch == '\n') {
            cursor_x = x;
            y += font.line_height();
            continue;
        }
        if (ch == '\t') {
            cursor_x += font.char_width() * 4.0f;
            continue;
        }

        const GlyphInfo* gi = font.glyph(static_cast<uint32_t>(ch));
        if (!gi) {
            cursor_x += font.char_width();
            continue;
        }

        if (gi->width > 0 && gi->height > 0 && instances_.size() < MAX_INSTANCES) {
            TextInstance inst;
            inst.pos    = {cursor_x + gi->bearing_x, y + (font.ascender() - gi->bearing_y)};
            inst.size   = {gi->width, gi->height};
            inst.uv_min = {gi->u0, gi->v0};
            inst.uv_max = {gi->u1, gi->v1};
            inst.color  = col;
            instances_.push_back(inst);
        }

        cursor_x += gi->advance;
    }
}

void TextRenderer::add_char(float x, float y, uint32_t codepoint, Color color, const FontAtlas& font) {
    const GlyphInfo* gi = font.glyph(codepoint);
    if (!gi || instances_.size() >= MAX_INSTANCES) return;

    if (gi->width > 0 && gi->height > 0) {
        TextInstance inst;
        inst.pos    = {x + gi->bearing_x, y + (font.ascender() - gi->bearing_y)};
        inst.size   = {gi->width, gi->height};
        inst.uv_min = {gi->u0, gi->v0};
        inst.uv_max = {gi->u1, gi->v1};
        inst.color  = color.vec4();
        instances_.push_back(inst);
    }
}

void TextRenderer::flush(VulkanContext& vk, VkCommandBuffer cmd, VkExtent2D extent, glm::vec2 scroll) {
    if (instances_.empty()) return;

    uint32_t fi = vk.frame_index();

    // Update uniform buffer
    TextUBO ubo{};
    ubo.screen_size   = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    ubo.scroll_offset = scroll;
    ubo.time          = 0.0f; // could use actual time
    memcpy(uniform_buffers_[fi].mapped, &ubo, sizeof(TextUBO));

    // Upload instances
    size_t copy_size = instances_.size() * sizeof(TextInstance);
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
