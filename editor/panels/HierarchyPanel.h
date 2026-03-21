#pragma once
#include "scene/Scene.h"
#include <vector>
#include <string>
#include <functional>

namespace QymEngine {

class UndoManager;
class Clipboard;

class HierarchyPanel {
public:
    using SaveStateFn = std::function<void()>;

    void onImGuiRender(Scene& scene, UndoManager* undo = nullptr, Clipboard* clipboard = nullptr);
    void setSaveStateFn(SaveStateFn fn) { m_saveState = fn; }

private:
    void drawNodeTree(Node* node, Scene& scene, int childIndex);
    void drawInsertTarget(Node* parent, int insertIndex, const char* id);

    std::vector<Node*> m_nodesToDelete;
    Node* m_lastClickedNode = nullptr;

    // Deferred reparent
    Node* m_reparentNode = nullptr;
    Node* m_reparentTarget = nullptr;
    int m_reparentIndex = -1;  // -1 = append, >= 0 = insert at index

    SaveStateFn m_saveState;
};

} // namespace QymEngine
