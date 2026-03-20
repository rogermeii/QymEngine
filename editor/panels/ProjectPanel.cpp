#include "ProjectPanel.h"
#include <imgui.h>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace QymEngine {

void ProjectPanel::onImGuiRender()
{
    if (!m_initialized) {
        m_assetsDir = std::string(ASSETS_DIR);
        m_currentDir = "";
        m_initialized = true;
    }

    ImGui::Begin("Project");

    // --- Breadcrumb navigation ---
    {
        // "assets" root button
        if (ImGui::SmallButton("assets")) {
            m_currentDir = "";
        }

        // Split current path into parts and show clickable breadcrumbs
        if (!m_currentDir.empty()) {
            std::string accumulated;
            std::string remaining = m_currentDir;

            // Replace backslashes with forward slashes for consistency
            std::replace(remaining.begin(), remaining.end(), '\\', '/');

            size_t pos = 0;
            while (pos < remaining.size()) {
                size_t nextSlash = remaining.find('/', pos);
                std::string part;
                if (nextSlash == std::string::npos) {
                    part = remaining.substr(pos);
                    pos = remaining.size();
                } else {
                    part = remaining.substr(pos, nextSlash - pos);
                    pos = nextSlash + 1;
                }

                if (part.empty()) continue;

                ImGui::SameLine();
                ImGui::Text("/");
                ImGui::SameLine();

                if (!accumulated.empty()) accumulated += "/";
                accumulated += part;

                // Make each breadcrumb segment clickable
                ImGui::PushID(accumulated.c_str());
                if (ImGui::SmallButton(part.c_str())) {
                    m_currentDir = accumulated;
                }
                ImGui::PopID();
            }
        }
    }

    ImGui::Separator();

    // Build full path to list
    std::string fullPath = m_assetsDir;
    if (!m_currentDir.empty()) {
        fullPath += "/" + m_currentDir;
    }

    // Check if directory exists
    if (!fs::exists(fullPath) || !fs::is_directory(fullPath)) {
        ImGui::Text("Directory not found: %s", fullPath.c_str());
        ImGui::End();
        return;
    }

    // Collect entries: separate directories and files
    struct Entry {
        std::string name;
        bool isDirectory;
    };
    std::vector<Entry> dirs;
    std::vector<Entry> files;

    try {
        for (auto& entry : fs::directory_iterator(fullPath)) {
            std::string name = entry.path().filename().string();
            if (entry.is_directory()) {
                dirs.push_back({name, true});
            } else if (entry.is_regular_file()) {
                files.push_back({name, false});
            }
        }
    } catch (const fs::filesystem_error&) {
        ImGui::Text("Error reading directory");
        ImGui::End();
        return;
    }

    // Sort alphabetically
    std::sort(dirs.begin(), dirs.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });

    // ".." to go up one level (if not at root)
    if (!m_currentDir.empty()) {
        if (ImGui::Selectable("[..] Go up")) {
            // Remove last path component
            std::string dir = m_currentDir;
            std::replace(dir.begin(), dir.end(), '\\', '/');
            size_t lastSlash = dir.rfind('/');
            if (lastSlash == std::string::npos) {
                m_currentDir = "";
            } else {
                m_currentDir = dir.substr(0, lastSlash);
            }
        }
    }

    // Show directories first
    for (auto& d : dirs) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f)); // yellow for dirs
        std::string label = "[DIR] " + d.name;
        if (ImGui::Selectable(label.c_str())) {
            if (m_currentDir.empty())
                m_currentDir = d.name;
            else
                m_currentDir += "/" + d.name;
        }
        ImGui::PopStyleColor();
    }

    // Show files
    for (auto& f : files) {
        // Determine file extension for coloring
        std::string ext;
        size_t dotPos = f.name.rfind('.');
        if (dotPos != std::string::npos) {
            ext = f.name.substr(dotPos);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                [](unsigned char c) { return std::tolower(c); });
        }

        ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f); // default gray
        std::string prefix;

        if (ext == ".obj") {
            color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); // blue for models
            prefix = "[OBJ] ";
        } else if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
            color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // green for textures
            prefix = "[IMG] ";
        } else if (ext == ".json") {
            color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f); // orange for json
            prefix = "[JSON] ";
        } else if (ext == ".spv" || ext == ".vert" || ext == ".frag" || ext == ".glsl") {
            color = ImVec4(0.8f, 0.5f, 1.0f, 1.0f); // purple for shaders
            prefix = "[SHADER] ";
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        std::string label = prefix + f.name;
        std::string relativePath = m_currentDir.empty() ? f.name : m_currentDir + "/" + f.name;
        bool isSelected = (m_selectedFile == relativePath);
        if (ImGui::Selectable(label.c_str(), isSelected)) {
            m_selectedFile = relativePath;
        }
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

bool ProjectPanel::isSelectedImage() const {
    if (m_selectedFile.empty()) return false;
    std::string ext;
    size_t dot = m_selectedFile.rfind('.');
    if (dot != std::string::npos) ext = m_selectedFile.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

bool ProjectPanel::isSelectedModel() const {
    if (m_selectedFile.empty()) return false;
    std::string ext;
    size_t dot = m_selectedFile.rfind('.');
    if (dot != std::string::npos) ext = m_selectedFile.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".obj";
}

} // namespace QymEngine
