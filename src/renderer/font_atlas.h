#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"
#include <string>
#include <unordered_map>

namespace oh {

class FontAtlas {
public:
    bool init(VulkanContext& vk, const std::string& font_path, float font_size);
    void cleanup(VulkanContext& vk);

    const GlyphInfo* glyph(uint32_t codepoint) const;
    float line_height() const { return line_height_; }
    float char_width() const { return char_width_; }
    float ascender() const { return ascender_; }
    float descender() const { return descender_; }
    VkImageView image_view() const { return atlas_image_.view; }
    VkSampler sampler() const { return sampler_; }
    uint32_t atlas_width() const { return atlas_w_; }
    uint32_t atlas_height() const { return atlas_h_; }

private:
    std::unordered_map<uint32_t, GlyphInfo> glyphs_;
    Image atlas_image_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    float line_height_ = 0;
    float char_width_ = 0;
    float ascender_ = 0;
    float descender_ = 0;
    float font_size_ = 0;
    uint32_t atlas_w_ = 0, atlas_h_ = 0;
};

} // namespace oh
