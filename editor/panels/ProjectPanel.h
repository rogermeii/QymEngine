#pragma once
#include <string>
#include <vector>

namespace QymEngine {

class ProjectPanel {
public:
    void onImGuiRender();

    // Selected file in Project panel (relative to assets/)
    const std::string& getSelectedFile() const { return m_selectedFile; }
    bool hasSelectedFile() const { return !m_selectedFile.empty(); }
    bool isSelectedImage() const;
    bool isSelectedModel() const;
    bool isSelectedMaterial() const;

private:
    std::string m_currentDir;
    std::string m_assetsDir;
    std::string m_selectedFile;  // e.g. "textures/container.jpg"
    bool m_initialized = false;
};

} // namespace QymEngine
