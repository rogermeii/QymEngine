#include "InspectorPanel.h"
#include "UIAutomation.h"
#include "asset/MaterialAsset.h"
#include "asset/ShaderAsset.h"
#include <imgui.h>
#include <json.hpp>
#include <cstring>
#include <fstream>

namespace QymEngine {

// Helper: draw a "Go To" button that navigates ProjectPanel to a file and highlights it
static bool drawGoToButton(const char* id, const std::string& path, ProjectPanel& projectPanel) {
    if (path.empty()) return false;
    ImGui::SameLine();
    ImGui::PushID(id);
    bool clicked = ImGui::SmallButton(">");
    if (clicked) {
        projectPanel.navigateToFile(path);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Go to: %s", path.c_str());
    ImGui::PopID();
    return clicked;
}

void InspectorPanel::onImGuiRender(Scene& scene, AssetManager& assetManager, ModelPreview& modelPreview, ProjectPanel& projectPanel) {
    ImGui::Begin("Inspector");
#ifndef __ANDROID__
    UIAutomation::recordPanel("Inspector");
#endif

    Node* selected = scene.getSelectedNode();

    // Asset preview mode: file selected in Project panel takes priority over node
    if (projectPanel.hasSelectedFile()) {
        ImGui::Text("Asset: %s", projectPanel.getSelectedFile().c_str());
        ImGui::Separator();

        if (projectPanel.isSelectedImage()) {
                auto* tex = assetManager.loadTexture(projectPanel.getSelectedFile());
                if (tex && tex->descriptorSet != VK_NULL_HANDLE) {
                    ImGui::Text("Texture Preview:");
                    float previewSize = ImGui::GetContentRegionAvail().x - 10.0f;
                    if (previewSize < 64.0f) previewSize = 64.0f;
                    if (previewSize > 512.0f) previewSize = 512.0f;
                    ImGui::Image(reinterpret_cast<ImTextureID>(tex->descriptorSet), ImVec2(previewSize, previewSize));
                }
            } else if (projectPanel.isSelectedModel()) {
                if (modelPreview.isReady()) {
                    ImGui::Text("Model Preview:");
                    float w = ImGui::GetContentRegionAvail().x - 10.0f;
                    float side = (w < 256.0f) ? w : 256.0f;
                    if (side < 64.0f) side = 64.0f;
                    ImGui::Image(reinterpret_cast<ImTextureID>(modelPreview.getDescriptorSet()), ImVec2(side, side));
                }
            } else if (projectPanel.isSelectedMaterial()) {
                // --- Material editing (only when selected in Project panel) ---
                const MaterialInstance* mat = assetManager.loadMaterial(projectPanel.getSelectedFile());
                if (mat) {
                    ImGui::Text("Material: %s", mat->name.c_str());
                    ImGui::Separator();

                    if (mat->shader) {
                        ImGui::Text("Shader: %s", mat->shader->name.c_str());
                        drawGoToButton("goto_shader", mat->shaderPath, projectPanel);
                    }

                    MaterialInstance* mutableMat = const_cast<MaterialInstance*>(mat);

                    // Shader switching dropdown
                    {
                        auto& shaderFiles = assetManager.getShaderFiles();
                        std::vector<std::string> shaderItems;
                        int currentShaderIdx = 0;
                        for (int i = 0; i < static_cast<int>(shaderFiles.size()); i++) {
                            shaderItems.push_back(shaderFiles[i]);
                            if (shaderFiles[i] == mutableMat->shaderPath) currentShaderIdx = i;
                        }

                        if (!shaderItems.empty() && ImGui::BeginCombo("Shader##switch", shaderItems[currentShaderIdx].c_str())) {
                            for (int i = 0; i < static_cast<int>(shaderItems.size()); i++) {
                                bool isSel = (currentShaderIdx == i);
                                if (ImGui::Selectable(shaderItems[i].c_str(), isSel)) {
                                    if (shaderItems[i] != mutableMat->shaderPath) {
                                        // Switch shader: update path and clear cache
                                        // so it rebuilds next frame with new shader
                                        mutableMat->shaderPath = shaderItems[i];
                                        assetManager.invalidateMaterial(projectPanel.getSelectedFile());
                                        ImGui::EndCombo();
                                        // Material invalidated, stop using freed pointer
                                        ImGui::End();
                                        return;
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }

                    // Dynamic UI from shader properties
                    if (mat->shader) {
                        // Build texture file list once (reused by all texture2D properties)
                        std::vector<std::string> texItems = {"None"};
                        for (auto& f : assetManager.getTextureFiles())
                            texItems.push_back(f);

                        for (auto& prop : mat->shader->properties) {
                            if (prop.type == "color4") {
                                auto& val = mutableMat->vec4Props[prop.name];
                                if (ImGui::ColorEdit4(prop.name.c_str(), &val.x)) {
                                    // Write updated value to mapped param buffer
                                    if (mutableMat->paramMapped && mat->shader) {
                                        const auto& reflection = mat->shader->pipeline.getReflection();
                                        auto set1It = reflection.sets.find(1);
                                        if (set1It != reflection.sets.end()) {
                                            for (auto& rb : set1It->second) {
                                                if (rb.type == "uniformBuffer") {
                                                    for (auto& member : rb.members) {
                                                        if (member.name == prop.name) {
                                                            memcpy(static_cast<char*>(mutableMat->paramMapped) + member.offset,
                                                                   &val, sizeof(glm::vec4));
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            } else if (prop.type == "float") {
                                auto& val = mutableMat->floatProps[prop.name];
                                if (ImGui::SliderFloat(prop.name.c_str(), &val, prop.rangeMin, prop.rangeMax)) {
                                    // Write updated value to mapped param buffer
                                    if (mutableMat->paramMapped && mat->shader) {
                                        const auto& reflection = mat->shader->pipeline.getReflection();
                                        auto set1It = reflection.sets.find(1);
                                        if (set1It != reflection.sets.end()) {
                                            for (auto& rb : set1It->second) {
                                                if (rb.type == "uniformBuffer") {
                                                    for (auto& member : rb.members) {
                                                        if (member.name == prop.name) {
                                                            memcpy(static_cast<char*>(mutableMat->paramMapped) + member.offset,
                                                                   &val, sizeof(float));
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            } else if (prop.type == "texture2D") {
                                std::string& texPath = mutableMat->texturePaths[prop.name];

                                int currentTex = 0;
                                for (int i = 1; i < static_cast<int>(texItems.size()); i++) {
                                    if (texItems[i] == texPath) { currentTex = i; break; }
                                }

                                if (ImGui::BeginCombo(prop.name.c_str(), texItems[currentTex].c_str())) {
                                    for (int i = 0; i < static_cast<int>(texItems.size()); i++) {
                                        bool isSel = (currentTex == i);
                                        if (ImGui::Selectable(texItems[i].c_str(), isSel)) {
                                            texPath = (i == 0) ? "" : texItems[i];

                                            // Save material properties to file, then invalidate cache to rebuild descriptor
                                            {
                                                nlohmann::json j;
                                                j["name"] = mutableMat->name;
                                                j["shader"] = mutableMat->shaderPath;
                                                nlohmann::json props2;
                                                for (auto& [n, v] : mutableMat->vec4Props)
                                                    props2[n] = {v.r, v.g, v.b, v.a};
                                                for (auto& [n, v] : mutableMat->floatProps)
                                                    props2[n] = v;
                                                for (auto& [n, p] : mutableMat->texturePaths) {
                                                    if (!p.empty())
                                                        props2[n] = p;
                                                }
                                                j["properties"] = props2;

                                                std::string savePath = std::string(ASSETS_DIR) + "/" + projectPanel.getSelectedFile();
                                                std::ofstream outFile(savePath);
                                                if (outFile.is_open()) {
                                                    outFile << j.dump(2);
                                                    outFile.close();
                                                }
                                            }
                                            assetManager.invalidateMaterial(projectPanel.getSelectedFile());
                                            ImGui::EndCombo();
                                            // Material invalidated, stop using freed pointer
                                            ImGui::End();
                                            return;
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                                if (!texPath.empty()) {
                                    drawGoToButton(("goto_" + prop.name).c_str(), texPath, projectPanel);
                                }
                            }
                        }
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Save Material")) {
                        nlohmann::json j;
                        j["name"] = mutableMat->name;
                        j["shader"] = mutableMat->shaderPath;
                        nlohmann::json props;
                        for (auto& [name, val] : mutableMat->vec4Props)
                            props[name] = {val.r, val.g, val.b, val.a};
                        for (auto& [name, val] : mutableMat->floatProps)
                            props[name] = val;
                        for (auto& [name, path] : mutableMat->texturePaths) {
                            if (!path.empty())
                                props[name] = path;
                        }
                        j["properties"] = props;

                        std::string savePath = std::string(ASSETS_DIR) + "/" + projectPanel.getSelectedFile();
                        std::ofstream outFile(savePath);
                        if (outFile.is_open()) {
                            outFile << j.dump(2);
                            outFile.close();
                        }
                    }
                }
            } else {
            ImGui::Text("No preview available for this file type");
        }
        ImGui::End();
        return;
    }

    if (!selected) {
        ImGui::Text("No node selected");
        ImGui::End();
        return;
    }

    // ===================== Node properties =====================

    // Editable name (undo on commit)
    char buf[256];
    std::strncpy(buf, selected->name.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (ImGui::InputText("Name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (m_saveState) m_saveState();
        selected->name = buf;
    }
#ifndef __ANDROID__
    UIAutomation::recordWidget("inspector/Name", "Name");
#endif

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Save undo state when drag starts
        bool anyActive = false;
        ImGui::DragFloat3("Position", &selected->transform.position.x, 0.1f);
        anyActive |= ImGui::IsItemActive();
#ifndef __ANDROID__
        UIAutomation::recordWidget("inspector/Position", "Position");
#endif
        ImGui::DragFloat3("Rotation", &selected->transform.rotation.x, 1.0f);
        anyActive |= ImGui::IsItemActive();
#ifndef __ANDROID__
        UIAutomation::recordWidget("inspector/Rotation", "Rotation");
#endif
        ImGui::DragFloat3("Scale",    &selected->transform.scale.x, 0.01f, 0.01f, 100.0f);
        anyActive |= ImGui::IsItemActive();
#ifndef __ANDROID__
        UIAutomation::recordWidget("inspector/Scale", "Scale");
#endif

        if (anyActive && !m_wasDragging && m_saveState)
            m_saveState();
        m_wasDragging = anyActive;
    }

    // --- Light properties (DirectionalLight / PointLight / SpotLight) ---
    if (selected->isLight()) {
        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Light Color", &selected->lightColor.x);
            bool lightActive = ImGui::IsItemActive();
            ImGui::SliderFloat("Intensity", &selected->lightIntensity, 0.0f, 10.0f);
            lightActive |= ImGui::IsItemActive();

            if (selected->nodeType == NodeType::PointLight || selected->nodeType == NodeType::SpotLight) {
                ImGui::DragFloat("Range", &selected->lightRange, 0.1f, 0.1f, 100.0f);
                lightActive |= ImGui::IsItemActive();
            }
            if (selected->nodeType == NodeType::SpotLight) {
                ImGui::SliderFloat("Inner Angle", &selected->spotInnerAngle, 1.0f, 89.0f);
                lightActive |= ImGui::IsItemActive();
                ImGui::SliderFloat("Outer Angle", &selected->spotOuterAngle, 1.0f, 90.0f);
                lightActive |= ImGui::IsItemActive();
                // Ensure outer >= inner
                if (selected->spotOuterAngle < selected->spotInnerAngle)
                    selected->spotOuterAngle = selected->spotInnerAngle;
            }

            // Undo tracking for light properties
            if (lightActive && !m_wasLightDragging && m_saveState)
                m_saveState();
            m_wasLightDragging = lightActive;

            glm::vec3 dir = selected->getLightDirection();
            ImGui::Text("Direction: (%.2f, %.2f, %.2f)", dir.x, dir.y, dir.z);
        }
        ImGui::End();
        return;
    }

    // --- Mesh (merged: built-in type + .obj model) ---
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Build combined items: built-in types + .obj files
        std::vector<std::string> meshItems = {"None", "Quad", "Cube", "Plane", "Sphere"};
        const int builtInCount = static_cast<int>(meshItems.size());
        for (auto& f : assetManager.getMeshFiles())
            meshItems.push_back(f);

        // Determine current selection
        int currentIdx = 0;
        if (!selected->meshPath.empty()) {
            // Using an .obj model
            for (int i = builtInCount; i < static_cast<int>(meshItems.size()); i++) {
                if (meshItems[i] == selected->meshPath) { currentIdx = i; break; }
            }
        } else {
            // Using built-in mesh type
            currentIdx = static_cast<int>(selected->meshType);
        }

        if (ImGui::BeginCombo("##MeshCombo", meshItems[currentIdx].c_str())) {
            for (int i = 0; i < static_cast<int>(meshItems.size()); i++) {
                bool isSel = (currentIdx == i);
                if (ImGui::Selectable(meshItems[i].c_str(), isSel)) {
                    if (m_saveState) m_saveState();
                    if (i < builtInCount) {
                        // Built-in mesh
                        selected->meshType = static_cast<MeshType>(i);
                        selected->meshPath = "";
                    } else {
                        // .obj model
                        selected->meshPath = meshItems[i];
                        selected->meshType = MeshType::None;
                    }
                }
            }
            ImGui::EndCombo();
        }
        // Go to .obj file
        if (!selected->meshPath.empty()) {
            drawGoToButton("goto_mesh", selected->meshPath, projectPanel);
        }
    }

    // Model 3D preview
    if (modelPreview.isReady()) {
        bool hasMesh = (!selected->meshPath.empty() || selected->meshType != MeshType::None);
        if (hasMesh) {
            ImGui::Separator();
            ImGui::Text("Model Preview:");
            ImGui::Image(reinterpret_cast<ImTextureID>(modelPreview.getDescriptorSet()), ImVec2(200, 200));
        }
    }

    // --- Material (reference only, editing in Project panel) ---
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Material file dropdown
        std::vector<std::string> matItems = {"Default"};
        for (auto& f : assetManager.getMaterialFiles())
            matItems.push_back(f);

        int currentMat = 0;
        for (int i = 1; i < static_cast<int>(matItems.size()); i++) {
            if (matItems[i] == selected->materialPath) { currentMat = i; break; }
        }

        if (ImGui::BeginCombo("##MaterialCombo", matItems[currentMat].c_str())) {
            for (int i = 0; i < static_cast<int>(matItems.size()); i++) {
                bool isSelected = (currentMat == i);
                if (ImGui::Selectable(matItems[i].c_str(), isSelected)) {
                    if (m_saveState) m_saveState();
                    selected->materialPath = (i == 0) ? "" : matItems[i];
                }
            }
            ImGui::EndCombo();
        }
        // Go to material file
        if (!selected->materialPath.empty()) {
            drawGoToButton("goto_material", selected->materialPath, projectPanel);

            // Show read-only summary of material properties
            const MaterialInstance* mat = assetManager.loadMaterial(selected->materialPath);
            if (mat && mat->shader) {
                ImGui::Text("  Shader: %s", mat->shader->name.c_str());
                auto bcIt = mat->vec4Props.find("baseColor");
                if (bcIt != mat->vec4Props.end())
                    ImGui::Text("  Color: (%.1f, %.1f, %.1f)", bcIt->second.r, bcIt->second.g, bcIt->second.b);
            }
        }
    }

    ImGui::End();
}

} // namespace QymEngine
