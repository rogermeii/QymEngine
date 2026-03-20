#pragma once
#include "Transform.h"
#include "renderer/MeshLibrary.h"
#include <string>
#include <vector>
#include <memory>

namespace QymEngine {

class Node {
public:
    explicit Node(const std::string& name = "Node");

    std::string name;
    Transform transform;
    MeshType meshType = MeshType::Cube;
    std::string meshPath;     // empty = use built-in meshType
    std::string texturePath;  // empty = use default texture

    Node* getParent() const { return m_parent; }
    const std::vector<std::unique_ptr<Node>>& getChildren() const { return m_children; }

    Node* addChild(const std::string& childName);
    void removeChild(Node* child);

    glm::mat4 getWorldMatrix() const;

private:
    Node* m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
};

} // namespace QymEngine
