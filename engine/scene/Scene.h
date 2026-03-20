#pragma once
#include "Node.h"
#include <string>
#include <functional>

namespace QymEngine {

class Scene {
public:
    Scene();

    std::string name = "Untitled";

    Node* getRoot() const { return m_root.get(); }
    Node* getSelectedNode() const { return m_selectedNode; }
    void setSelectedNode(Node* node) { m_selectedNode = node; }

    Node* createNode(const std::string& nodeName, Node* parent = nullptr);
    void removeNode(Node* node);

    void traverseNodes(const std::function<void(Node*)>& fn) const;

    void serialize(const std::string& path) const;
    void deserialize(const std::string& path);

private:
    void traverseRecursive(Node* node, const std::function<void(Node*)>& fn) const;

    std::unique_ptr<Node> m_root;
    Node* m_selectedNode = nullptr;
};

} // namespace QymEngine
