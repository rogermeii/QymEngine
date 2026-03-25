#include "asset/ShaderBundle.h"
#include <SDL.h>
#include <fstream>
#include <cstring>
#include <iostream>

namespace QymEngine {

bool ShaderBundle::load(const std::string& path)
{
    // 使用 SDL_RWops 读取文件（兼容 Android assets）
    // Android AAssetManager 需要纯相对路径，去掉前导 "./" 或 "/"
    std::string cleanPath = path;
#ifdef __ANDROID__
    if (cleanPath.size() >= 2 && cleanPath[0] == '.' && (cleanPath[1] == '/' || cleanPath[1] == '\\'))
        cleanPath = cleanPath.substr(2);
    else if (!cleanPath.empty() && (cleanPath[0] == '/' || cleanPath[0] == '\\'))
        cleanPath = cleanPath.substr(1);
#endif
    SDL_RWops* rw = SDL_RWFromFile(cleanPath.c_str(), "rb");
    if (!rw) {
        std::cerr << "ShaderBundle: 无法打开文件 " << path << std::endl;
        return false;
    }

    Sint64 fileSize = SDL_RWsize(rw);
    if (fileSize < 12) {
        SDL_RWclose(rw);
        std::cerr << "ShaderBundle: 文件太小 " << path << std::endl;
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    SDL_RWread(rw, data.data(), 1, data.size());
    SDL_RWclose(rw);

    return loadFromMemory(data.data(), data.size());
}

bool ShaderBundle::loadFromMemory(const uint8_t* data, size_t size)
{
    if (size < 12) return false;

    // 验证魔数
    if (memcmp(data, MAGIC, 4) != 0) {
        std::cerr << "ShaderBundle: 魔数不匹配" << std::endl;
        return false;
    }

    // 读取版本和变体数量
    uint32_t version, variantCount;
    memcpy(&version, data + 4, 4);
    memcpy(&variantCount, data + 8, 4);

    if (version != CURRENT_VERSION) {
        std::cerr << "ShaderBundle: 不支持的版本 " << version << std::endl;
        return false;
    }

    // 保存原始数据
    m_data.assign(data, data + size);
    m_variants.clear();

    // 解析变体表
    size_t offset = 12;
    for (uint32_t i = 0; i < variantCount; i++) {
        if (offset + 2 > size) return false;

        uint16_t nameLen;
        memcpy(&nameLen, m_data.data() + offset, 2);
        offset += 2;

        if (offset + nameLen + 24 > size) return false;

        std::string name(reinterpret_cast<const char*>(m_data.data() + offset), nameLen);
        offset += nameLen;

        VariantEntry entry;
        memcpy(&entry.vertOff,    m_data.data() + offset, 4); offset += 4;
        memcpy(&entry.vertSize,   m_data.data() + offset, 4); offset += 4;
        memcpy(&entry.fragOff,    m_data.data() + offset, 4); offset += 4;
        memcpy(&entry.fragSize,   m_data.data() + offset, 4); offset += 4;
        memcpy(&entry.reflectOff, m_data.data() + offset, 4); offset += 4;
        memcpy(&entry.reflectSize,m_data.data() + offset, 4); offset += 4;

        m_variants[name] = entry;
    }

    return true;
}

bool ShaderBundle::hasVariant(const std::string& name) const
{
    return m_variants.count(name) > 0;
}

std::vector<std::string> ShaderBundle::getVariantNames() const
{
    std::vector<std::string> names;
    for (auto& [name, _] : m_variants)
        names.push_back(name);
    return names;
}

std::vector<char> ShaderBundle::getVertSpv(const std::string& variant) const
{
    auto it = m_variants.find(variant);
    if (it == m_variants.end()) return {};
    auto& e = it->second;
    if (e.vertOff + e.vertSize > m_data.size()) return {};
    return std::vector<char>(m_data.begin() + e.vertOff, m_data.begin() + e.vertOff + e.vertSize);
}

std::vector<char> ShaderBundle::getFragSpv(const std::string& variant) const
{
    auto it = m_variants.find(variant);
    if (it == m_variants.end()) return {};
    auto& e = it->second;
    if (e.fragOff + e.fragSize > m_data.size()) return {};
    return std::vector<char>(m_data.begin() + e.fragOff, m_data.begin() + e.fragOff + e.fragSize);
}

std::string ShaderBundle::getReflectJson(const std::string& variant) const
{
    auto it = m_variants.find(variant);
    if (it == m_variants.end()) return {};
    auto& e = it->second;
    if (e.reflectOff + e.reflectSize > m_data.size()) return {};
    return std::string(m_data.begin() + e.reflectOff, m_data.begin() + e.reflectOff + e.reflectSize);
}

} // namespace QymEngine
