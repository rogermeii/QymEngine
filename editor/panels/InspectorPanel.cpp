#include "InspectorPanel.h"
#include "asset/MaterialAsset.h"
#include "asset/ShaderAsset.h"
#include <imgui.h>
#include <json.hpp>
#include <cstring>
#include <fstream>

namespace QymEngine {

void InspectorPanel::onImGuiRender(Scene& scene, AssetManager& assetManager, ModelPreview& modelPreview, ProjectPanel& projectPanel) {
    ImGui::Begin("Inspector");

    Node* selected = scene.getSelectedNode();
    if (!selected) {
        // Asset preview mode: show preview of selected file in Project panel
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
                auto* mat = assetManager.loadMaterial(projectPanel.getSelectedFile());
                if (mat) {
                    ImGui::Text("Material: %s", mat->name.c_str());
                    ImGui::Separator();
                    if (mat->shader)
                        ImGui::Text("Shader: %s", mat->shader->name.c_str());
                    float previewColor[4] = {mat->baseColor.x, mat->baseColor.y, mat->baseColor.z, mat->baseColor.w};
                    ImGui::ColorEdit4("Base Color", previewColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
                    ImGui::Text("Metallic: %.2f", mat->metallic);
                    ImGui::Text("Roughness: %.2f", mat->roughness);
                    if (!mat->albedoMapPath.empty())
                        ImGui::Text("Albedo: %s", mat->albedoMapPath.c_str());
                    if (!mat->normalMapPath.empty())
                        ImGui::Text("Normal: %s", mat->normalMapPath.c_str());
                }
            } else {
                ImGui::Text("No preview available for this file type");
            }
        } else {
            ImGui::Text("No node selected");
        }
        ImGui::End();
        return;
    }

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

    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* meshTypes[] = {"None", "Quad", "Cube", "Plane", "Sphere"};
        int current = static_cast<int>(selected->meshType);
        if (ImGui::Combo("Type", &current, meshTypes, IM_ARRAYSIZE(meshTypes)))
            selected->meshType = static_cast<MeshType>(current);
    }

    if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Build items list: "Built-in" + all .obj files from AssetManager
        std::vector<std::string> meshItems = {"Built-in"};
        for (auto& f : assetManager.getMeshFiles())
            meshItems.push_back(f);

        // Find current selection
        int currentMesh = 0; // "Built-in"
        for (int i = 1; i < static_cast<int>(meshItems.size()); i++) {
            if (meshItems[i] == selected->meshPath) { currentMesh = i; break; }
        }

        // Combo
        if (ImGui::BeginCombo("Mesh File", meshItems[currentMesh].c_str())) {
            for (int i = 0; i < static_cast<int>(meshItems.size()); i++) {
                bool isSelected = (currentMesh == i);
                if (ImGui::Selectable(meshItems[i].c_str(), isSelected)) {
                    if (i == 0) selected->meshPath = "";  // Built-in
                    else selected->meshPath = meshItems[i];
                }
            }
            ImGui::EndCombo();
        }
    }

    // 模型3D预览
    if (modelPreview.isReady()) {
        bool hasMesh = (!selected->meshPath.empty() || selected->meshType != MeshType::None);
        if (hasMesh) {
            ImGui::Separator();
            ImGui::Text("Model Preview:");
            ImGui::Image(reinterpret_cast<ImTextureID>(modelPreview.getDescriptorSet()), ImVec2(200, 200));
        }
    }

    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Material file dropdown
        std::vector<std::string> matItems = {"Default"};
        for (auto& f : assetManager.getMaterialFiles())
            matItems.push_back(f);

        int currentMat = 0;
        for (int i = 1; i < static_cast<int>(matItems.size()); i++) {
            if (matItems[i] == selected->materialPath) { currentMat = i; break; }
        }

        if (ImGui::BeginCombo("Material File", matItems[currentMat].c_str())) {
            for (int i = 0; i < static_cast<int>(matItems.size()); i++) {
                bool isSelected = (currentMat == i);
                if (ImGui::Selectable(matItems[i].c_str(), isSelected)) {
                    selected->materialPath = (i == 0) ? "" : matItems[i];
                }
            }
            ImGui::EndCombo();
        }

        // Show and edit material properties if a material is assigned
        if (!selected->materialPath.empty()) {
            const MaterialAsset* mat = assetManager.loadMaterial(selected->materialPath);
            if (mat && mat->shader) {
                ImGui::Separator();
                ImGui::Text("Shader: %s", mat->shader->name.c_str());

                // We need mutable access for editing
                MaterialAsset* mutableMat = const_cast<MaterialAsset*>(mat);
                bool modified = false;

                // Dynamic UI from shader properties
                for (auto& prop : mat->shader->properties) {
                    if (prop.type == "color4" && prop.name == "baseColor") {
                        modified |= ImGui::ColorEdit4("Base Color", &mutableMat->baseColor.x);
                    } else if (prop.type == "float") {
                        if (prop.name == "metallic") {
                            modified |= ImGui::SliderFloat("Metallic", &mutableMat->metallic, prop.rangeMin, prop.rangeMax);
                        } else if (prop.name == "roughness") {
                            modified |= ImGui::SliderFloat("Roughness", &mutableMat->roughness, prop.rangeMin, prop.rangeMax);
                        }
                    } else if (prop.type == "texture2D") {
                        // Texture path display
                        std::string* texPath = nullptr;
                        if (prop.name == "albedoMap") texPath = &mutableMat->albedoMapPath;
                        else if (prop.name == "normalMap") texPath = &mutableMat->normalMapPath;

                        if (texPath) {
                            // Show texture dropdown
                            std::string label = prop.name;
                            std::vector<std::string> texItems2 = {"None"};
                            for (auto& f : assetManager.getTextureFiles())
                                texItems2.push_back(f);

                            int currentTex = 0;
                            for (int i = 1; i < static_cast<int>(texItems2.size()); i++) {
                                if (texItems2[i] == *texPath) { currentTex = i; break; }
                            }

                            if (ImGui::BeginCombo(label.c_str(), texItems2[currentTex].c_str())) {
                                for (int i = 0; i < static_cast<int>(texItems2.size()); i++) {
                                    bool isSel = (currentTex == i);
                                    if (ImGui::Selectable(texItems2[i].c_str(), isSel)) {
                                        *texPath = (i == 0) ? "" : texItems2[i];
                                        modified = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                        }
                    }
                }

                // Save button
                if (ImGui::Button("Save Material")) {
                    // Write material back to .mat.json
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

                    std::string savePath = std::string(ASSETS_DIR) + "/" + selected->materialPath;
                    std::ofstream outFile(savePath);
                    if (outFile.is_open()) {
                        outFile << j.dump(2);
                        outFile.close();
                    }
                }
            }
        }
    }

    ImGui::End();
}

} // namespace QymEngine
