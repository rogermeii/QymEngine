#pragma once
#include "scene/Scene.h"
#include "asset/AssetManager.h"
#include "panels/ModelPreview.h"

namespace QymEngine {
class InspectorPanel {
public:
    void onImGuiRender(Scene& scene, AssetManager& assetManager, ModelPreview& modelPreview);
};
}
