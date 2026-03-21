#pragma once
#include "Transform.h"
#include "renderer/MeshLibrary.h"
#include <string>
#include <vector>
#include <memory>

namespace QymEngine {

enum class NodeType { Mesh, DirectionalLight };

class Node {
public:
    explicit Node(const std::string& name = "Node");

    std::string name;
    Transform transform;
    NodeType nodeType = NodeType::Mesh;

    // Mesh properties (only used when nodeType == Mesh)
    MeshType meshType = MeshType::Cube;
    std::string meshPath;
    std::string materialPath;

    // Light properties (only used when nodeType == DirectionalLight)
    glm::vec3 lightColor = glm::vec3(1.0f);
    float lightIntensity = 1.0f;

    // Get light direction from rotation (forward = -Z rotated by euler angles)
    glm::vec3 getLightDirection() const;

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
