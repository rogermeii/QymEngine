#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
#include "renderer/VkDispatch.h"

#include <stb_image.h>
#include <json.hpp>

#include "asset/AssetManager.h"
#include "asset/ShaderBundle.h"
#include "core/FileUtils.h"
#include "renderer/VulkanContext.h"
#include "renderer/CommandManager.h"
#include "renderer/Buffer.h"
#include "renderer/Descriptor.h"
#include "renderer/Renderer.h"

#include <array>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

#ifndef __ANDROID__
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace QymEngine {

void AssetManager::init(VulkanContext& ctx, CommandManager& cmdMgr)
{
    m_ctx = &ctx;
    m_cmdMgr = &cmdMgr;
}

void AssetManager::shutdown(VkDevice device)
{
    for (auto& [key, mesh] : m_meshCache) {
        if (mesh.vertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
        if (mesh.vertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.vertexMemory, nullptr);
        if (mesh.indexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
        if (mesh.indexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.indexMemory, nullptr);
    }
    m_meshCache.clear();

    for (auto& [key, tex] : m_textureCache) {
        if (tex.sampler != VK_NULL_HANDLE)
            vkDestroySampler(device, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE)
            vkDestroyImageView(device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE)
            vkDestroyImage(device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, tex.memory, nullptr);
    }
    m_textureCache.clear();

    for (auto& [key, shader] : m_shaderCache) {
        shader.pipeline.cleanup(device);
    }
    m_shaderCache.clear();

    // Destroy material param buffers
    for (auto& [key, mat] : m_materialCache) {
        if (mat.paramBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mat.paramBuffer, nullptr);
        if (mat.paramMemory != VK_NULL_HANDLE) {
            if (mat.paramMapped)
                vkUnmapMemory(device, mat.paramMemory);
            vkFreeMemory(device, mat.paramMemory, nullptr);
        }
        // Descriptor sets are freed when the pool is destroyed
    }
    m_materialCache.clear();
}

#ifndef __ANDROID__
void AssetManager::scanAssets(const std::string& assetsDir)
{
    m_assetsDir = assetsDir;
    m_meshFiles.clear();
    m_textureFiles.clear();
    m_shaderFiles.clear();
    m_materialFiles.clear();

    if (!fs::exists(assetsDir)) return;

    for (auto& entry : fs::recursive_directory_iterator(assetsDir)) {
        if (!entry.is_regular_file()) continue;

        // Get relative path from assetsDir
        std::string relPath = fs::relative(entry.path(), assetsDir).generic_string();
        std::string ext = entry.path().extension().string();

        // Convert extension to lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c){ return std::tolower(c); });

        // Check for .mat.json and .shader.json files first (before simple extension check)
        if (relPath.find(".mat.json") != std::string::npos) {
            m_materialFiles.push_back(relPath);
        } else if (relPath.find(".shader.json") != std::string::npos) {
            m_shaderFiles.push_back(relPath);
        } else if (ext == ".obj") {
            m_meshFiles.push_back(relPath);
        } else if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
            m_textureFiles.push_back(relPath);
        }
    }
}
#endif

const MeshAsset* AssetManager::loadMesh(const std::string& relativePath)
{
    // Check cache
    auto it = m_meshCache.find(relativePath);
    if (it != m_meshCache.end())
        return &it->second;

    if (!m_ctx || !m_cmdMgr) return nullptr;

    std::string fullPath = m_assetsDir.empty() ? relativePath : (m_assetsDir + "/" + relativePath);
    if (!fileExists(fullPath)) return nullptr;

    // Load OBJ file into memory and parse with tinyobjloader via stream
    std::string objContent;
    try {
        objContent = readFileAsString(fullPath);
    } catch (...) {
        return nullptr;
    }
    std::istringstream objStream(objContent);

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &objStream))
        return nullptr;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    for (auto& shape : shapes) {
        for (auto& index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };
            vertex.color = {1.0f, 1.0f, 1.0f};
            if (index.texcoord_index >= 0) {
                vertex.texCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1] // flip Y for Vulkan
                };
            }
            if (index.normal_index >= 0) {
                vertex.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            indices.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(vertex);
        }
    }

    if (vertices.empty()) return nullptr;

    MeshAsset mesh{};
    mesh.indexCount = static_cast<uint32_t>(indices.size());

    // Compute AABB
    mesh.boundsMin = vertices[0].pos;
    mesh.boundsMax = vertices[0].pos;
    for (const auto& v : vertices) {
        mesh.boundsMin = glm::min(mesh.boundsMin, v.pos);
        mesh.boundsMax = glm::max(mesh.boundsMax, v.pos);
    }
    VkDevice device = m_ctx->getDevice();

    // --- Vertex buffer (staging upload) ---
    {
        VkDeviceSize bufferSize = sizeof(Vertex) * vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(*m_ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, stagingMemory);

        Buffer::createBuffer(*m_ctx, bufferSize,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             mesh.vertexBuffer, mesh.vertexMemory);

        VkCommandBuffer cmdBuf = m_cmdMgr->beginSingleTimeCommands(device);
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, mesh.vertexBuffer, 1, &copyRegion);
        m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), cmdBuf);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    // --- Index buffer (staging upload) ---
    {
        VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        Buffer::createBuffer(*m_ctx, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, stagingMemory);

        Buffer::createBuffer(*m_ctx, bufferSize,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             mesh.indexBuffer, mesh.indexMemory);

        VkCommandBuffer cmdBuf = m_cmdMgr->beginSingleTimeCommands(device);
        VkBufferCopy copyRegion{};
        copyRegion.size = bufferSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, mesh.indexBuffer, 1, &copyRegion);
        m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), cmdBuf);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    m_meshCache[relativePath] = mesh;
    return &m_meshCache[relativePath];
}

const TextureAsset* AssetManager::loadTexture(const std::string& relativePath)
{
    // Check cache
    auto it = m_textureCache.find(relativePath);
    if (it != m_textureCache.end())
        return &it->second;

    if (!m_ctx || !m_cmdMgr) return nullptr;

    std::string fullPath = m_assetsDir.empty() ? relativePath : (m_assetsDir + "/" + relativePath);
    auto fileData = readFileAsBytes(fullPath);
    if (fileData.empty()) return nullptr;

    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()),
                                             &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    if (!pixels) return nullptr;

    TextureAsset texAsset{};
    createTextureFromPixels(pixels, texWidth, texHeight, texAsset);
    stbi_image_free(pixels);

    // Create descriptor set for this texture (for Inspector preview)
    if (m_textureSetLayout != VK_NULL_HANDLE && m_textureDescriptorPool != VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_textureDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_textureSetLayout;

        if (vkAllocateDescriptorSets(m_ctx->getDevice(), &allocInfo, &texAsset.descriptorSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate texture descriptor set!");

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texAsset.view;
        imageInfo.sampler = texAsset.sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = texAsset.descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_ctx->getDevice(), 1, &descriptorWrite, 0, nullptr);
    }

    m_textureCache[relativePath] = texAsset;
    return &m_textureCache[relativePath];
}

const ShaderAsset* AssetManager::loadShader(const std::string& relativePath)
{
    // Check cache - but invalidate if pipeline wasn't ready when first loaded
    auto it = m_shaderCache.find(relativePath);
    if (it != m_shaderCache.end()) {
        if (it->second.pipeline.getPipeline() != VK_NULL_HANDLE)
            return &it->second;
        // Pipeline wasn't ready before (e.g. offscreen render pass not set yet), retry
        m_shaderCache.erase(it);
    }

    if (!m_ctx) return nullptr;

    std::string fullPath = m_assetsDir.empty() ? relativePath : (m_assetsDir + "/" + relativePath);

    // Read and parse JSON file via SDL_RWops
    std::string content;
    try {
        content = readFileAsString(fullPath);
    } catch (...) {
        return nullptr;
    }
    nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded()) return nullptr;

    ShaderAsset shader;
    shader.name = j.value("name", "");
    shader.bundlePath = j.value("bundle", "");

    // Parse properties
    if (j.contains("properties") && j["properties"].is_array()) {
        for (auto& prop : j["properties"]) {
            ShaderProperty sp;
            sp.name = prop.value("name", "");
            sp.type = prop.value("type", "");

            if (sp.type == "color4") {
                if (prop.contains("default") && prop["default"].is_array()) {
                    auto& arr = prop["default"];
                    sp.defaultVec = glm::vec4(
                        arr.size() > 0 ? arr[0].get<float>() : 1.0f,
                        arr.size() > 1 ? arr[1].get<float>() : 1.0f,
                        arr.size() > 2 ? arr[2].get<float>() : 1.0f,
                        arr.size() > 3 ? arr[3].get<float>() : 1.0f
                    );
                }
            } else if (sp.type == "float") {
                sp.defaultFloat = prop.value("default", 0.0f);
                if (prop.contains("range") && prop["range"].is_array()) {
                    auto& range = prop["range"];
                    sp.rangeMin = range.size() > 0 ? range[0].get<float>() : 0.0f;
                    sp.rangeMax = range.size() > 1 ? range[1].get<float>() : 1.0f;
                }
            } else if (sp.type == "texture2D") {
                sp.defaultTex = prop.value("default", "white");
            }

            shader.properties.push_back(sp);
        }
    }

    // Create pipeline from ShaderBundle
    if (m_offscreenRenderPass != VK_NULL_HANDLE && m_layoutCache && !shader.bundlePath.empty()) {
        std::string bundleFullPath = m_assetsDir.empty()
            ? shader.bundlePath
            : (m_assetsDir + "/" + shader.bundlePath);
        ShaderBundle bundle;
        std::string var = vkIsD3D12Backend() ? "default_dxil" : (vkIsD3D11Backend() ? "default_dxbc" : ((vkIsOpenGLBackend() || vkIsGLESBackend()) ? "default_glsl" : "default"));
        if (bundle.load(bundleFullPath) && bundle.hasVariant(var)) {
            auto vertSpv = bundle.getVertSpv(var);
            auto fragSpv = bundle.getFragSpv(var);
            auto reflectJson = bundle.getReflectJson(var);
            shader.pipeline.createFromMemory(
                m_ctx->getDevice(),
                m_offscreenRenderPass,
                m_offscreenExtent,
                *m_layoutCache,
                VK_POLYGON_MODE_FILL,
                vertSpv, fragSpv, reflectJson
            );
        }
    }

    m_shaderCache[relativePath] = std::move(shader);
    return &m_shaderCache[relativePath];
}

const MaterialInstance* AssetManager::loadMaterial(const std::string& relativePath)
{
    // Check cache - but invalidate if pipeline wasn't ready when first loaded
    auto it = m_materialCache.find(relativePath);
    if (it != m_materialCache.end()) {
        // Bindless: check bindlessIndex; Non-bindless: check descriptorSet
        bool isLoaded = (m_renderer && m_renderer->isBindlessEnabled())
            ? (it->second.bindlessIndex != UINT32_MAX)
            : (it->second.descriptorSet != VK_NULL_HANDLE);
        if (isLoaded)
            return &it->second;
        // Pipeline wasn't ready before, try again
        m_materialCache.erase(it);
    }

    if (!m_ctx) return nullptr;

    std::string fullPath = m_assetsDir.empty() ? relativePath : (m_assetsDir + "/" + relativePath);

    // Read and parse JSON file
    std::string content;
    try {
        content = readFileAsString(fullPath);
    } catch (...) {
        return nullptr;
    }
    nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded()) return nullptr;

    MaterialInstance mat{};
    mat.name = j.value("name", "Unnamed");
    mat.shaderPath = j.value("shader", "");

    // Load shader
    if (!mat.shaderPath.empty()) {
        mat.shader = const_cast<ShaderAsset*>(loadShader(mat.shaderPath));
    }

    // Parse properties from JSON
    if (j.contains("properties")) {
        auto& props = j["properties"];
        if (props.contains("baseColor")) {
            auto& c = props["baseColor"];
            mat.vec4Props["baseColor"] = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
        }
        if (props.contains("metallic"))
            mat.floatProps["metallic"] = props["metallic"].get<float>();
        if (props.contains("roughness"))
            mat.floatProps["roughness"] = props["roughness"].get<float>();
        if (props.contains("albedoMap"))
            mat.texturePaths["albedoMap"] = props["albedoMap"].get<std::string>();
        if (props.contains("normalMap"))
            mat.texturePaths["normalMap"] = props["normalMap"].get<std::string>();
    }

    VkDevice device = m_ctx->getDevice();

    // --- Bindless path: register textures/samplers and fill SSBO entry ---
    if (m_renderer && m_renderer->isBindlessEnabled()) {
        // Register textures into bindless arrays
        uint32_t albedoTexIdx = 0; // fallback white (registered at init as index 0)
        uint32_t normalTexIdx = 1; // fallback normal (registered at init as index 1)
        uint32_t samplerIdx = 0;   // fallback sampler (registered at init as index 0)

        // Load albedo texture
        auto albedoIt = mat.texturePaths.find("albedoMap");
        if (albedoIt != mat.texturePaths.end() && !albedoIt->second.empty()) {
            auto* tex = loadTexture(albedoIt->second);
            if (tex) {
                albedoTexIdx = m_renderer->registerBindlessTexture(tex->view);
                // Register the sampler (could reuse default, but let's register it)
                samplerIdx = m_renderer->registerBindlessSampler(tex->sampler);
            }
        }

        // Load normal texture
        auto normalIt = mat.texturePaths.find("normalMap");
        if (normalIt != mat.texturePaths.end() && !normalIt->second.empty()) {
            auto* tex = loadTexture(normalIt->second);
            if (tex) {
                normalTexIdx = m_renderer->registerBindlessTexture(tex->view);
            }
        }

        // Allocate material index in SSBO
        mat.bindlessIndex = m_renderer->allocateBindlessMaterialIndex();

        // Fill material entry
        BindlessMaterialEntry entry{};
        auto bcIt = mat.vec4Props.find("baseColor");
        entry.baseColor = bcIt != mat.vec4Props.end() ? bcIt->second : glm::vec4(1.0f);
        auto mIt = mat.floatProps.find("metallic");
        entry.metallic = mIt != mat.floatProps.end() ? mIt->second : 0.0f;
        auto rIt = mat.floatProps.find("roughness");
        entry.roughness = rIt != mat.floatProps.end() ? rIt->second : 0.5f;
        entry.albedoTexIndex = albedoTexIdx;
        entry.normalTexIndex = normalTexIdx;
        entry.samplerIndex = samplerIdx;

        m_renderer->updateBindlessMaterialEntry(mat.bindlessIndex, entry);

        // Mark descriptor set as valid (bindless doesn't need per-material sets,
        // but set a sentinel so the cache check passes)
        // Use a dummy non-null value to indicate "loaded"
        // Actually for the non-bindless fallback to still work, we should also
        // create the traditional descriptor set. But since bindless is the primary path,
        // we can skip the traditional set and just mark it as valid.
        // We set descriptorSet to a special sentinel value (reinterpret_cast hack).
        // Better approach: just set a small non-null value since it won't be used.
        mat.descriptorSet = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(1));

    } else {
        // --- Non-bindless path (existing code) ---
        if (mat.shader && m_layoutCache && m_descriptor) {
            const auto& reflection = mat.shader->pipeline.getReflection();

            auto set1It = reflection.sets.find(1);
            if (set1It != reflection.sets.end()) {
                auto bindings = reflection.buildBindings(1);
                VkDescriptorSetLayout set1Layout = m_layoutCache->getOrCreate(device, bindings);

                mat.descriptorSet = m_descriptor->allocateSet(device, set1Layout);

                uint32_t paramBufferSize = 0;
                const ReflectedBinding* uboBinding = nullptr;
                for (auto& rb : set1It->second) {
                    if (rb.type == "uniformBuffer" && rb.binding == 0) {
                        paramBufferSize = rb.size;
                        uboBinding = &rb;
                        break;
                    }
                }

                if (paramBufferSize > 0) {
                    mat.paramBufferSize = paramBufferSize;
                    Buffer::createBuffer(*m_ctx, paramBufferSize,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        mat.paramBuffer, mat.paramMemory);
                    vkMapMemory(device, mat.paramMemory, 0, paramBufferSize, 0, &mat.paramMapped);

                    memset(mat.paramMapped, 0, paramBufferSize);

                    if (uboBinding) {
                        for (auto& member : uboBinding->members) {
                            if (member.type == "float4") {
                                auto vIt = mat.vec4Props.find(member.name);
                                if (vIt != mat.vec4Props.end()) {
                                    memcpy(static_cast<char*>(mat.paramMapped) + member.offset,
                                           &vIt->second, sizeof(glm::vec4));
                                }
                            } else if (member.type == "float") {
                                auto fIt = mat.floatProps.find(member.name);
                                if (fIt != mat.floatProps.end()) {
                                    memcpy(static_cast<char*>(mat.paramMapped) + member.offset,
                                           &fIt->second, sizeof(float));
                                }
                            }
                        }
                    }

                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = mat.paramBuffer;
                    bufferInfo.offset = 0;
                    bufferInfo.range = paramBufferSize;

                    VkWriteDescriptorSet uboWrite{};
                    uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    uboWrite.dstSet = mat.descriptorSet;
                    uboWrite.dstBinding = 0;
                    uboWrite.dstArrayElement = 0;
                    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    uboWrite.descriptorCount = 1;
                    uboWrite.pBufferInfo = &bufferInfo;

                    vkUpdateDescriptorSets(device, 1, &uboWrite, 0, nullptr);
                }

                for (auto& rb : set1It->second) {
                    if (rb.type == "combinedImageSampler") {
                        VkImageView texView = m_fallbackAlbedoView;
                        VkSampler texSampler = m_fallbackAlbedoSampler;

                        bool isNormal = (rb.name.find("normal") != std::string::npos ||
                                         rb.name.find("Normal") != std::string::npos);

                        if (isNormal) {
                            texView = m_fallbackNormalView;
                            texSampler = m_fallbackNormalSampler;
                        }

                        auto tIt = mat.texturePaths.find(rb.name);
                        if (tIt != mat.texturePaths.end() && !tIt->second.empty()) {
                            auto* tex = loadTexture(tIt->second);
                            if (tex) {
                                texView = tex->view;
                                texSampler = tex->sampler;
                            }
                        }

                        VkDescriptorImageInfo imageInfo{};
                        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfo.imageView = texView;
                        imageInfo.sampler = texSampler;

                        VkWriteDescriptorSet samplerWrite{};
                        samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        samplerWrite.dstSet = mat.descriptorSet;
                        samplerWrite.dstBinding = rb.binding;
                        samplerWrite.dstArrayElement = 0;
                        samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        samplerWrite.descriptorCount = 1;
                        samplerWrite.pImageInfo = &imageInfo;

                        vkUpdateDescriptorSets(device, 1, &samplerWrite, 0, nullptr);
                    }
                }
            }
        }
    }

    m_materialCache[relativePath] = std::move(mat);
    return &m_materialCache[relativePath];
}

void AssetManager::invalidateMaterial(const std::string& relativePath)
{
    auto it = m_materialCache.find(relativePath);
    if (it == m_materialCache.end()) return;

    VkDevice device = m_ctx->getDevice();
    auto& mat = it->second;

    // Wait for GPU to finish before destroying resources
    vkDeviceWaitIdle(device);

    if (mat.paramBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, mat.paramBuffer, nullptr);
    if (mat.paramMemory != VK_NULL_HANDLE) {
        if (mat.paramMapped)
            vkUnmapMemory(device, mat.paramMemory);
        vkFreeMemory(device, mat.paramMemory, nullptr);
    }
    // Descriptor sets are freed when the pool is destroyed

    m_materialCache.erase(it);
}

bool AssetManager::materialFileExists(const std::string& relativePath) const
{
    for (auto& f : m_materialFiles) {
        if (f == relativePath) return true;
    }
    return false;
}

void AssetManager::invalidateAllShadersAndMaterials()
{
    if (!m_ctx) return;
    VkDevice device = m_ctx->getDevice();
    vkDeviceWaitIdle(device);

    // Destroy all material param buffers
    for (auto& [path, mat] : m_materialCache) {
        if (mat.paramBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mat.paramBuffer, nullptr);
        if (mat.paramMemory != VK_NULL_HANDLE) {
            if (mat.paramMapped)
                vkUnmapMemory(device, mat.paramMemory);
            vkFreeMemory(device, mat.paramMemory, nullptr);
        }
    }
    m_materialCache.clear();

    // Destroy all shader pipelines
    for (auto& [path, shader] : m_shaderCache) {
        shader.pipeline.cleanup(device);
    }
    m_shaderCache.clear();
}

// ---------------------------------------------------------------------------
// Private helpers (mirroring Texture.cpp patterns)
// ---------------------------------------------------------------------------

void AssetManager::createTextureFromPixels(const unsigned char* pixels, int width, int height,
                                            TextureAsset& outAsset)
{
    VkDevice device = m_ctx->getDevice();
    VkDeviceSize imageSize = width * height * 4;

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    Buffer::createBuffer(*m_ctx, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stagingBuffer, stagingMemory);

    void* data;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(device, stagingMemory);

    // Create image
    createImage(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                outAsset.image, outAsset.memory);

    transitionImageLayout(outAsset.image, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer, outAsset.image,
                      static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    transitionImageLayout(outAsset.image, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Image view
    outAsset.view = createImageView(outAsset.image, VK_FORMAT_R8G8B8A8_SRGB);

    // Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_ctx->getPhysicalDevice(), &properties);
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &outAsset.sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler!");
}

void AssetManager::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageTiling tiling, VkImageUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkImage& image, VkDeviceMemory& imageMemory)
{
    VkDevice device = m_ctx->getDevice();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
        throw std::runtime_error("failed to create image!");

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = m_ctx->findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate image memory!");

    vkBindImageMemory(device, image, imageMemory, 0);
}

VkImageView AssetManager::createImageView(VkImage image, VkFormat format)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(m_ctx->getDevice(), &viewInfo, nullptr, &imageView) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture image view!");

    return imageView;
}

void AssetManager::transitionImageLayout(VkImage image, VkFormat format,
                                          VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkDevice device = m_ctx->getDevice();
    VkCommandBuffer commandBuffer = m_cmdMgr->beginSingleTimeCommands(device);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), commandBuffer);
}

void AssetManager::copyBufferToImage(VkBuffer buffer, VkImage image,
                                      uint32_t width, uint32_t height)
{
    VkDevice device = m_ctx->getDevice();
    VkCommandBuffer commandBuffer = m_cmdMgr->beginSingleTimeCommands(device);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    m_cmdMgr->endSingleTimeCommands(device, m_ctx->getGraphicsQueue(), commandBuffer);
}

} // namespace QymEngine
