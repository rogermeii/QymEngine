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

namespace fs = std::filesystem;
using json = nlohmann::json;

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

// Compile a single shader with optional preprocessor defines
// outputSuffix: "" for normal, "_bindless" for bindless variant
static bool compileShaderVariant(const std::string& inputPath, const std::string& outputDir,
                                  const std::string& baseName,
                                  const std::vector<slang::PreprocessorMacroDesc>& macros,
                                  const std::string& outputSuffix) {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    slang_createGlobalSession(SLANG_API_VERSION, globalSession.writeRef());
    if (!globalSession) {
        std::cerr << "Failed to create Slang global session" << std::endl;
        return false;
    }

    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = globalSession->findProfile("glsl_450");

    sessionDesc.targetCount = 1;
    sessionDesc.targets = &targetDesc;

    // Set preprocessor macros
    sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    sessionDesc.preprocessorMacros = macros.empty() ? nullptr : macros.data();

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

    // Find entry points
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

    // Compile each entry point to SPIR-V
    std::map<std::string, std::vector<uint8_t>> spvOutputs; // suffix -> spv data

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

        // Save SPIR-V data for reflection
        auto* ptr = (const uint8_t*)spirvCode->getBufferPointer();
        spvOutputs[entry.suffix] = std::vector<uint8_t>(ptr, ptr + spirvCode->getBufferSize());

        // Write .spv file
        std::string spvPath;
        if (baseName == "Triangle" && outputSuffix.empty()) {
            spvPath = outputDir + "/" + (entry.suffix == "_vert" ? "vert.spv" : "frag.spv");
        } else {
            std::string lower = baseName;
            lower[0] = tolower(lower[0]);
            spvPath = outputDir + "/" + lower + entry.suffix + outputSuffix + ".spv";
        }

        std::ofstream spvFile(spvPath, std::ios::binary);
        spvFile.write((const char*)spirvCode->getBufferPointer(), spirvCode->getBufferSize());
        spvFile.close();
        std::cout << "  -> " << fs::path(spvPath).filename().string()
                  << " (" << spirvCode->getBufferSize() << " bytes)" << std::endl;
    }

    // Reflect using SPIRV-Reflect on the generated SPIR-V
    if (spvOutputs.count("_vert") && spvOutputs.count("_frag")) {
        json reflectJson = reflectSpirv(spvOutputs["_vert"], spvOutputs["_frag"]);
        std::string jsonPath = outputDir + "/" + baseName + outputSuffix + ".reflect.json";
        std::ofstream jsonFile(jsonPath);
        jsonFile << reflectJson.dump(2);
        jsonFile.close();
        std::cout << "  -> " << baseName + outputSuffix << ".reflect.json" << std::endl;
    }

    return true;
}

bool compileShader(const std::string& inputPath, const std::string& outputDir) {
    std::string baseName = fs::path(inputPath).stem().string();
    std::cout << "Compiling: " << baseName << ".slang" << std::endl;

    // 1. Normal compilation (no defines)
    bool ok = compileShaderVariant(inputPath, outputDir, baseName, {}, "");
    if (!ok) return false;

    // 2. Bindless compilation (USE_BINDLESS=1) -- skip Grid.slang (no set 1)
    if (baseName != "Grid") {
        std::cout << "  [bindless variant]" << std::endl;
        slang::PreprocessorMacroDesc bindlessMacro;
        bindlessMacro.name = "USE_BINDLESS";
        bindlessMacro.value = "1";
        std::vector<slang::PreprocessorMacroDesc> macros = { bindlessMacro };
        bool bindlessOk = compileShaderVariant(inputPath, outputDir, baseName, macros, "_bindless");
        if (!bindlessOk) {
            std::cerr << "  WARNING: Bindless variant failed for " << baseName << ", continuing..." << std::endl;
            // Not fatal - non-bindless path still works
        }
    }

    return true;
}

int main(int argc, char* argv[]) {
    std::string inputDir = "assets/shaders";
    std::string outputDir = "assets/shaders";

    if (argc >= 2) inputDir = argv[1];
    if (argc >= 3) outputDir = argv[2];

    std::cout << "=== QymEngine Shader Compiler ===" << std::endl;
    std::cout << "Input:  " << inputDir << std::endl;
    std::cout << "Output: " << outputDir << std::endl;

    int compiled = 0, failed = 0;
    for (auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.path().extension() == ".slang") {
            if (compileShader(entry.path().string(), outputDir))
                compiled++;
            else
                failed++;
        }
    }

    std::cout << "\nResults: " << compiled << " compiled, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
