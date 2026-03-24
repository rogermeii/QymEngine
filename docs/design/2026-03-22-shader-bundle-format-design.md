# Shader Bundle 二进制格式设计

## 目标

将多个 shader 变体（default, bindless 等）打包到单个二进制文件中，替代散落的 .spv + .reflect.json 文件。

## 文件格式

```
[Header] (12 bytes)
  magic: "QSHD"         (4 bytes, uint8[4])
  version: uint32        (4 bytes, 当前 = 1)
  variantCount: uint32   (4 bytes)

[Variant Table] (variantCount entries, 变长)
  每个 entry:
    nameLength: uint16   (2 bytes)
    name: char[]         (nameLength bytes, 如 "default", "bindless")
    vertSpvOffset: uint32
    vertSpvSize: uint32
    fragSpvOffset: uint32
    fragSpvSize: uint32
    reflectOffset: uint32
    reflectSize: uint32

[Data Section]
  连续的 raw bytes (SPIR-V 二进制 + reflect JSON 文本)
```

## 示例

`Lit.shaderbundle`:
```
QSHD | v1 | 2 variants
├── "default"   → vert @offset1 (3692B), frag @offset2 (4580B), reflect @offset3 (1200B)
├── "bindless"  → vert @offset4 (4096B), frag @offset5 (5120B), reflect @offset6 (1500B)
└── [data bytes...]
```

## 运行时 API

```cpp
// engine/asset/ShaderBundle.h
class ShaderBundle {
public:
    bool load(const std::string& path);  // 读整个文件到内存

    bool hasVariant(const std::string& name) const;
    std::vector<char> getVertSpv(const std::string& variant) const;
    std::vector<char> getFragSpv(const std::string& variant) const;
    std::string getReflectJson(const std::string& variant) const;

private:
    std::vector<uint8_t> m_data;
    struct VariantEntry {
        uint32_t vertOff, vertSize;
        uint32_t fragOff, fragSize;
        uint32_t reflectOff, reflectSize;
    };
    std::map<std::string, VariantEntry> m_variants;
};
```

## Shader Compiler 变化

### 之前
```
assets/shaders/
  lit_vert.spv, lit_frag.spv, Lit.reflect.json
  lit_vert_bindless.spv, lit_frag_bindless.spv, Lit_bindless.reflect.json
```

### 之后
```
assets/shaders/
  Lit.shaderbundle
```

Compiler 输出改为: 编译所有变体 → 打包为 .shaderbundle 文件。

## Pipeline 加载变化

```cpp
// 之前
auto vertCode = readFile("shaders/lit_vert.spv");
auto fragCode = readFile("shaders/lit_frag.spv");

// 之后
ShaderBundle bundle;
bundle.load("shaders/Lit.shaderbundle");
std::string variant = m_bindlessEnabled ? "bindless" : "default";
auto vertCode = bundle.getVertSpv(variant);
auto fragCode = bundle.getFragSpv(variant);
auto reflectJson = bundle.getReflectJson(variant);
```

## 扩展性

未来可以添加更多变体：
- "shadow" — shadow pass 专用（depth-only, 无 fragment shader）
- "depth_prepass" — Z prepass
- "wireframe" — 线框模式专用

每个变体只是 Variant Table 中多一个 entry。
