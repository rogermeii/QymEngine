#include "renderer/MeshLibrary.h"
#include "renderer/VkDispatch.h"
#include "renderer/MeshData.h"
#include "renderer/Buffer.h"
#include "renderer/VulkanContext.h"
#include "renderer/CommandManager.h"

#include <cstring>
#include <stdexcept>

namespace QymEngine {

void MeshLibrary::init(VulkanContext& ctx, CommandManager& cmdMgr)
{
    // Generate and upload each mesh type
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        generateQuad(verts, idx);
        uploadMesh(ctx, cmdMgr, MeshType::Quad, verts, idx);
    }
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        generateCube(verts, idx);
        uploadMesh(ctx, cmdMgr, MeshType::Cube, verts, idx);
    }
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        generatePlane(verts, idx);
        uploadMesh(ctx, cmdMgr, MeshType::Plane, verts, idx);
    }
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        generateSphere(verts, idx);
        uploadMesh(ctx, cmdMgr, MeshType::Sphere, verts, idx);
    }
}

void MeshLibrary::shutdown(VkDevice device)
{
    for (auto& [type, mesh] : m_meshes)
    {
        if (mesh.vertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
        if (mesh.vertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.vertexMemory, nullptr);
        if (mesh.indexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
        if (mesh.indexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.indexMemory, nullptr);
    }
    m_meshes.clear();
}

void MeshLibrary::bind(VkCommandBuffer cmd, MeshType type) const
{
    auto it = m_meshes.find(type);
    if (it == m_meshes.end()) return;

    const MeshGPU& mesh = it->second;
    VkBuffer vertexBuffers[] = { mesh.vertexBuffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
}

uint32_t MeshLibrary::getIndexCount(MeshType type) const
{
    auto it = m_meshes.find(type);
    if (it == m_meshes.end()) return 0;
    return it->second.indexCount;
}

AABB MeshLibrary::getAABB(MeshType type) const
{
    AABB aabb;
    switch (type) {
        case MeshType::Cube:
            aabb.min = glm::vec3(-0.5f, -0.5f, -0.5f);
            aabb.max = glm::vec3( 0.5f,  0.5f,  0.5f);
            break;
        case MeshType::Plane:
            aabb.min = glm::vec3(-0.5f, 0.0f, -0.5f);
            aabb.max = glm::vec3( 0.5f, 0.0f,  0.5f);
            break;
        case MeshType::Quad:
            aabb.min = glm::vec3(-0.5f, -0.5f, 0.0f);
            aabb.max = glm::vec3( 0.5f,  0.5f, 0.0f);
            break;
        case MeshType::Sphere:
            aabb.min = glm::vec3(-0.5f, -0.5f, -0.5f);
            aabb.max = glm::vec3( 0.5f,  0.5f,  0.5f);
            break;
        case MeshType::None:
        default:
            break;
    }
    return aabb;
}

void MeshLibrary::uploadMesh(VulkanContext& ctx, CommandManager& cmdMgr,
                              MeshType type,
                              const std::vector<Vertex>& vertices,
                              const std::vector<uint32_t>& indices)
{
    VkDevice device = ctx.getDevice();
    MeshGPU mesh{};
    mesh.indexCount = static_cast<uint32_t>(indices.size());

    // --- Vertex buffer ---
    {
        VkDeviceSize bufferSize = sizeof(Vertex) * vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, stagingMemory);

        Buffer::createBuffer(ctx, bufferSize,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             mesh.vertexBuffer, mesh.vertexMemory);

        // Copy staging -> device-local
        VkCommandBuffer cmdBuf = cmdMgr.beginSingleTimeCommands(device);
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, mesh.vertexBuffer, 1, &copyRegion);
        cmdMgr.endSingleTimeCommands(device, ctx.getGraphicsQueue(), cmdBuf);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    // --- Index buffer ---
    {
        VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, stagingMemory);

        Buffer::createBuffer(ctx, bufferSize,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             mesh.indexBuffer, mesh.indexMemory);

        VkCommandBuffer cmdBuf = cmdMgr.beginSingleTimeCommands(device);
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, mesh.indexBuffer, 1, &copyRegion);
        cmdMgr.endSingleTimeCommands(device, ctx.getGraphicsQueue(), cmdBuf);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    m_meshes[type] = mesh;
}

} // namespace QymEngine
