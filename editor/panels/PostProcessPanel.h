#pragma once
#include "scene/Scene.h"

namespace QymEngine {

class Renderer;

class PostProcessPanel {
public:
    void onImGuiRender(Scene& scene);
    static void setRenderer(Renderer* r);
};

} // namespace QymEngine
