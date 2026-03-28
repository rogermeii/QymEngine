#include <slang.h>
#include <slang-com-ptr.h>
#include <spirv_reflect.h>
#include <json.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstring>

namespace fs = std::filesystem;
using json = nlohmann::json;

// 全局选项
static bool g_emitDebugInfo = false;  // 默认关闭调试符号（避免生成 DebugFunctionDefinition 等扩展指令导致 spirv-val 报错）
static bool g_emitDxil = true;        // 默认编译 DXIL 变体（D3D12 后端用）
static bool g_emitDxbc = true;        // 默认编译 DXBC/SM5.0 变体（D3D11 后端用）
static bool g_emitGlsl = true;        // 默认编译 GLSL 变体（OpenGL 后端用）
static bool g_emitMsl = true;        // 默认编译 MSL 变体（Metal 后端用）

static std::string memberTypeStr(const SpvReflectBlockVariable& member) {
    if (!member.type_description) return "unknown";
    auto* td = member.type_description;
    auto flags = td->type_flags;

    // Slang wraps matrices in structs named "_MatrixStorage_<type>std140/std430".
    // Detect this pattern and extract the matrix type from the type_name.
    if (td->type_name) {
        std::string typeName(td->type_name);
        const std::string prefix = "_MatrixStorage_";
        if (typeName.find(prefix) == 0) {
            std::string rest = typeName.substr(prefix.size());
            auto pos = rest.find("std");
            if (pos != std::string::npos)
                rest = rest.substr(0, pos);
            return rest;
        }
    }

    bool isMatrix = (flags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0;
    bool isVector = (flags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0;
    bool isFloat = (flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0;
    bool isInt = (flags & SPV_REFLECT_TYPE_FLAG_INT) != 0;

    std::string base = isFloat ? "float" : (isInt ? "int" : "unknown");

    if (isMatrix) {
        uint32_t cols = td->traits.numeric.matrix.column_count;
        uint32_t rows = td->traits.numeric.matrix.row_count;
        return base + std::to_string(cols) + "x" + std::to_string(rows);
    }
    if (isVector) {
        uint32_t components = td->traits.numeric.vector.component_count;
        return base + std::to_string(components);
    }
    return base;
}

static std::string descriptorTypeStr(SpvReflectDescriptorType type) {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniformBuffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combinedImageSampler";
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampledImage";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storageBuffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
    default: return "unknown";
    }
}

// Reflect SPIR-V binaries and merge into a single JSON
static json reflectSpirv(const std::vector<uint8_t>& vertSpv, const std::vector<uint8_t>& fragSpv) {
    // set -> binding -> {type, name, stages}
    struct BindingInfo {
        uint32_t binding;
        std::string type;
        std::string name;
        std::set<std::string> stages;
        uint32_t size = 0;
        json members = json::array();
    };
    std::map<uint32_t, std::map<uint32_t, BindingInfo>> setMap;

    struct PcInfo {
        uint32_t offset, size;
        std::set<std::string> stages;
        json members = json::array();
    };
    std::vector<PcInfo> pcList;

    auto reflectStage = [&](const std::vector<uint8_t>& spv, const std::string& stageName) {
        SpvReflectShaderModule module;
        if (spvReflectCreateShaderModule(spv.size(), spv.data(), &module) != SPV_REFLECT_RESULT_SUCCESS)
            return;

        // Descriptor bindings
        uint32_t count = 0;
        spvReflectEnumerateDescriptorBindings(&module, &count, nullptr);
        std::vector<SpvReflectDescriptorBinding*> bindings(count);
        spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data());

        for (auto* b : bindings) {
            auto& info = setMap[b->set][b->binding];
            info.binding = b->binding;
            info.type = descriptorTypeStr(b->descriptor_type);
            if (b->name && info.name.empty()) info.name = b->name;
            info.stages.insert(stageName);

            if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                b->block.member_count > 0 && info.members.empty()) {
                info.size = b->block.size;
                for (uint32_t m = 0; m < b->block.member_count; m++) {
                    auto& mem = b->block.members[m];
                    json mj;
                    mj["name"] = mem.name ? mem.name : "";
                    mj["type"] = memberTypeStr(mem);
                    mj["offset"] = mem.offset;
                    mj["size"] = mem.size;
                    info.members.push_back(mj);
                }
            }
        }

        // Push constants
        uint32_t pcCount = 0;
        spvReflectEnumeratePushConstantBlocks(&module, &pcCount, nullptr);
        std::vector<SpvReflectBlockVariable*> pcBlocks(pcCount);
        spvReflectEnumeratePushConstantBlocks(&module, &pcCount, pcBlocks.data());

        for (auto* pc : pcBlocks) {
            bool found = false;
            for (auto& existing : pcList) {
                if (existing.offset == pc->offset && existing.size == pc->size) {
                    existing.stages.insert(stageName);
                    found = true;
                    break;
                }
            }
            if (!found) {
                json pcMembers = json::array();
                for (uint32_t m = 0; m < pc->member_count; m++) {
                    auto& mem = pc->members[m];
                    json mj;
                    mj["name"] = mem.name ? mem.name : "";
                    mj["type"] = memberTypeStr(mem);
                    mj["offset"] = mem.offset;
                    mj["size"] = mem.size;
                    pcMembers.push_back(mj);
                }
                pcList.push_back({pc->offset, pc->size, {stageName}, pcMembers});
            }
        }

        spvReflectDestroyShaderModule(&module);
    };

    reflectStage(vertSpv, "vertex");
    reflectStage(fragSpv, "fragment");

    // Build JSON
    json result;
    json setsJson = json::array();
    for (auto& [setIdx, bindings] : setMap) {
        json setJson;
        setJson["set"] = setIdx;
        setJson["bindings"] = json::array();
        for (auto& [_, info] : bindings) {
            json b;
            b["binding"] = info.binding;
            b["name"] = info.name;
            b["type"] = info.type;
            b["stages"] = json::array();
            for (auto& s : info.stages) b["stages"].push_back(s);
            if (info.size > 0) b["size"] = info.size;
            if (!info.members.empty()) b["members"] = info.members;
            setJson["bindings"].push_back(b);
        }
        setsJson.push_back(setJson);
    }
    result["sets"] = setsJson;

    json pcJson = json::array();
    for (auto& pc : pcList) {
        json p;
        p["offset"] = pc.offset;
        p["size"] = pc.size;
        p["stages"] = json::array();
        for (auto& s : pc.stages) p["stages"].push_back(s);
        if (!pc.members.empty()) p["members"] = pc.members;
        pcJson.push_back(p);
    }
    result["pushConstants"] = pcJson;

    return result;
}

// 单个变体的编译结果
struct VariantResult {
    std::vector<uint8_t> vertSpv;
    std::vector<uint8_t> fragSpv;
    std::string reflectJson;
};

// 编译单个变体，返回编译结果而不直接写文件
// targetFormat: SLANG_SPIRV 或 SLANG_DXIL
// forceNoDebug: true = 强制禁用调试信息 (用于 OpenGL SPIR-V 兼容性)
static bool compileShaderVariant(const std::string& inputPath,
                                  const std::string& baseName,
                                  const std::vector<slang::PreprocessorMacroDesc>& macros,
                                  VariantResult& result,
                                  SlangCompileTarget targetFormat = SLANG_SPIRV,
                                  bool forceNoDebug = false) {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    slang_createGlobalSession(SLANG_API_VERSION, globalSession.writeRef());
    if (!globalSession) {
        std::cerr << "Failed to create Slang global session" << std::endl;
        return false;
    }

    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};
    targetDesc.format = targetFormat;

    if (targetFormat == SLANG_SPIRV) {
        targetDesc.profile = globalSession->findProfile("glsl_450");
        if (g_emitDebugInfo && !forceNoDebug)
            targetDesc.flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY;
    } else if (targetFormat == SLANG_DXIL) {
        targetDesc.profile = globalSession->findProfile("sm_6_0");
    } else if (targetFormat == SLANG_DXBC) {
        targetDesc.profile = globalSession->findProfile("sm_5_0");
    } else if (targetFormat == SLANG_GLSL) {
        targetDesc.profile = globalSession->findProfile("glsl_450");
        // Slang API 的 SessionDesc 默认是 row-major。
        // 对 OpenGL/GLES 的 GLSL 变体，这会和当前引擎的矩阵上传/乘法约定打架，
        // 导致场景 shader 里经 _MatrixStorage + unpackStorage 生成的矩阵链在 Adreno 上失效。
        // 这里显式切到 column-major，与 glslc/slangc 的历史默认和 GLM 主序更一致。
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    } else if (targetFormat == SLANG_METAL) {
        // MSL 不需要额外的 profile 设置，targetDesc.format 已统一赋值为 SLANG_METAL
    }

    sessionDesc.targetCount = 1;
    sessionDesc.targets = &targetDesc;

    // 设置预处理器宏
    sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    sessionDesc.preprocessorMacros = macros.empty() ? nullptr : macros.data();

    // 设置编译选项：调试信息
    std::vector<slang::CompilerOptionEntry> options;
    if (g_emitDebugInfo && !forceNoDebug) {
        slang::CompilerOptionEntry debugOpt = {};
        debugOpt.name = slang::CompilerOptionName::DebugInformation;
        debugOpt.value.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD;
        options.push_back(debugOpt);
    }
    if (targetFormat == SLANG_GLSL) {
        slang::CompilerOptionEntry matrixLayoutOpt = {};
        matrixLayoutOpt.name = slang::CompilerOptionName::MatrixLayoutColumn;
        matrixLayoutOpt.value.intValue0 = 1;
        options.push_back(matrixLayoutOpt);
    }
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());
    sessionDesc.compilerOptionEntries = options.empty() ? nullptr : options.data();

    Slang::ComPtr<slang::ISession> session;
    globalSession->createSession(sessionDesc, session.writeRef());
    if (!session) {
        std::cerr << "Failed to create Slang session" << std::endl;
        return false;
    }

    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModule(inputPath.c_str(), diagnostics.writeRef());
    if (!module) {
        if (diagnostics)
            std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
        return false;
    }

    // 查找入口点
    struct EntryInfo { std::string name; std::string suffix; };
    std::vector<EntryInfo> entries;

    int epCount = module->getDefinedEntryPointCount();
    for (int i = 0; i < epCount; i++) {
        Slang::ComPtr<slang::IEntryPoint> ep;
        module->getDefinedEntryPoint(i, ep.writeRef());
        if (!ep) continue;
        auto* funcReflection = ep->getFunctionReflection();
        if (!funcReflection) continue;

        std::string epName = funcReflection->getName();
        if (epName.find("vertex") != std::string::npos || epName.find("Vertex") != std::string::npos)
            entries.push_back({epName, "_vert"});
        else if (epName.find("fragment") != std::string::npos || epName.find("Fragment") != std::string::npos)
            entries.push_back({epName, "_frag"});
    }

    if (entries.empty()) {
        std::cerr << "No entry points found in " << inputPath << std::endl;
        return false;
    }

    // 编译入口点
    std::map<std::string, std::vector<uint8_t>> spvOutputs;

    if (targetFormat == SLANG_GLSL && entries.size() >= 2) {
        // GLSL: 所有入口点一起链接，确保 struct 命名一致（避免 link 时 field mismatch）
        std::vector<slang::IComponentType*> components = { module };
        std::vector<Slang::ComPtr<slang::IEntryPoint>> epPtrs;
        for (auto& entry : entries) {
            Slang::ComPtr<slang::IEntryPoint> ep;
            module->findEntryPointByName(entry.name.c_str(), ep.writeRef());
            if (!ep) { std::cerr << "Entry point not found: " << entry.name << std::endl; return false; }
            epPtrs.push_back(ep);
            components.push_back(ep.get());
        }

        Slang::ComPtr<slang::IComponentType> composedProgram;
        session->createCompositeComponentType(
            components.data(), (SlangInt)components.size(),
            composedProgram.writeRef(), diagnostics.writeRef());
        if (!composedProgram) {
            if (diagnostics) std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
            return false;
        }

        Slang::ComPtr<slang::IComponentType> linkedProgram;
        composedProgram->link(linkedProgram.writeRef(), diagnostics.writeRef());
        if (!linkedProgram) {
            if (diagnostics) std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
            return false;
        }

        // 分别导出每个入口点的代码
        for (size_t i = 0; i < entries.size(); i++) {
            Slang::ComPtr<slang::IBlob> code;
            linkedProgram->getEntryPointCode((SlangInt)i, 0, code.writeRef(), diagnostics.writeRef());
            if (!code) {
                if (diagnostics) std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
                return false;
            }
            auto* ptr = (const uint8_t*)code->getBufferPointer();
            spvOutputs[entries[i].suffix] = std::vector<uint8_t>(ptr, ptr + code->getBufferSize());
        }
    } else {
        // 非 GLSL（SPIRV/DXIL/DXBC）: 每个入口点单独编译
        for (auto& entry : entries) {
            Slang::ComPtr<slang::IEntryPoint> entryPoint;
            module->findEntryPointByName(entry.name.c_str(), entryPoint.writeRef());
            if (!entryPoint) {
                std::cerr << "Entry point not found: " << entry.name << std::endl;
                return false;
            }

            std::vector<slang::IComponentType*> components = { module, entryPoint };
            Slang::ComPtr<slang::IComponentType> composedProgram;
            session->createCompositeComponentType(
                components.data(), (SlangInt)components.size(),
                composedProgram.writeRef(), diagnostics.writeRef());
            if (!composedProgram) {
                if (diagnostics) std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
                return false;
            }

            Slang::ComPtr<slang::IComponentType> linkedProgram;
            composedProgram->link(linkedProgram.writeRef(), diagnostics.writeRef());
            if (!linkedProgram) {
                if (diagnostics) std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
                return false;
            }

            Slang::ComPtr<slang::IBlob> spirvCode;
            linkedProgram->getEntryPointCode(0, 0, spirvCode.writeRef(), diagnostics.writeRef());
            if (!spirvCode) {
                if (diagnostics) std::cerr << (const char*)diagnostics->getBufferPointer() << std::endl;
                return false;
            }

            auto* ptr = (const uint8_t*)spirvCode->getBufferPointer();
            spvOutputs[entry.suffix] = std::vector<uint8_t>(ptr, ptr + spirvCode->getBufferSize());
        }
    }

    // GLSL 后处理: 规范化 Slang 生成的 struct 字段名后缀
    // Slang 为不同入口点生成 data_0, data_1, data_2 等不同后缀，
    // 导致 OpenGL link 时 "struct fields mismatch" 错误。
    // 统一将 data_N 替换为 data_0。
    if (targetFormat == SLANG_GLSL) {
        auto normalizeGlsl = [](std::vector<uint8_t>& code) {
            std::string src(code.begin(), code.end());
            // 替换所有 data_N (N>0) 为 data_0
            for (int n = 9; n >= 1; n--) {
                std::string from = "data_" + std::to_string(n);
                std::string to = "data_0";
                size_t pos = 0;
                while ((pos = src.find(from, pos)) != std::string::npos) {
                    src.replace(pos, from.length(), to);
                    pos += to.length();
                }
            }
            code.assign(src.begin(), src.end());
        };
        for (auto& [suffix, code] : spvOutputs)
            normalizeGlsl(code);
    }

    // 保存编译结果
    if (spvOutputs.count("_vert")) result.vertSpv = spvOutputs["_vert"];
    if (spvOutputs.count("_frag")) result.fragSpv = spvOutputs["_frag"];

    // 反射 (仅 SPIRV 目标需要，DXIL 复用 SPIRV 的反射数据)
    if (targetFormat == SLANG_SPIRV && !result.vertSpv.empty() && !result.fragSpv.empty()) {
        json reflectJson = reflectSpirv(result.vertSpv, result.fragSpv);
        result.reflectJson = reflectJson.dump(2);
    }

    return true;
}

// .shaderbundle 文件头魔数
static constexpr uint8_t BUNDLE_MAGIC[4] = {'Q', 'S', 'H', 'D'};
static constexpr uint32_t BUNDLE_VERSION = 1;

// 写入 .shaderbundle 文件
static bool writeShaderBundle(const std::string& outputPath,
                               const std::map<std::string, VariantResult>& variants) {
    // 计算变体表大小
    size_t tableSize = 0;
    for (auto& [name, _] : variants) {
        tableSize += 2 + name.size() + 24; // nameLen(2) + name + 6*uint32(24)
    }

    // 数据段起始偏移 = header(12) + table
    uint32_t dataOffset = 12 + static_cast<uint32_t>(tableSize);

    // 收集数据段
    std::vector<uint8_t> dataSection;
    struct VariantOffsets {
        uint32_t vertOff, vertSize;
        uint32_t fragOff, fragSize;
        uint32_t reflectOff, reflectSize;
    };
    std::map<std::string, VariantOffsets> offsets;

    for (auto& [name, variant] : variants) {
        VariantOffsets vo;
        vo.vertOff = dataOffset + static_cast<uint32_t>(dataSection.size());
        vo.vertSize = static_cast<uint32_t>(variant.vertSpv.size());
        dataSection.insert(dataSection.end(), variant.vertSpv.begin(), variant.vertSpv.end());

        vo.fragOff = dataOffset + static_cast<uint32_t>(dataSection.size());
        vo.fragSize = static_cast<uint32_t>(variant.fragSpv.size());
        dataSection.insert(dataSection.end(), variant.fragSpv.begin(), variant.fragSpv.end());

        vo.reflectOff = dataOffset + static_cast<uint32_t>(dataSection.size());
        vo.reflectSize = static_cast<uint32_t>(variant.reflectJson.size());
        dataSection.insert(dataSection.end(), variant.reflectJson.begin(), variant.reflectJson.end());

        offsets[name] = vo;
    }

    // 写入文件
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to create bundle: " << outputPath << std::endl;
        return false;
    }

    // Header
    out.write(reinterpret_cast<const char*>(BUNDLE_MAGIC), 4);
    uint32_t version = BUNDLE_VERSION;
    uint32_t variantCount = static_cast<uint32_t>(variants.size());
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(reinterpret_cast<const char*>(&variantCount), 4);

    // Variant table
    for (auto& [name, vo] : offsets) {
        uint16_t nameLen = static_cast<uint16_t>(name.size());
        out.write(reinterpret_cast<const char*>(&nameLen), 2);
        out.write(name.data(), nameLen);
        out.write(reinterpret_cast<const char*>(&vo.vertOff), 4);
        out.write(reinterpret_cast<const char*>(&vo.vertSize), 4);
        out.write(reinterpret_cast<const char*>(&vo.fragOff), 4);
        out.write(reinterpret_cast<const char*>(&vo.fragSize), 4);
        out.write(reinterpret_cast<const char*>(&vo.reflectOff), 4);
        out.write(reinterpret_cast<const char*>(&vo.reflectSize), 4);
    }

    // Data section
    out.write(reinterpret_cast<const char*>(dataSection.data()), dataSection.size());
    out.close();

    return true;
}

bool compileShader(const std::string& inputPath, const std::string& outputDir) {
    std::string baseName = fs::path(inputPath).stem().string();
    std::cout << "Compiling: " << baseName << ".slang"
              << (g_emitDebugInfo ? " [debug]" : "") << std::endl;

    std::map<std::string, VariantResult> variants;

    // 1. 默认变体
    VariantResult defaultResult;
    if (!compileShaderVariant(inputPath, baseName, {}, defaultResult))
        return false;
    std::cout << "  vert: " << defaultResult.vertSpv.size() << "B, frag: "
              << defaultResult.fragSpv.size() << "B" << std::endl;
    variants["default"] = std::move(defaultResult);

    // 2. Bindless 变体 (Grid 没有 set 1，跳过)
    if (baseName != "Grid") {
        std::cout << "  [bindless variant]" << std::endl;
        slang::PreprocessorMacroDesc bindlessMacro;
        bindlessMacro.name = "USE_BINDLESS";
        bindlessMacro.value = "1";
        std::vector<slang::PreprocessorMacroDesc> macros = { bindlessMacro };

        VariantResult bindlessResult;
        if (compileShaderVariant(inputPath, baseName, macros, bindlessResult)) {
            std::cout << "  vert: " << bindlessResult.vertSpv.size() << "B, frag: "
                      << bindlessResult.fragSpv.size() << "B" << std::endl;
            variants["bindless"] = std::move(bindlessResult);
        } else {
            std::cerr << "  WARNING: Bindless variant failed for " << baseName << ", continuing..." << std::endl;
        }
    }

    // DX 后端的 NDC Y 方向与 Vulkan 相反，全屏 quad UV 需要翻转
    slang::PreprocessorMacroDesc flipYMacro;
    flipYMacro.name = "GRAPHICS_FLIP_Y";
    flipYMacro.value = "1";

    // 3. DXIL 变体 (D3D12 后端用)
    if (g_emitDxil) {
        std::cout << "  [dxil default]" << std::endl;
        VariantResult dxilDefault;
        if (compileShaderVariant(inputPath, baseName, {flipYMacro}, dxilDefault, SLANG_DXIL)) {
            // DXIL 变体复用 SPIRV 的反射数据
            dxilDefault.reflectJson = variants["default"].reflectJson;
            std::cout << "  vert: " << dxilDefault.vertSpv.size() << "B, frag: "
                      << dxilDefault.fragSpv.size() << "B (DXIL)" << std::endl;
            variants["default_dxil"] = std::move(dxilDefault);
        } else {
            std::cerr << "  WARNING: DXIL default variant failed for " << baseName << std::endl;
        }

        if (baseName != "Grid" && variants.count("bindless")) {
            std::cout << "  [dxil bindless]" << std::endl;
            slang::PreprocessorMacroDesc bindlessMacro;
            bindlessMacro.name = "USE_BINDLESS";
            bindlessMacro.value = "1";
            std::vector<slang::PreprocessorMacroDesc> macros = { bindlessMacro, flipYMacro };
            VariantResult dxilBindless;
            if (compileShaderVariant(inputPath, baseName, macros, dxilBindless, SLANG_DXIL)) {
                dxilBindless.reflectJson = variants["bindless"].reflectJson;
                std::cout << "  vert: " << dxilBindless.vertSpv.size() << "B, frag: "
                          << dxilBindless.fragSpv.size() << "B (DXIL)" << std::endl;
                variants["bindless_dxil"] = std::move(dxilBindless);
            } else {
                std::cerr << "  WARNING: DXIL bindless variant failed for " << baseName << std::endl;
            }
        }
    }

    // 4. DXBC/SM5.0 变体 (D3D11 后端用)
    if (g_emitDxbc) {
        std::cout << "  [dxbc default]" << std::endl;
        VariantResult dxbcDefault;
        if (compileShaderVariant(inputPath, baseName, {flipYMacro}, dxbcDefault, SLANG_DXBC)) {
            dxbcDefault.reflectJson = variants["default"].reflectJson;
            std::cout << "  vert: " << dxbcDefault.vertSpv.size() << "B, frag: "
                      << dxbcDefault.fragSpv.size() << "B (DXBC)" << std::endl;
            variants["default_dxbc"] = std::move(dxbcDefault);
        } else {
            std::cerr << "  WARNING: DXBC default variant failed for " << baseName << std::endl;
        }
    }

    // 5. GLSL 450 文本变体 (OpenGL 后端用)
    if (g_emitGlsl) {
        std::cout << "  [glsl default]" << std::endl;
        VariantResult glslDefault;
        if (compileShaderVariant(inputPath, baseName, {}, glslDefault, SLANG_GLSL)) {
            glslDefault.reflectJson = variants["default"].reflectJson;
            std::cout << "  vert: " << glslDefault.vertSpv.size() << "B, frag: "
                      << glslDefault.fragSpv.size() << "B (GLSL)" << std::endl;
            variants["default_glsl"] = std::move(glslDefault);
        } else {
            std::cerr << "  WARNING: OpenGL SPIR-V variant failed for " << baseName << std::endl;
        }
    }

    // 5b. GLSL GLES shadow 变体：Shadow shader 的 SHADOW_COLOR_OUTPUT 宏版本
    if (g_emitGlsl && baseName == "Shadow") {
        std::cout << "  [glsl gles_shadow]" << std::endl;
        slang::PreprocessorMacroDesc shadowMacro;
        shadowMacro.name = "SHADOW_COLOR_OUTPUT";
        shadowMacro.value = "1";
        std::vector<slang::PreprocessorMacroDesc> shadowMacros = { shadowMacro };
        VariantResult glesShadow;
        if (compileShaderVariant(inputPath, baseName, shadowMacros, glesShadow, SLANG_GLSL)) {
            glesShadow.reflectJson = variants["default"].reflectJson;
            std::cout << "  vert: " << glesShadow.vertSpv.size() << "B, frag: "
                      << glesShadow.fragSpv.size() << "B (GLSL GLES shadow)" << std::endl;
            variants["gles_shadow"] = std::move(glesShadow);
        } else {
            std::cerr << "  WARNING: GLES shadow variant failed" << std::endl;
        }
    }

    // 6. MSL 变体 (Metal 后端用)
    if (g_emitMsl) {
        std::cout << "  [msl default]" << std::endl;
        // Metal 编译传入 __METAL__ 宏，着色器可用于平台条件编译
        slang::PreprocessorMacroDesc metalMacro;
        metalMacro.name = "__METAL__";
        metalMacro.value = "1";
        std::vector<slang::PreprocessorMacroDesc> mslMacros = { metalMacro };
        VariantResult mslDefault;
        if (compileShaderVariant(inputPath, baseName, mslMacros, mslDefault, SLANG_METAL)) {
            mslDefault.reflectJson = variants["default"].reflectJson;
            std::cout << "  vert: " << mslDefault.vertSpv.size() << "B, frag: "
                      << mslDefault.fragSpv.size() << "B (MSL)" << std::endl;
            variants["default_msl"] = std::move(mslDefault);
        } else {
            std::cerr << "  WARNING: MSL variant failed for " << baseName << std::endl;
        }
    }

    // 打包为 .shaderbundle（唯一输出格式）
    std::string bundlePath = outputDir + "/" + baseName + ".shaderbundle";
    if (writeShaderBundle(bundlePath, variants)) {
        std::cout << "  => " << baseName << ".shaderbundle" << std::endl;
    } else {
        std::cerr << "  ERROR: Failed to write bundle for " << baseName << std::endl;
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    std::string inputDir = "assets/shaders";
    std::string outputDir = "assets/shaders";

    // 解析命令行参数
    // 用法: ShaderCompiler [inputDir] [outputDir] [--no-debug]
    std::vector<std::string> positionalArgs;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-debug" || arg == "-O") {
            g_emitDebugInfo = false;
        } else if (arg == "--no-dxbc") {
            g_emitDxbc = false;
        } else if (arg == "--no-dxil") {
            g_emitDxil = false;
        } else if (arg == "--no-glsl") {
            g_emitGlsl = false;
        } else if (arg == "--no-msl") {
            g_emitMsl = false;
        } else {
            positionalArgs.push_back(arg);
        }
    }
    if (positionalArgs.size() >= 1) inputDir = positionalArgs[0];
    if (positionalArgs.size() >= 2) outputDir = positionalArgs[1];

    std::cout << "=== QymEngine Shader Compiler ===" << std::endl;
    std::cout << "Input:  " << inputDir << std::endl;
    std::cout << "Output: " << outputDir << std::endl;
    std::cout << "Debug:  " << (g_emitDebugInfo ? "ON" : "OFF") << std::endl;
    std::cout << "DXIL:   " << (g_emitDxil ? "ON" : "OFF") << std::endl;
    std::cout << "MSL:    " << (g_emitMsl ? "ON" : "OFF") << std::endl;

    int compiled = 0, failed = 0;
    for (auto& entry : fs::recursive_directory_iterator(inputDir)) {
        if (entry.path().extension() == ".slang") {
            // 输出目录与源文件同级（保持子目录结构）
            std::string shaderOutDir = entry.path().parent_path().string();
            if (compileShader(entry.path().string(), shaderOutDir))
                compiled++;
            else
                failed++;
        }
    }

    std::cout << "\nResults: " << compiled << " compiled, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
