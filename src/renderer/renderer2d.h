#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"
#include "renderer/font_atlas.h"
#include <vector>

namespace oh {

// One ordered stream of quads per frame, read by the shaders from a storage buffer
// via gl_InstanceIndex and drawn with a single instanced draw. Submission order is
// paint order, so overlays need no extra passes.
class Renderer2D {
public:
    bool init(VulkanContext& vk, FontAtlas& font);
    void cleanup(VulkanContext& vk);

    void begin(VkExtent2D extent);
    void flush(VulkanContext& vk, VkCommandBuffer cmd);
    // Point the atlas binding at a re-created font (zoom). Call with the device idle.
    void rebind_font(VulkanContext& vk, FontAtlas& font);

    void push_clip(Rect r);
    void pop_clip();

    // Horizontal alpha fade for subsequent quads; pass start >= end to disable.
    void set_fade(float start_x, float end_x) { fade_start_ = start_x; fade_end_ = end_x; }
    // Opacity multiplier for subsequent quads (fade-in animations).
    void set_alpha(float a) { alpha_ = a; }
    // Window background opacity, used when the VulkanContext is translucent: filled
    // rects reach the desktop at this alpha, text and squiggles stay opaque.
    void set_window_opacity(float o) { opacity_ = o; }
    bool can_translucent() const { return pipeline_mask_ != VK_NULL_HANDLE; }

    void add_rect(Rect rect, Color color, float corner_radius = 0.0f,
                  float border_width = 0.0f, Color border_color = {0, 0, 0, 0});
    // y is the text baseline. Returns the pen x after the last glyph. Text is UTF-8.
    float add_text(float x, float y, std::string_view text, Color color, const FontAtlas& font);
    void add_char(float x, float y, uint32_t codepoint, Color color, const FontAtlas& font);
    void add_glyph(float x, float y, const GlyphInfo* gi, Color color);
    void add_squiggle(Rect rect, Color color, float amplitude = 1.5f,
                      float wavelength = 6.0f, float thickness = 1.2f);

    size_t quad_count() const { return quads_.size(); }

    static constexpr size_t MAX_QUADS = 131072;

private:
    struct alignas(16) GpuQuad {
        glm::vec4 rect;
        glm::vec4 clip;
        glm::vec4 color;
        glm::vec4 data;
        glm::vec4 color2;
        glm::vec4 params;
    };
    static_assert(sizeof(GpuQuad) == 96);

    void push(const GpuQuad& q);
    glm::vec4 current_clip() const;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline pipeline_mask_ = VK_NULL_HANDLE;  // translucent-window variant
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_sets_[VulkanContext::MAX_FRAMES]{};
    Buffer quad_buffers_[VulkanContext::MAX_FRAMES];
    Buffer uniform_buffers_[VulkanContext::MAX_FRAMES];

    std::vector<GpuQuad> quads_;
    std::vector<glm::vec4> clip_stack_;
    VkExtent2D extent_{};
    float fade_start_ = 0, fade_end_ = 0;
    float alpha_ = 1.0f;
    float opacity_ = 1.0f;
    bool overflow_warned_ = false;
};

} // namespace oh
