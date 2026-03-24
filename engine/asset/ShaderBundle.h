#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace QymEngine {

/// .shaderbundle 二进制文件的运行时加载器
/// 格式: [Header(12B)] [VariantTable(变长)] [DataSection(原始字节)]
class ShaderBundle {
public:
    /// 从文件加载整个 .shaderbundle
    bool load(const std::string& path);

    /// 从内存加载（用于嵌入式场景）
    bool loadFromMemory(const uint8_t* data, size_t size);

    /// 是否包含指定变体
    bool hasVariant(const std::string& name) const;

    /// 获取变体列表
    std::vector<std::string> getVariantNames() const;

    /// 获取顶点着色器 SPIR-V 字节码
    std::vector<char> getVertSpv(const std::string& variant) const;

    /// 获取片段着色器 SPIR-V 字节码
    std::vector<char> getFragSpv(const std::string& variant) const;

    /// 获取反射 JSON 字符串
    std::string getReflectJson(const std::string& variant) const;

    /// 文件头魔数
    static constexpr uint8_t MAGIC[4] = {'Q', 'S', 'H', 'D'};
    static constexpr uint32_t CURRENT_VERSION = 1;

private:
    std::vector<uint8_t> m_data;

    struct VariantEntry {
        uint32_t vertOff, vertSize;
        uint32_t fragOff, fragSize;
        uint32_t reflectOff, reflectSize;
    };
    std::map<std::string, VariantEntry> m_variants;
};

} // namespace QymEngine
