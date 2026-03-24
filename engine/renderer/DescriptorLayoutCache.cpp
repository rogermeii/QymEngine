#include "renderer/DescriptorLayoutCache.h"
#include "renderer/VkDispatch.h"
#include <algorithm>
#include <stdexcept>

namespace QymEngine {

bool DescriptorLayoutCache::LayoutKey::operator==(const LayoutKey& other) const {
    if (bindings.size() != other.bindings.size()) return false;
    for (size_t i = 0; i < bindings.size(); i++) {
        if (bindings[i].binding != other.bindings[i].binding) return false;
        if (bindings[i].descriptorType != other.bindings[i].descriptorType) return false;
        if (bindings[i].descriptorCount != other.bindings[i].descriptorCount) return false;
        if (bindings[i].stageFlags != other.bindings[i].stageFlags) return false;
    }
    return true;
}

size_t DescriptorLayoutCache::LayoutKeyHash::operator()(const LayoutKey& key) const {
    size_t hash = key.bindings.size();
    for (auto& b : key.bindings) {
        size_t bHash = b.binding | (b.descriptorType << 8) |
                       (b.stageFlags << 16) | (b.descriptorCount << 24);
        hash ^= std::hash<size_t>()(bHash);
    }
    return hash;
}

VkDescriptorSetLayout DescriptorLayoutCache::getOrCreate(VkDevice device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    LayoutKey key;
    key.bindings = bindings;
    std::sort(key.bindings.begin(), key.bindings.end(),
        [](const auto& a, const auto& b) { return a.binding < b.binding; });

    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it->second;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(key.bindings.size());
    layoutInfo.pBindings = key.bindings.data();

    VkDescriptorSetLayout layout;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("failed to create descriptor set layout in cache!");

    m_cache[key] = layout;
    return layout;
}

void DescriptorLayoutCache::cleanup(VkDevice device) {
    for (auto& [key, layout] : m_cache)
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
    m_cache.clear();
}

} // namespace QymEngine
