#include "renderer/font_atlas.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

namespace oh {

bool FontAtlas::init(VulkanContext& vk, const std::string& font_path, float font_size) {
    font_size_ = font_size;

    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "Failed to init FreeType\n");
        return false;
    }

    FT_Face face;
    if (FT_New_Face(ft, font_path.c_str(), 0, &face)) {
        fprintf(stderr, "Failed to load font: %s\n", font_path.c_str());
        FT_Done_FreeType(ft);
        return false;
    }

    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(font_size));

    // Compute metrics from face
    ascender_    = static_cast<float>(face->size->metrics.ascender) / 64.0f;
    descender_   = static_cast<float>(face->size->metrics.descender) / 64.0f;
    line_height_ = static_cast<float>(face->size->metrics.height) / 64.0f;

    // First pass: render all glyphs to measure total area
    struct RenderedGlyph {
        uint32_t codepoint;
        uint32_t width, height;
        int bearing_x, bearing_y;
        float advance;
        std::vector<uint8_t> bitmap;
    };
    std::vector<RenderedGlyph> rendered;
    rendered.reserve(95); // ASCII 32-126

    uint32_t total_area = 0;
    uint32_t max_glyph_h = 0;

    for (uint32_t cp = 32; cp <= 126; cp++) {
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) {
            continue;
        }
        FT_GlyphSlot g = face->glyph;

        RenderedGlyph rg;
        rg.codepoint = cp;
        rg.width     = g->bitmap.width;
        rg.height    = g->bitmap.rows;
        rg.bearing_x = g->bitmap_left;
        rg.bearing_y = g->bitmap_top;
        rg.advance   = static_cast<float>(g->advance.x) / 64.0f;

        if (rg.width > 0 && rg.height > 0) {
            rg.bitmap.resize(rg.width * rg.height);
            // FreeType bitmaps may have pitch != width
            for (uint32_t row = 0; row < rg.height; row++) {
                memcpy(rg.bitmap.data() + row * rg.width,
                       g->bitmap.buffer + row * g->bitmap.pitch,
                       rg.width);
            }
            total_area += rg.width * rg.height;
            max_glyph_h = std::max(max_glyph_h, rg.height);
        }

        rendered.push_back(std::move(rg));
    }

    // Store char_width from a representative character (e.g. 'M')
    for (auto& rg : rendered) {
        if (rg.codepoint == 'M') {
            char_width_ = rg.advance;
            break;
        }
    }
    if (char_width_ == 0.0f && !rendered.empty()) {
        char_width_ = rendered[0].advance;
    }

    // Auto-size atlas: start from sqrt of total area, round up to power of 2
    uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(total_area) * 1.5f)));
    atlas_w_ = 256;
    while (atlas_w_ < side) atlas_w_ *= 2;
    if (atlas_w_ > 2048) atlas_w_ = 2048;
    atlas_h_ = atlas_w_; // start square

    // Row-based packing
    std::vector<uint8_t> atlas_data(atlas_w_ * atlas_h_, 0);
    uint32_t pen_x = 1; // 1px padding
    uint32_t pen_y = 1;
    uint32_t row_h = 0;
    const uint32_t padding = 1;

    for (auto& rg : rendered) {
        if (rg.width == 0 || rg.height == 0) {
            // Space or non-renderable: store UV as zero-area
            GlyphInfo info{};
            info.u0 = 0; info.v0 = 0;
            info.u1 = 0; info.v1 = 0;
            info.width    = 0;
            info.height   = 0;
            info.bearing_x = static_cast<float>(rg.bearing_x);
            info.bearing_y = static_cast<float>(rg.bearing_y);
            info.advance   = rg.advance;
            glyphs_[rg.codepoint] = info;
            continue;
        }

        // Check if glyph fits in current row
        if (pen_x + rg.width + padding > atlas_w_) {
            pen_x = padding;
            pen_y += row_h + padding;
            row_h = 0;
        }

        // If we exceed atlas height, grow it
        if (pen_y + rg.height + padding > atlas_h_) {
            uint32_t new_h = atlas_h_ * 2;
            if (new_h > 4096) new_h = 4096;
            atlas_data.resize(atlas_w_ * new_h, 0);
            atlas_h_ = new_h;
        }

        // Blit glyph into atlas
        for (uint32_t row = 0; row < rg.height; row++) {
            memcpy(atlas_data.data() + (pen_y + row) * atlas_w_ + pen_x,
                   rg.bitmap.data() + row * rg.width,
                   rg.width);
        }

        float inv_w = 1.0f / static_cast<float>(atlas_w_);
        float inv_h = 1.0f / static_cast<float>(atlas_h_);

        GlyphInfo info{};
        info.u0 = static_cast<float>(pen_x) * inv_w;
        info.v0 = static_cast<float>(pen_y) * inv_h;
        info.u1 = static_cast<float>(pen_x + rg.width) * inv_w;
        info.v1 = static_cast<float>(pen_y + rg.height) * inv_h;
        info.width     = static_cast<float>(rg.width);
        info.height    = static_cast<float>(rg.height);
        info.bearing_x = static_cast<float>(rg.bearing_x);
        info.bearing_y = static_cast<float>(rg.bearing_y);
        info.advance   = rg.advance;
        glyphs_[rg.codepoint] = info;

        pen_x += rg.width + padding;
        row_h = std::max(row_h, rg.height);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    // Upload atlas to Vulkan as R8_UNORM
    atlas_image_ = vk.create_image(atlas_w_, atlas_h_, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    vk.upload_image(atlas_image_, atlas_data.data(), atlas_w_, atlas_h_, VK_FORMAT_R8_UNORM);

    sampler_ = vk.create_sampler(VK_FILTER_LINEAR);

    fprintf(stderr, "Font atlas: %ux%u, %zu glyphs, char_width=%.1f, line_height=%.1f\n",
            atlas_w_, atlas_h_, glyphs_.size(), char_width_, line_height_);

    return true;
}

void FontAtlas::cleanup(VulkanContext& vk) {
    if (sampler_) {
        vkDestroySampler(vk.device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    vk.destroy_image(atlas_image_);
    glyphs_.clear();
}

const GlyphInfo* FontAtlas::glyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) return &it->second;
    // Fallback to '?' if codepoint not found
    it = glyphs_.find('?');
    if (it != glyphs_.end()) return &it->second;
    return nullptr;
}

} // namespace oh
