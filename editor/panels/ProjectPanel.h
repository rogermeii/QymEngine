#pragma once
#include <string>
#include <vector>

namespace QymEngine {

class ProjectPanel {
public:
    void onImGuiRender();
private:
    std::string m_currentDir;  // relative path within assets/
    std::string m_assetsDir;   // absolute path to assets/
    bool m_initialized = false;
};

} // namespace QymEngine
