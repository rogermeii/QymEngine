#pragma once
#include <string>
#include <vector>

namespace QymEngine {

class Clipboard {
public:
    void copyNodes(const std::vector<std::string>& serializedNodes) {
        m_nodeJsons = serializedNodes;
    }

    const std::vector<std::string>& getNodes() const { return m_nodeJsons; }
    bool hasContent() const { return !m_nodeJsons.empty(); }
    void clear() { m_nodeJsons.clear(); }

private:
    std::vector<std::string> m_nodeJsons;
};

} // namespace QymEngine
