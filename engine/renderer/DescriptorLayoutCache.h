#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>

namespace QymEngine {

class DescriptorLayoutCache {
public:
    // Returns existing layout if bindings match, otherwise creates new
    VkDescriptorSetLayout getOrCreate(VkDevice device,
        const std::vector<VkDescriptorSetLayoutBinding>& bindings);

    void cleanup(VkDevice device);

private:
    struct LayoutKey {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bool operator==(const LayoutKey& other) const;
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& key) const;
    };

    std::unordered_map<LayoutKey, VkDescriptorSetLayout, LayoutKeyHash> m_cache;
};

} // namespace QymEngine
