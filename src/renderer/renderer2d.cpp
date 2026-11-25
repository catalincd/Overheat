#include "renderer/renderer2d.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace oh {

namespace {

constexpr float KIND_RECT = 0, KIND_GLYPH = 1, KIND_SQUIGGLE = 2;

struct Globals {
    glm::vec2 viewport;
    float time;
    float text_gamma;
};

uint32_t decode_utf8(std::string_view s, size_t& i) {
    auto b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80) return b;
    int extra = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : (b >= 0xC0) ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = b & (0x3F >> extra);
    for (int k = 0; k < extra; k++) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

} // namespace

bool Renderer2D::init(VulkanContext& vk, FontAtlas& font) {
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 3;
    layout_ci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(vk.device(), &layout_ci, nullptr, &desc_layout_) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts    = &desc_layout_;
    if (vkCreatePipelineLayout(vk.device(), &pl_ci, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return false;

    std::string dir = SHADER_DIR;
    VkShaderModule vert = vk.load_shader(dir + "/quad.vert.spv");
    VkShaderModule frag = vk.load_shader(dir + "/quad.frag.spv");
    if (!vert || !frag) {
        fprintf(stderr, "Renderer2D: failed to load shaders from %s\n", dir.c_str());
        return false;
    }
    pipeline_ = vk.create_graphics_pipeline(vert, frag, pipeline_layout_, {}, {}, true);
    vkDestroyShaderModule(vk.device(), vert, nullptr);
    vkDestroyShaderModule(vk.device(), frag, nullptr);
    if (!pipeline_) return false;

    constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        quad_buffers_[i]    = vk.create_buffer(sizeof(GpuQuad) * MAX_QUADS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
        uniform_buffers_[i] = vk.create_buffer(sizeof(Globals), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host);
    }

    VkDescriptorSetLayout layouts[VulkanContext::MAX_FRAMES];
    for (auto& l : layouts) l = desc_layout_;
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = vk.descriptor_pool();
    alloc.descriptorSetCount = VulkanContext::MAX_FRAMES;
    alloc.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(vk.device(), &alloc, desc_sets_) != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        VkDescriptorBufferInfo ubo{uniform_buffers_[i].buffer, 0, sizeof(Globals)};
        VkDescriptorBufferInfo ssbo{quad_buffers_[i].buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo img{font.sampler(), font.image_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet w[3]{};
        for (int k = 0; k < 3; k++) {
            w[k].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet          = desc_sets_[i];
            w[k].dstBinding      = static_cast<uint32_t>(k);
            w[k].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         w[0].pBufferInfo = &ubo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[1].pBufferInfo = &ssbo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo  = &img;
        vkUpdateDescriptorSets(vk.device(), 3, w, 0, nullptr);
    }

    quads_.reserve(16384);
    return true;
}

void Renderer2D::rebind_font(VulkanContext& vk, FontAtlas& font) {
    VkDescriptorImageInfo img{font.sampler(), font.image_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = desc_sets_[i];
        w.dstBinding = 2;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
    }
}

void Renderer2D::cleanup(VulkanContext& vk) {
    if (!vk.device()) return;
    vkDeviceWaitIdle(vk.device());
    for (uint32_t i = 0; i < VulkanContext::MAX_FRAMES; i++) {
        vk.destroy_buffer(quad_buffers_[i]);
        vk.destroy_buffer(uniform_buffers_[i]);
    }
    if (pipeline_)        vkDestroyPipeline(vk.device(), pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(vk.device(), pipeline_layout_, nullptr);
    if (desc_layout_)     vkDestroyDescriptorSetLayout(vk.device(), desc_layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    desc_layout_ = VK_NULL_HANDLE;
}

void Renderer2D::begin(VkExtent2D extent) {
    extent_ = extent;
    quads_.clear();
    clip_stack_.clear();
    fade_start_ = fade_end_ = 0;
    alpha_ = 1.0f;
}

glm::vec4 Renderer2D::current_clip() const {
    if (!clip_stack_.empty()) return clip_stack_.back();
    return {0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height)};
}

void Renderer2D::push_clip(Rect r) {
    glm::vec4 c = current_clip();
    clip_stack_.push_back({std::max(c.x, r.x), std::max(c.y, r.y),
                           std::min(c.z, r.x + r.w), std::min(c.w, r.y + r.h)});
}

void Renderer2D::pop_clip() {
    if (!clip_stack_.empty()) clip_stack_.pop_back();
}

void Renderer2D::push(const GpuQuad& q) {
    if (quads_.size() >= MAX_QUADS) {
        if (!overflow_warned_) {
            fprintf(stderr, "Renderer2D: quad budget (%zu) exceeded, dropping geometry\n", MAX_QUADS);
            overflow_warned_ = true;
        }
        return;
    }
    // Cull on the CPU too: most of a long file or scrollback is off-panel.
    glm::vec4 c = q.clip;
    if (alpha_ < 1.0f) {
        GpuQuad faded = q;
        faded.color.a *= alpha_;
        faded.color2.a *= alpha_;
        float w = q.rect.z + 1.0f;
        if (q.rect.x >= c.z || q.rect.y >= c.w || q.rect.x + w <= c.x || q.rect.y + q.rect.w <= c.y) return;
        quads_.push_back(faded);
        return;
    }
    float w = q.rect.z + 1.0f;
    if (q.rect.x >= c.z || q.rect.y >= c.w || q.rect.x + w <= c.x || q.rect.y + q.rect.w <= c.y)
        return;
    quads_.push_back(q);
}

void Renderer2D::add_rect(Rect r, Color color, float radius, float border_w, Color border_color) {
    if (r.w <= 0 || r.h <= 0) return;
    GpuQuad q{};
    q.rect   = {r.x, r.y, r.w, r.h};
    q.clip   = current_clip();
    q.color  = color.vec4();
    q.data   = {radius, border_w, 0, 0};
    q.color2 = border_color.vec4();
    q.params = {KIND_RECT, 0, fade_start_, fade_end_};
    push(q);
}

void Renderer2D::add_char(float x, float y, uint32_t cp, Color color, const FontAtlas& font) {
    add_glyph(x, y, font.glyph(cp), color);
}

void Renderer2D::add_glyph(float x, float y, const GlyphInfo* gi, Color color) {
    if (!gi || gi->width <= 0 || gi->height <= 0) return;
    float gx = x + gi->bearing_x;
    float px = std::floor(gx);
    float frac = gx - px;
    if (frac < 1.0f / 16.0f) frac = 0;
    else if (frac > 15.0f / 16.0f) { px += 1; frac = 0; }

    GpuQuad q{};
    q.rect   = {px, std::round(y) - gi->bearing_y, gi->width, gi->height};
    q.clip   = current_clip();
    q.color  = color.vec4();
    q.data   = {gi->atlas_x, gi->atlas_y, gi->width, gi->height};
    q.params = {KIND_GLYPH, frac, fade_start_, fade_end_};
    push(q);
}

float Renderer2D::add_text(float x, float y, std::string_view text, Color color, const FontAtlas& font) {
    float pen = x;
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = decode_utf8(text, i);
        if (cp == '\n') { pen = x; y += font.line_height(); continue; }
        if (cp == '\t') { pen += font.char_width() * 4.0f; continue; }
        const GlyphInfo* gi = font.glyph(cp);
        add_char(pen, y, cp, color, font);
        pen += gi ? gi->advance : font.char_width();
    }
    return pen;
}

void Renderer2D::add_squiggle(Rect r, Color color, float amplitude, float wavelength, float thickness) {
    GpuQuad q{};
    q.rect   = {r.x, r.y, r.w, r.h};
    q.clip   = current_clip();
    q.color  = color.vec4();
    q.data   = {amplitude, wavelength, thickness, 0};
    q.params = {KIND_SQUIGGLE, 0, fade_start_, fade_end_};
    push(q);
}

void Renderer2D::flush(VulkanContext& vk, VkCommandBuffer cmd) {
    if (quads_.empty()) return;
    uint32_t fi = vk.frame_index();

    Globals glob{};
    glob.viewport   = {static_cast<float>(extent_.width), static_cast<float>(extent_.height)};
    glob.time       = 0;
    glob.text_gamma = 1.45f;
    std::memcpy(uniform_buffers_[fi].mapped, &glob, sizeof(glob));
    std::memcpy(quad_buffers_[fi].mapped, quads_.data(), quads_.size() * sizeof(GpuQuad));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &desc_sets_[fi], 0, nullptr);
    vkCmdDraw(cmd, 6, static_cast<uint32_t>(quads_.size()), 0, 0);
}

} // namespace oh
