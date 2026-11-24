#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"
#include "renderer/font_atlas.h"

namespace oh {

class TextRenderer {
public:
    bool init(VulkanContext& vk, FontAtlas& font);
    void cleanup(VulkanContext& vk);

    void begin_batch();
    void add_text(float x, float y, const std::string& text, Color color, const FontAtlas& font);
    void add_char(float x, float y, uint32_t codepoint, Color color, const FontAtlas& font);
    void flush(VulkanContext& vk, VkCommandBuffer cmd, VkExtent2D extent, glm::vec2 scroll = {0,0});

    static constexpr size_t MAX_INSTANCES = 200000;

private:
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_sets_[VulkanContext::MAX_FRAMES];
    Buffer instance_buffers_[VulkanContext::MAX_FRAMES];
    Buffer uniform_buffers_[VulkanContext::MAX_FRAMES];

    std::vector<TextInstance> instances_;
};

} // namespace oh
