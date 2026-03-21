#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <string>
#include "scene/Frustum.h"

namespace QymEngine {

class VulkanContext;
class CommandManager;
struct Vertex;

enum class MeshType { None = 0, Quad, Cube, Plane, Sphere };

// Utility: convert MeshType <-> string for serialization
inline std::string meshTypeToString(MeshType t) {
    switch (t) {
        case MeshType::Quad:   return "Quad";
        case MeshType::Cube:   return "Cube";
        case MeshType::Plane:  return "Plane";
        case MeshType::Sphere: return "Sphere";
        default:               return "None";
    }
}

inline MeshType meshTypeFromString(const std::string& s) {
    if (s == "Quad")   return MeshType::Quad;
    if (s == "Cube")   return MeshType::Cube;
    if (s == "Plane")  return MeshType::Plane;
    if (s == "Sphere") return MeshType::Sphere;
    return MeshType::None;
}

class MeshLibrary {
public:
    void init(VulkanContext& ctx, CommandManager& cmdMgr);
    void shutdown(VkDevice device);

    void bind(VkCommandBuffer cmd, MeshType type) const;
    uint32_t getIndexCount(MeshType type) const;

    // Get local-space AABB for built-in meshes
    AABB getAABB(MeshType type) const;

private:
    struct MeshGPU {
        VkBuffer       vertexBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory  = VK_NULL_HANDLE;
        VkBuffer       indexBuffer   = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory   = VK_NULL_HANDLE;
        uint32_t       indexCount    = 0;
    };

    std::unordered_map<MeshType, MeshGPU> m_meshes;

    void uploadMesh(VulkanContext& ctx, CommandManager& cmdMgr,
                    MeshType type,
                    const std::vector<Vertex>& vertices,
                    const std::vector<uint32_t>& indices);
};

} // namespace QymEngine
