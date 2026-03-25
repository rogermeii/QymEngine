#include "Log.h"
#include <iostream>

namespace QymEngine {

std::vector<LogEntry> Log::s_entries;
std::vector<Log::Callback> Log::s_callbacks;
std::mutex Log::s_mutex;

void Log::info(const std::string& msg)  { log(LogLevel::Info, msg); }
void Log::warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
void Log::error(const std::string& msg) { log(LogLevel::Error, msg); }

void Log::addCallback(Callback cb)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_callbacks.push_back(std::move(cb));
}

void Log::log(LogLevel level, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(s_mutex);

    LogEntry entry{level, msg};
    s_entries.push_back(entry);

    const char* prefix = "";
    switch (level) {
        case LogLevel::Info:  prefix = "[INFO]";  break;
        case LogLevel::Warn:  prefix = "[WARN]";  break;
        case LogLevel::Error: prefix = "[ERROR]"; break;
    }
    std::cout << prefix << " " << msg << std::endl;

    for (auto& cb : s_callbacks)
        cb(entry);
}

} // namespace QymEngine
