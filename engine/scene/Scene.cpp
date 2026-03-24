#include "Scene.h"
#include "core/FileUtils.h"
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

void Scene::selectNode(Node* node, bool addToSelection) {
    if (!addToSelection) {
        m_selectedNodes.clear();
    }
    if (node) {
        m_selectedNodes.insert(node);
        m_selectedNode = node;
    }
}

void Scene::deselectNode(Node* node) {
    m_selectedNodes.erase(node);
    if (m_selectedNode == node) {
        m_selectedNode = m_selectedNodes.empty() ? nullptr : *m_selectedNodes.begin();
    }
}

void Scene::clearSelection() {
    m_selectedNodes.clear();
    m_selectedNode = nullptr;
}

void Scene::selectRange(Node* from, Node* to) {
    std::vector<Node*> flat;
    collectFlatList(m_root.get(), flat);

    int idxFrom = -1, idxTo = -1;
    for (int i = 0; i < static_cast<int>(flat.size()); i++) {
        if (flat[i] == from) idxFrom = i;
        if (flat[i] == to) idxTo = i;
    }
    if (idxFrom < 0 || idxTo < 0) return;
    if (idxFrom > idxTo) std::swap(idxFrom, idxTo);

    m_selectedNodes.clear();
    for (int i = idxFrom; i <= idxTo; i++) {
        m_selectedNodes.insert(flat[i]);
    }
    m_selectedNode = to;
}

void Scene::collectFlatList(Node* node, std::vector<Node*>& list) const {
    for (auto& child : node->getChildren()) {
        list.push_back(child.get());
        collectFlatList(child.get(), list);
    }
}

void Scene::removeNode(Node* node) {
    if (!node || node == m_root.get()) return;
    // Recursively clear selection for node and all descendants to prevent dangling pointers
    std::function<void(Node*)> clearDescendants = [&](Node* n) {
        m_selectedNodes.erase(n);
        if (m_selectedNode == n) m_selectedNode = nullptr;
        for (auto& child : n->getChildren())
            clearDescendants(child.get());
    };
    clearDescendants(node);
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
    j["meshType"] = meshTypeToString(node->meshType);
    j["transform"]["position"] = {node->transform.position.x, node->transform.position.y, node->transform.position.z};
    j["transform"]["rotation"] = {node->transform.rotation.x, node->transform.rotation.y, node->transform.rotation.z};
    j["transform"]["scale"] = {node->transform.scale.x, node->transform.scale.y, node->transform.scale.z};
    // NodeType 序列化
    const char* ntStr = "Mesh";
    switch (node->nodeType) {
        case NodeType::DirectionalLight: ntStr = "DirectionalLight"; break;
        case NodeType::PointLight:       ntStr = "PointLight"; break;
        case NodeType::SpotLight:        ntStr = "SpotLight"; break;
        default: break;
    }
    j["nodeType"] = ntStr;
    j["meshPath"] = node->meshPath;
    j["materialPath"] = node->materialPath;
    if (node->isLight()) {
        j["lightColor"] = {node->lightColor.r, node->lightColor.g, node->lightColor.b};
        j["lightIntensity"] = node->lightIntensity;
        if (node->nodeType == NodeType::PointLight || node->nodeType == NodeType::SpotLight)
            j["lightRange"] = node->lightRange;
        if (node->nodeType == NodeType::SpotLight) {
            j["spotInnerAngle"] = node->spotInnerAngle;
            j["spotOuterAngle"] = node->spotOuterAngle;
        }
    }
    j["children"] = json::array();
    for (auto& child : node->getChildren())
        j["children"].push_back(serializeNode(child.get()));
    return j;
}

static void deserializeNode(Node* parent, const json& j, int insertIndex = -1) {
    Node* node = (insertIndex >= 0)
        ? parent->insertChild(j.value("name", "Node"), insertIndex)
        : parent->addChild(j.value("name", "Node"));
    if (j.contains("meshType"))
        node->meshType = meshTypeFromString(j["meshType"].get<std::string>());
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
    if (j.contains("nodeType")) {
        std::string nt = j["nodeType"].get<std::string>();
        if (nt == "DirectionalLight")    node->nodeType = NodeType::DirectionalLight;
        else if (nt == "PointLight")     node->nodeType = NodeType::PointLight;
        else if (nt == "SpotLight")      node->nodeType = NodeType::SpotLight;
        else                             node->nodeType = NodeType::Mesh;
    }
    if (j.contains("meshPath"))
        node->meshPath = j["meshPath"].get<std::string>();
    if (j.contains("materialPath"))
        node->materialPath = j["materialPath"].get<std::string>();
    if (j.contains("lightColor")) {
        auto& c = j["lightColor"];
        node->lightColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
    }
    if (j.contains("lightIntensity"))
        node->lightIntensity = j["lightIntensity"].get<float>();
    if (j.contains("lightRange"))
        node->lightRange = j["lightRange"].get<float>();
    if (j.contains("spotInnerAngle"))
        node->spotInnerAngle = j["spotInnerAngle"].get<float>();
    if (j.contains("spotOuterAngle"))
        node->spotOuterAngle = j["spotOuterAngle"].get<float>();
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
    try {
        std::string content = readFileAsString(path);
        json j = json::parse(content, nullptr, false);
        if (j.is_discarded() || !j.contains("scene")) return;

        m_selectedNode = nullptr;
        m_selectedNodes.clear();
        m_root = std::make_unique<Node>("Root");

        auto& sceneJson = j["scene"];
        name = sceneJson.value("name", "Untitled");
        if (sceneJson.contains("nodes")) {
            for (auto& nodeJson : sceneJson["nodes"])
                deserializeNode(m_root.get(), nodeJson);
        }
    } catch (...) {
        return; // File not found or parse error
    }
}

std::string Scene::serializeNodeToString(Node* node) const {
    return serializeNode(node).dump();
}

Node* Scene::deserializeNodeFromString(const std::string& jsonStr, Node* parent) {
    json j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) return nullptr;
    if (!parent) parent = m_root.get();
    int childCountBefore = static_cast<int>(parent->getChildren().size());
    deserializeNode(parent, j);
    if (static_cast<int>(parent->getChildren().size()) > childCountBefore)
        return parent->getChildren().back().get();
    return nullptr;
}

Node* Scene::deserializeNodeFromString(const std::string& jsonStr, Node* parent, int index) {
    json j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) return nullptr;
    if (!parent) parent = m_root.get();
    int childCountBefore = static_cast<int>(parent->getChildren().size());
    deserializeNode(parent, j, index);
    // Find the newly inserted node
    if (index >= 0 && index < static_cast<int>(parent->getChildren().size()))
        return parent->getChildren()[index].get();
    if (static_cast<int>(parent->getChildren().size()) > childCountBefore)
        return parent->getChildren().back().get();
    return nullptr;
}

std::string Scene::toJsonString() const {
    json j;
    j["scene"]["name"] = name;
    j["scene"]["nodes"] = json::array();
    for (auto& child : m_root->getChildren())
        j["scene"]["nodes"].push_back(serializeNode(child.get()));
    return j.dump();
}

void Scene::fromJsonString(const std::string& jsonStr) {
    json j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded() || !j.contains("scene")) return;

    m_selectedNode = nullptr;
    m_selectedNodes.clear();
    m_root = std::make_unique<Node>("Root");

    auto& sceneJson = j["scene"];
    name = sceneJson.value("name", "Untitled");
    if (sceneJson.contains("nodes")) {
        for (auto& nodeJson : sceneJson["nodes"])
            deserializeNode(m_root.get(), nodeJson);
    }
}

} // namespace QymEngine
