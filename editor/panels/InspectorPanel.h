#pragma once
#include "scene/Scene.h"
#include "asset/AssetManager.h"
#include "panels/ModelPreview.h"
#include "panels/ProjectPanel.h"

namespace QymEngine {
class InspectorPanel {
public:
    void onImGuiRender(Scene& scene, AssetManager& assetManager, ModelPreview& modelPreview, ProjectPanel& projectPanel);
};
}
