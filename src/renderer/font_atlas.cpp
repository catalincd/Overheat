#include "renderer/font_atlas.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace oh {

bool FontAtlas::init(VulkanContext& vk, const std::vector<std::string>& font_paths, float font_size) {
    if (FT_Init_FreeType(&ft_)) {
        fprintf(stderr, "Failed to init FreeType\n");
        return false;
    }
    for (auto& path : font_paths) {
        FT_Face face;
        if (FT_New_Face(ft_, path.c_str(), 0, &face)) continue;
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(font_size));
        faces_.push_back(face);
        fprintf(stderr, "Font %s: %s\n", faces_.size() == 1 ? "primary" : "fallback", path.c_str());
    }
    if (faces_.empty()) {
        fprintf(stderr, "No usable font found\n");
        return false;
    }

    FT_Face primary = faces_[0];
    ascender_    = static_cast<float>(primary->size->metrics.ascender) / 64.0f;
    descender_   = static_cast<float>(primary->size->metrics.descender) / 64.0f;
    line_height_ = static_cast<float>(primary->size->metrics.height) / 64.0f;

    pixels_.assign(size_t(ATLAS_SIZE) * ATLAS_SIZE, 0);
    atlas_image_ = vk.create_image(ATLAS_SIZE, ATLAS_SIZE, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    // Warm the cache with printable ASCII, then upload the whole atlas once.
    for (uint32_t cp = 32; cp < 127; cp++) glyph(cp);
    vk.upload_image(atlas_image_, pixels_.data(), ATLAS_SIZE, ATLAS_SIZE, VK_FORMAT_R8_UNORM);
    dirty_x0_ = dirty_y0_ = UINT32_MAX;
    dirty_x1_ = dirty_y1_ = 0;

    char_width_ = glyph('M')->advance;
    sampler_ = vk.create_sampler(VK_FILTER_NEAREST);

    fprintf(stderr, "Font atlas: %ux%u cache, char_width=%.1f, line_height=%.1f\n",
            ATLAS_SIZE, ATLAS_SIZE, char_width_, line_height_);
    return true;
}

void FontAtlas::cleanup(VulkanContext& vk) {
    if (sampler_) {
        vkDestroySampler(vk.device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    vk.destroy_image(atlas_image_);
    glyphs_.clear();
    for (auto f : faces_) FT_Done_Face(f);
    faces_.clear();
    if (ft_) FT_Done_FreeType(ft_);
    ft_ = nullptr;
}

const GlyphInfo* FontAtlas::glyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) return &it->second;
    return rasterize(codepoint);
}

const GlyphInfo* FontAtlas::rasterize(uint32_t codepoint) const {
    FT_Face face = nullptr;
    FT_UInt index = 0;
    for (auto f : faces_) {
        index = FT_Get_Char_Index(f, codepoint);
        if (index) { face = f; break; }
    }
    if (!face) {
        // Cache the miss as an alias of the replacement glyph.
        const GlyphInfo* repl = codepoint != 0xFFFD ? glyph(0xFFFD) : nullptr;
        if (!repl && codepoint != '?') repl = glyph('?');
        if (!repl) return nullptr;
        return &(glyphs_[codepoint] = *repl);
    }
    if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return nullptr;

    FT_GlyphSlot g = face->glyph;
    uint32_t w = g->bitmap.width, h = g->bitmap.rows;
    GlyphInfo info{};
    info.width     = static_cast<float>(w);
    info.height    = static_cast<float>(h);
    info.bearing_x = static_cast<float>(g->bitmap_left);
    info.bearing_y = static_cast<float>(g->bitmap_top);
    info.advance   = static_cast<float>(g->advance.x) / 64.0f;
    // Fallback glyphs must not break the monospace grid.
    if (face != faces_[0] && char_width_ > 0) info.advance = char_width_;

    if (w > 0 && h > 0) {
        const uint32_t pad = 1;
        if (pen_x_ + w + pad > ATLAS_SIZE) {
            pen_x_ = pad;
            pen_y_ += shelf_h_ + pad;
            shelf_h_ = 0;
        }
        if (pen_y_ + h + pad > ATLAS_SIZE) {
            if (!full_warned_) {
                fprintf(stderr, "FontAtlas: atlas full, glyph U+%04X dropped\n", codepoint);
                full_warned_ = true;
            }
            info.width = info.height = 0;
            return &(glyphs_[codepoint] = info);
        }
        for (uint32_t row = 0; row < h; row++)
            memcpy(&pixels_[size_t(pen_y_ + row) * ATLAS_SIZE + pen_x_],
                   g->bitmap.buffer + ptrdiff_t(row) * g->bitmap.pitch, w);
        info.atlas_x = static_cast<float>(pen_x_);
        info.atlas_y = static_cast<float>(pen_y_);
        dirty_x0_ = std::min(dirty_x0_, pen_x_);
        dirty_y0_ = std::min(dirty_y0_, pen_y_);
        dirty_x1_ = std::max(dirty_x1_, pen_x_ + w);
        dirty_y1_ = std::max(dirty_y1_, pen_y_ + h);
        pen_x_ += w + pad;
        shelf_h_ = std::max(shelf_h_, h);
    }
    return &(glyphs_[codepoint] = info);
}

void FontAtlas::upload_pending(VulkanContext& vk) {
    if (dirty_x1_ <= dirty_x0_ || dirty_y1_ <= dirty_y0_) return;
    vk.update_image_region(atlas_image_, pixels_.data(), ATLAS_SIZE, dirty_x0_, dirty_y0_,
                           dirty_x1_ - dirty_x0_, dirty_y1_ - dirty_y0_);
    dirty_x0_ = dirty_y0_ = UINT32_MAX;
    dirty_x1_ = dirty_y1_ = 0;
}

} // namespace oh
