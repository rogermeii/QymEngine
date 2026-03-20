#include "Node.h"
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

} // namespace QymEngine
