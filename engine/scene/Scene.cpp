#include "Scene.h"
#include <json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace QymEngine {

Scene::Scene() {
    m_root = std::make_unique<Node>("Root");
}

Node* Scene::createNode(const std::string& nodeName, Node* parent) {
    if (!parent) parent = m_root.get();
    return parent->addChild(nodeName);
}

void Scene::removeNode(Node* node) {
    if (!node || node == m_root.get()) return;
    if (m_selectedNode == node) m_selectedNode = nullptr;
    Node* parent = node->getParent();
    if (parent) parent->removeChild(node);
}

void Scene::traverseNodes(const std::function<void(Node*)>& fn) const {
    for (auto& child : m_root->getChildren())
        traverseRecursive(child.get(), fn);
}

void Scene::traverseRecursive(Node* node, const std::function<void(Node*)>& fn) const {
    fn(node);
    for (auto& child : node->getChildren())
        traverseRecursive(child.get(), fn);
}

static json serializeNode(const Node* node) {
    json j;
    j["name"] = node->name;
    j["transform"]["position"] = {node->transform.position.x, node->transform.position.y, node->transform.position.z};
    j["transform"]["rotation"] = {node->transform.rotation.x, node->transform.rotation.y, node->transform.rotation.z};
    j["transform"]["scale"] = {node->transform.scale.x, node->transform.scale.y, node->transform.scale.z};
    j["children"] = json::array();
    for (auto& child : node->getChildren())
        j["children"].push_back(serializeNode(child.get()));
    return j;
}

static void deserializeNode(Node* parent, const json& j) {
    Node* node = parent->addChild(j.value("name", "Node"));
    if (j.contains("transform")) {
        auto& t = j["transform"];
        if (t.contains("position")) {
            auto& p = t["position"];
            node->transform.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
        }
        if (t.contains("rotation")) {
            auto& r = t["rotation"];
            node->transform.rotation = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
        }
        if (t.contains("scale")) {
            auto& s = t["scale"];
            node->transform.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
        }
    }
    if (j.contains("children")) {
        for (auto& childJson : j["children"])
            deserializeNode(node, childJson);
    }
}

void Scene::serialize(const std::string& path) const {
    json j;
    j["scene"]["name"] = name;
    j["scene"]["nodes"] = json::array();
    for (auto& child : m_root->getChildren())
        j["scene"]["nodes"].push_back(serializeNode(child.get()));

    std::ofstream file(path);
    if (file.is_open())
        file << j.dump(2);
}

void Scene::deserialize(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.contains("scene")) return;

    m_selectedNode = nullptr;
    m_root = std::make_unique<Node>("Root");

    auto& sceneJson = j["scene"];
    name = sceneJson.value("name", "Untitled");
    if (sceneJson.contains("nodes")) {
        for (auto& nodeJson : sceneJson["nodes"])
            deserializeNode(m_root.get(), nodeJson);
    }
}

} // namespace QymEngine
