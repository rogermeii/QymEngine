#include "Node.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace QymEngine {

Node::Node(const std::string& name) : name(name) {}

Node* Node::addChild(const std::string& childName) {
    auto child = std::make_unique<Node>(childName);
    child->m_parent = this;
    Node* ptr = child.get();
    m_children.push_back(std::move(child));
    return ptr;
}

void Node::removeChild(Node* child) {
    m_children.erase(
        std::remove_if(m_children.begin(), m_children.end(),
            [child](const std::unique_ptr<Node>& c) { return c.get() == child; }),
        m_children.end());
}

glm::mat4 Node::getWorldMatrix() const {
    glm::mat4 local = transform.getLocalMatrix();
    if (m_parent)
        return m_parent->getWorldMatrix() * local;
    return local;
}

glm::vec3 Node::getLightDirection() const {
    glm::vec3 r = glm::radians(transform.rotation);
    glm::mat4 rot = glm::mat4(1.0f);
    rot = glm::rotate(rot, r.y, glm::vec3(0, 1, 0));
    rot = glm::rotate(rot, r.x, glm::vec3(1, 0, 0));
    rot = glm::rotate(rot, r.z, glm::vec3(0, 0, 1));
    return glm::normalize(glm::vec3(rot * glm::vec4(0, 0, -1, 0)));
}

} // namespace QymEngine
