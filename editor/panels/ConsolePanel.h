#pragma once

#include "core/Log.h"
#include <vector>

namespace QymEngine {

class ConsolePanel {
public:
    void init();
    void onImGuiRender();

private:
    std::vector<LogEntry> m_logs;
    bool m_autoScroll = true;
};

} // namespace QymEngine
