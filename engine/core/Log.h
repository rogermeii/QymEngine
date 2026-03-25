#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace QymEngine {

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
    LogLevel level;
    std::string message;
};

class Log {
public:
    using Callback = std::function<void(const LogEntry&)>;

    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);

    static void addCallback(Callback cb);

private:
    static void log(LogLevel level, const std::string& msg);

    static std::vector<LogEntry> s_entries;
    static std::vector<Callback> s_callbacks;
    static std::mutex s_mutex;
};

} // namespace QymEngine
