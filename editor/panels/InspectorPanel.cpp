#include "InspectorPanel.h"
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
                const MaterialAsset* mat = assetManager.loadMaterial(projectPanel.getSelectedFile());
                if (mat) {
                    ImGui::Text("Material: %s", mat->name.c_str());
                    ImGui::Separator();

                    if (mat->shader) {
                        ImGui::Text("Shader: %s", mat->shader->name.c_str());
                        drawGoToButton("goto_shader", mat->shaderPath, projectPanel);
                    }

                    MaterialAsset* mutableMat = const_cast<MaterialAsset*>(mat);

                    // Dynamic UI from shader properties
                    if (mat->shader) {
                        // Build texture file list once (reused by all texture2D properties)
                        std::vector<std::string> texItems = {"None"};
                        for (auto& f : assetManager.getTextureFiles())
                            texItems.push_back(f);

                        for (auto& prop : mat->shader->properties) {
                            if (prop.type == "color4" && prop.name == "baseColor") {
                                ImGui::ColorEdit4("Base Color", &mutableMat->baseColor.x);
                            } else if (prop.type == "float") {
                                if (prop.name == "metallic") {
                                    ImGui::SliderFloat("Metallic", &mutableMat->metallic, prop.rangeMin, prop.rangeMax);
                                } else if (prop.name == "roughness") {
                                    ImGui::SliderFloat("Roughness", &mutableMat->roughness, prop.rangeMin, prop.rangeMax);
                                }
                            } else if (prop.type == "texture2D") {
                                std::string* texPath = nullptr;
                                if (prop.name == "albedoMap") texPath = &mutableMat->albedoMapPath;
                                else if (prop.name == "normalMap") texPath = &mutableMat->normalMapPath;

                                if (texPath) {
                                    int currentTex = 0;
                                    for (int i = 1; i < static_cast<int>(texItems.size()); i++) {
                                        if (texItems[i] == *texPath) { currentTex = i; break; }
                                    }

                                    if (ImGui::BeginCombo(prop.name.c_str(), texItems[currentTex].c_str())) {
                                        for (int i = 0; i < static_cast<int>(texItems.size()); i++) {
                                            bool isSel = (currentTex == i);
                                            if (ImGui::Selectable(texItems[i].c_str(), isSel)) {
                                                *texPath = (i == 0) ? "" : texItems[i];
                                            }
                                        }
                                        ImGui::EndCombo();
                                    }
                                    if (!texPath->empty()) {
                                        drawGoToButton(("goto_" + prop.name).c_str(), *texPath, projectPanel);
                                    }
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
                        props["baseColor"] = {mutableMat->baseColor.r, mutableMat->baseColor.g,
                                              mutableMat->baseColor.b, mutableMat->baseColor.a};
                        props["metallic"] = mutableMat->metallic;
                        props["roughness"] = mutableMat->roughness;
                        if (!mutableMat->albedoMapPath.empty())
                            props["albedoMap"] = mutableMat->albedoMapPath;
                        if (!mutableMat->normalMapPath.empty())
                            props["normalMap"] = mutableMat->normalMapPath;
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

    // Editable name
    char buf[256];
    std::strncpy(buf, selected->name.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (ImGui::InputText("Name", buf, sizeof(buf)))
        selected->name = buf;

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Position", &selected->transform.position.x, 0.1f);
        ImGui::DragFloat3("Rotation", &selected->transform.rotation.x, 1.0f);
        ImGui::DragFloat3("Scale",    &selected->transform.scale.x, 0.01f, 0.01f, 100.0f);
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
                    selected->materialPath = (i == 0) ? "" : matItems[i];
                }
            }
            ImGui::EndCombo();
        }
        // Go to material file
        if (!selected->materialPath.empty()) {
            drawGoToButton("goto_material", selected->materialPath, projectPanel);

            // Show read-only summary of material properties
            const MaterialAsset* mat = assetManager.loadMaterial(selected->materialPath);
            if (mat && mat->shader) {
                ImGui::Text("  Shader: %s", mat->shader->name.c_str());
                ImGui::Text("  Color: (%.1f, %.1f, %.1f)", mat->baseColor.r, mat->baseColor.g, mat->baseColor.b);
            }
        }
    }

    ImGui::End();
}

} // namespace QymEngine
