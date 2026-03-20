#include "InspectorPanel.h"
#include <imgui.h>
#include <cstring>

namespace QymEngine {

void InspectorPanel::onImGuiRender(Scene& scene, AssetManager& assetManager) {
    ImGui::Begin("Inspector");

    Node* selected = scene.getSelectedNode();
    if (!selected) {
        ImGui::Text("No node selected");
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

    if (ImGui::CollapsingHeader("Texture", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::vector<std::string> texItems = {"Default"};
        for (auto& f : assetManager.getTextureFiles())
            texItems.push_back(f);

        int currentTex = 0;
        for (int i = 1; i < static_cast<int>(texItems.size()); i++) {
            if (texItems[i] == selected->texturePath) { currentTex = i; break; }
        }

        if (ImGui::BeginCombo("Texture File", texItems[currentTex].c_str())) {
            for (int i = 0; i < static_cast<int>(texItems.size()); i++) {
                bool isSelected = (currentTex == i);
                if (ImGui::Selectable(texItems[i].c_str(), isSelected)) {
                    if (i == 0) selected->texturePath = "";
                    else selected->texturePath = texItems[i];
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::End();
}

} // namespace QymEngine
