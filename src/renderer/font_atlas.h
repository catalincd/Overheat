#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;

namespace oh {

// Glyph cache: glyphs are rasterized on first use from the first face in the
// fallback chain that has them, shelf-packed into one R8 atlas, and uploaded
// in a batch by upload_pending() before the frame's render pass begins.
class FontAtlas {
public:
    bool init(VulkanContext& vk, const std::vector<std::string>& font_paths, float font_size);
    void cleanup(VulkanContext& vk);

    // Logically const: a cache miss rasterizes and records a pending upload.
    const GlyphInfo* glyph(uint32_t codepoint) const;

    // Shaping with the primary face (ligatures / contextual alternates). Each glyph
    // carries the byte offset of the character it starts at. gid 0 = not in the face.
    struct Shaped {
        uint32_t gid;
        uint32_t cluster;
        float x_offset, y_offset;
    };
    void shape(std::string_view text, std::vector<Shaped>& out) const;
    const GlyphInfo* glyph_by_index(uint32_t gid) const;  // primary face
    // A codepoint at an explicit pixel size (UI icons), cached separately.
    const GlyphInfo* glyph_sized(uint32_t codepoint, uint32_t px) const;
    void set_ligatures(bool on) { ligatures_ = on; }
    bool ligatures() const { return ligatures_; }
    // Bumped on every init(): glyph ids from a previous font are invalid.
    uint64_t generation() const { return generation_; }
    void upload_pending(VulkanContext& vk);

    float line_height() const { return line_height_; }
    float char_width() const { return char_width_; }
    float ascender() const { return ascender_; }
    float descender() const { return descender_; }
    VkImageView image_view() const { return atlas_image_.view; }
    VkSampler sampler() const { return sampler_; }
    uint32_t atlas_width() const { return ATLAS_SIZE; }
    uint32_t atlas_height() const { return ATLAS_SIZE; }

    static constexpr uint32_t ATLAS_SIZE = 2048;

private:
    const GlyphInfo* rasterize(uint32_t codepoint) const;
    const GlyphInfo* rasterize_glyph(FT_Face face, uint32_t index, GlyphInfo info_seed, bool cache_by_index) const;

    FT_Library ft_ = nullptr;
    std::vector<FT_Face> faces_;

    mutable std::unordered_map<uint32_t, GlyphInfo> glyphs_;
    mutable std::unordered_map<uint32_t, GlyphInfo> by_gid_;
    mutable std::unordered_map<uint64_t, GlyphInfo> sized_;
    float font_size_ = 15.0f;
    hb_font_t* hb_font_ = nullptr;
    hb_buffer_t* hb_buf_ = nullptr;
    bool ligatures_ = true;
    uint64_t generation_ = 0;
    mutable std::vector<uint8_t> pixels_;
    mutable uint32_t pen_x_ = 1, pen_y_ = 1, shelf_h_ = 0;
    mutable uint32_t dirty_x0_ = UINT32_MAX, dirty_y0_ = UINT32_MAX, dirty_x1_ = 0, dirty_y1_ = 0;
    mutable bool full_warned_ = false;
    mutable GlyphInfo scratch_{};

    Image atlas_image_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    float line_height_ = 0;
    float char_width_ = 0;
    float ascender_ = 0;
    float descender_ = 0;
};

} // namespace oh
