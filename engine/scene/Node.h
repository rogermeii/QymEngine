#pragma once
#include "Transform.h"
#include "renderer/MeshLibrary.h"
#include <string>
#include <vector>
#include <memory>

namespace QymEngine {

enum class NodeType { Mesh, DirectionalLight, PointLight, SpotLight };

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

    // Light properties (DirectionalLight / PointLight / SpotLight)
    glm::vec3 lightColor = glm::vec3(1.0f);
    float lightIntensity = 1.0f;
    float lightRange = 10.0f;         // PointLight / SpotLight 衰减半径
    float spotInnerAngle = 25.0f;     // SpotLight 内锥角（度）
    float spotOuterAngle = 35.0f;     // SpotLight 外锥角（度）

    bool isLight() const {
        return nodeType == NodeType::DirectionalLight
            || nodeType == NodeType::PointLight
            || nodeType == NodeType::SpotLight;
    }

    // Get light direction from rotation (forward = -Z rotated by euler angles)
    glm::vec3 getLightDirection() const;

    Node* getParent() const { return m_parent; }
    const std::vector<std::unique_ptr<Node>>& getChildren() const { return m_children; }

    Node* addChild(const std::string& childName);
    Node* insertChild(const std::string& childName, int index);
    void removeChild(Node* child);
    int getChildIndex(Node* child) const;

    glm::mat4 getWorldMatrix() const;

private:
    Node* m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
};

} // namespace QymEngine
