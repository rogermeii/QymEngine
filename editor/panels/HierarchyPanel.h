#pragma once
#include "scene/Scene.h"

namespace QymEngine {
class HierarchyPanel {
public:
    void onImGuiRender(Scene& scene);
private:
    void drawNodeTree(Node* node, Scene& scene);
};
}
