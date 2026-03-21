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

    // Navigate to a file's directory and highlight it (does NOT change Inspector)
    void navigateToFile(const std::string& relativePath);
    // Clear selection (when user clicks in another panel)
    void clearSelectedFile() { m_selectedFile.clear(); }
    // Clear highlight
    void clearHighlight() { m_highlightedFile.clear(); }
    const std::string& getHighlightedFile() const { return m_highlightedFile; }

private:
    std::string m_currentDir;
    std::string m_assetsDir;
    std::string m_selectedFile;      // clicked file — drives Inspector content
    std::string m_highlightedFile;   // jumped-to file — visual only
    bool m_initialized = false;
};

} // namespace QymEngine
