#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"

namespace oh {

class UIRenderer {
public:
    bool init(VulkanContext& vk);
    void cleanup(VulkanContext& vk);

    void begin_batch();
    void add_rect(Rect rect, Color color, float corner_radius = 0.0f,
                  float border_width = 0.0f, Color border_color = {0,0,0,0});
    void flush(VulkanContext& vk, VkCommandBuffer cmd, VkExtent2D extent);

    static constexpr size_t MAX_INSTANCES = 10000;

private:
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_sets_[VulkanContext::MAX_FRAMES];
    Buffer instance_buffers_[VulkanContext::MAX_FRAMES];
    Buffer uniform_buffers_[VulkanContext::MAX_FRAMES];

    std::vector<RectInstance> instances_;
};

} // namespace oh
