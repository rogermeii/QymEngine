#pragma once
#include "scene/Scene.h"
#include "asset/AssetManager.h"
#include "panels/ModelPreview.h"
#include "panels/ProjectPanel.h"
#include <functional>

namespace QymEngine {
class InspectorPanel {
public:
    using SaveStateFn = std::function<void()>;

    void onImGuiRender(Scene& scene, AssetManager& assetManager, ModelPreview& modelPreview, ProjectPanel& projectPanel);
    void setSaveStateFn(SaveStateFn fn) { m_saveState = fn; }

private:
    SaveStateFn m_saveState;
    bool m_wasDragging = false;      // Transform drag undo tracking
    bool m_wasLightDragging = false;  // Light property drag undo tracking
};
}
