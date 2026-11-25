#include "renderer/font_atlas.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace oh {

bool FontAtlas::init(VulkanContext& vk, const std::vector<std::string>& font_paths, float font_size) {
    generation_++;
    glyphs_.clear();
    by_gid_.clear();
    pen_x_ = pen_y_ = 1;
    shelf_h_ = 0;
    full_warned_ = false;
    char_width_ = 0;
    dirty_x0_ = dirty_y0_ = UINT32_MAX;
    dirty_x1_ = dirty_y1_ = 0;
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
    hb_font_ = hb_ft_font_create_referenced(primary);
    hb_buf_ = hb_buffer_create();
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
    by_gid_.clear();
    if (hb_buf_) hb_buffer_destroy(hb_buf_);
    if (hb_font_) hb_font_destroy(hb_font_);
    hb_buf_ = nullptr;
    hb_font_ = nullptr;
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
    GlyphInfo seed{};
    seed.advance = -1.0f;  // filled from the face
    const GlyphInfo* gi = rasterize_glyph(face, index, seed, false);
    if (!gi) return nullptr;
    GlyphInfo info = *gi;
    // Fallback glyphs must not break the monospace grid.
    if (face != faces_[0] && char_width_ > 0) info.advance = char_width_;
    return &(glyphs_[codepoint] = info);
}

const GlyphInfo* FontAtlas::rasterize_glyph(FT_Face face, uint32_t index, GlyphInfo info, bool cache_by_index) const {
    if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return nullptr;
    FT_GlyphSlot g = face->glyph;
    uint32_t w = g->bitmap.width, h = g->bitmap.rows;
    info.width     = static_cast<float>(w);
    info.height    = static_cast<float>(h);
    info.bearing_x = static_cast<float>(g->bitmap_left);
    info.bearing_y = static_cast<float>(g->bitmap_top);
    info.advance   = static_cast<float>(g->advance.x) / 64.0f;
    if (w > 0 && h > 0) {
        const uint32_t pad = 1;
        if (pen_x_ + w + pad > ATLAS_SIZE) {
            pen_x_ = pad;
            pen_y_ += shelf_h_ + pad;
            shelf_h_ = 0;
        }
        if (pen_y_ + h + pad > ATLAS_SIZE) {
            if (!full_warned_) {
                fprintf(stderr, "FontAtlas: atlas full, dropping glyphs\n");
                full_warned_ = true;
            }
            info.width = info.height = 0;
        } else {
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
    }
    if (!cache_by_index) {
        scratch_ = info;  // caller copies it into the codepoint cache
        return &scratch_;
    }
    return &(by_gid_[index] = info);
}

const GlyphInfo* FontAtlas::glyph_by_index(uint32_t gid) const {
    auto it = by_gid_.find(gid);
    if (it != by_gid_.end()) return &it->second;
    return rasterize_glyph(faces_[0], gid, GlyphInfo{}, true);
}

void FontAtlas::shape(std::string_view text, std::vector<Shaped>& out) const {
    out.clear();
    if (!hb_font_ || text.empty()) return;
    hb_buffer_clear_contents(hb_buf_);
    hb_buffer_add_utf8(hb_buf_, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(hb_buf_);
    hb_feature_t off[2];
    hb_feature_from_string("-calt", -1, &off[0]);
    hb_feature_from_string("-liga", -1, &off[1]);
    hb_shape(hb_font_, hb_buf_, ligatures_ ? nullptr : off, ligatures_ ? 0 : 2);
    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(hb_buf_, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(hb_buf_, &n);
    out.reserve(n);
    for (unsigned i = 0; i < n; i++)
        out.push_back({info[i].codepoint, info[i].cluster, pos[i].x_offset / 64.0f, pos[i].y_offset / 64.0f});
}

void FontAtlas::upload_pending(VulkanContext& vk) {
    if (dirty_x1_ <= dirty_x0_ || dirty_y1_ <= dirty_y0_) return;
    vk.update_image_region(atlas_image_, pixels_.data(), ATLAS_SIZE, dirty_x0_, dirty_y0_,
                           dirty_x1_ - dirty_x0_, dirty_y1_ - dirty_y0_);
    dirty_x0_ = dirty_y0_ = UINT32_MAX;
    dirty_x1_ = dirty_y1_ = 0;
}

} // namespace oh
