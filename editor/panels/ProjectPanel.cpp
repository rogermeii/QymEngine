#include "ProjectPanel.h"
#include "UIAutomation.h"
#include <imgui.h>
#include <json.hpp>
#include <algorithm>
#ifndef __ANDROID__
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
#else
#include <SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#endif

namespace QymEngine {

void ProjectPanel::onImGuiRender()
{
    if (!m_initialized) {
        m_assetsDir = std::string(ASSETS_DIR);
        m_currentDir = "";
        m_initialized = true;
    }

    ImGui::Begin("Project");
#ifndef __ANDROID__
    UIAutomation::recordPanel("Project");
#endif

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

    // Collect entries: separate directories and files
    struct Entry {
        std::string name;
        bool isDirectory;
    };
    std::vector<Entry> dirs;
    std::vector<Entry> files;

#ifdef __ANDROID__
    // Android: 用 AAssetManager 列举 APK 内的 assets 目录
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass clazz = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(clazz, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject jAssetMgr = env->CallObjectMethod(activity, getAssets);
    AAssetManager* mgr = AAssetManager_fromJava(env, jAssetMgr);
    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(jAssetMgr);
    env->DeleteLocalRef(activity);

    if (mgr) {
        // 对 Android，fullPath 就是相对于 assets/ 的路径，根目录用 ""
        const char* dirPath = m_currentDir.empty() ? "" : m_currentDir.c_str();
        AAssetDir* assetDir = AAssetManager_openDir(mgr, dirPath);
        if (assetDir) {
            // AAssetDir_getNextFileName 只返回文件名
            const char* filename = nullptr;
            while ((filename = AAssetDir_getNextFileName(assetDir)) != nullptr) {
                files.push_back({std::string(filename), false});
            }
            AAssetDir_close(assetDir);
        }
        // AAssetDir 不返回子目录，通过尝试打开已知子目录名来检测
        // 收集文件名中的路径前缀作为子目录候选
        // 更简单的方案：对根目录硬编码已知子目录
        if (m_currentDir.empty()) {
            const char* knownDirs[] = {"branding", "materials", "models", "scenes", "shaders", "textures"};
            for (auto* d : knownDirs) {
                AAssetDir* subDir = AAssetManager_openDir(mgr, d);
                if (subDir) {
                    // 检查是否有内容
                    if (AAssetDir_getNextFileName(subDir) != nullptr) {
                        dirs.push_back({std::string(d), true});
                    }
                    AAssetDir_close(subDir);
                }
            }
        } else {
            // 非根目录：扫描已有文件路径的前缀推断子目录
            // 通过列举更深一级的路径来检测子目录
            // AAssetManager_openDir 对子目录也能工作
            // 用一个简单的启发式：尝试常见子目录名
            // 先收集当前目录下的所有文件，从文件名推断不出子目录
            // 实际上 Android AAssetDir 的行为因设备而异，这里保守处理
        }
    }
#else
    // Desktop: 用 std::filesystem 列举真实文件系统
    if (!fs::exists(fullPath) || !fs::is_directory(fullPath)) {
        ImGui::Text("Directory not found: %s", fullPath.c_str());
        ImGui::End();
        return;
    }

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
#endif

    // Sort alphabetically
    std::sort(dirs.begin(), dirs.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });

    // ".." to go up one level (if not at root)
    if (!m_currentDir.empty()) {
        ImGui::Selectable("[..] Go up");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_selectedFile.clear();
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

    // Show directories first (double-click to enter)
    for (auto& d : dirs) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f)); // yellow for dirs
        std::string label = "[DIR] " + d.name;
        ImGui::Selectable(label.c_str());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (m_currentDir.empty())
                m_currentDir = d.name;
            else
                m_currentDir += "/" + d.name;
            m_selectedFile.clear();
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

        // Check for compound extensions (.mat.json, .shader.json) before simple .json
        std::string nameLower = f.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        bool isMat = nameLower.size() > 9 && nameLower.substr(nameLower.size() - 9) == ".mat.json";
        bool isShader = nameLower.size() > 12 && nameLower.substr(nameLower.size() - 12) == ".shader.json";

        if (isMat) {
            color = ImVec4(1.0f, 0.4f, 0.6f, 1.0f); // pink for materials
            prefix = "[MAT] ";
        } else if (isShader) {
            color = ImVec4(0.8f, 0.5f, 1.0f, 1.0f); // purple for shader definitions
            prefix = "[SHADER] ";
        } else if (ext == ".obj") {
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
        bool isHighlighted = (m_highlightedFile == relativePath);

        // Highlighted items (from jump) get a distinct background
        if (isHighlighted && !isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.4f, 0.5f, 0.2f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.5f, 0.6f, 0.3f, 0.7f));
        }

        if (ImGui::Selectable(label.c_str(), isSelected || isHighlighted)) {
            m_selectedFile = relativePath;
            m_highlightedFile.clear();  // clicking clears highlight
        }
#ifndef __ANDROID__
        UIAutomation::recordWidget("project/" + relativePath, f.name);
#endif

        if (isHighlighted && !isSelected) {
            ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleColor();
    }

    // Click empty space to deselect
    if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
        m_selectedFile.clear();

#ifndef __ANDROID__
    // Right-click context menu (仅 PC，Android APK assets 只读)
    if (ImGui::BeginPopupContextWindow("ProjectContextMenu")) {
        if (ImGui::MenuItem("New Material")) {
            std::string dir = m_currentDir.empty() ? "materials" : m_currentDir;
            std::string newName = "new_material";
            std::string newPath = dir + "/" + newName + ".mat.json";

            int counter = 1;
            while (fs::exists(m_assetsDir + "/" + newPath)) {
                newPath = dir + "/" + newName + "_" + std::to_string(counter++) + ".mat.json";
            }

            nlohmann::json j;
            j["name"] = newName;
            j["shader"] = "shaders/standard_lit.shader.json";
            j["properties"] = {
                {"baseColor", {1.0, 1.0, 1.0, 1.0}},
                {"metallic", 0.0},
                {"roughness", 0.5}
            };

            std::string fullPath = m_assetsDir + "/" + newPath;
            fs::create_directories(fs::path(fullPath).parent_path());
            std::ofstream outFile(fullPath);
            if (outFile.is_open()) {
                outFile << j.dump(2);
                outFile.close();
                m_selectedFile = newPath;
            }
        }
        ImGui::EndPopup();
    }
#endif

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

bool ProjectPanel::isSelectedMaterial() const {
    return m_selectedFile.find(".mat.json") != std::string::npos;
}

void ProjectPanel::navigateToFile(const std::string& relativePath) {
    // Navigate to directory and highlight (NOT select) the file
    std::string path = relativePath;
    std::replace(path.begin(), path.end(), '\\', '/');
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos) {
        m_currentDir = "";
    } else {
        m_currentDir = path.substr(0, lastSlash);
    }
    m_highlightedFile = relativePath;
}

} // namespace QymEngine
