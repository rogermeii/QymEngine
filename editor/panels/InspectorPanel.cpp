#include "InspectorPanel.h"
#include <imgui.h>
#include <cstring>

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
        // Placeholder: material path display (full material UI in Task 8)
        char matBuf[256] = {};
        strncpy(matBuf, selected->materialPath.c_str(), sizeof(matBuf) - 1);
        if (ImGui::InputText("Material Path", matBuf, sizeof(matBuf)))
            selected->materialPath = matBuf;
    }

    ImGui::End();
}

} // namespace QymEngine
