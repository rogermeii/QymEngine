#pragma once
#include "Node.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>

namespace QymEngine {

class Scene {
public:
    Scene();

    std::string name = "Untitled";

    Node* getRoot() const { return m_root.get(); }

    // Single selection (primary selected node)
    Node* getSelectedNode() const { return m_selectedNode; }
    void setSelectedNode(Node* node) { m_selectedNode = node; }

    // Multi-selection
    const std::unordered_set<Node*>& getSelectedNodes() const { return m_selectedNodes; }
    bool isNodeSelected(Node* node) const { return m_selectedNodes.count(node) > 0; }
    void selectNode(Node* node, bool addToSelection = false);
    void deselectNode(Node* node);
    void clearSelection();
    void selectRange(Node* from, Node* to);

    Node* createNode(const std::string& nodeName, Node* parent = nullptr);
    void removeNode(Node* node);

    void traverseNodes(const std::function<void(Node*)>& fn) const;

    void serialize(const std::string& path) const;
    void deserialize(const std::string& path);

    // Serialize/deserialize to/from JSON string (for undo/redo)
    std::string toJsonString() const;
    void fromJsonString(const std::string& jsonStr);

    // Serialize a single node subtree to JSON string (for copy/paste)
    std::string serializeNodeToString(Node* node) const;
    // Deserialize and add a node subtree as child of parent
    Node* deserializeNodeFromString(const std::string& jsonStr, Node* parent);
    // Deserialize and insert at specific index
    Node* deserializeNodeFromString(const std::string& jsonStr, Node* parent, int index);

private:
    void traverseRecursive(Node* node, const std::function<void(Node*)>& fn) const;
    void collectFlatList(Node* node, std::vector<Node*>& list) const;

    std::unique_ptr<Node> m_root;
    Node* m_selectedNode = nullptr;
    std::unordered_set<Node*> m_selectedNodes;
};

} // namespace QymEngine
