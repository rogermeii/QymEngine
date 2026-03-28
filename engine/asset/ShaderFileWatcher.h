#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>

namespace QymEngine {

/// 监视 .slang 文件变化，支持依赖追踪的增量重编
class ShaderFileWatcher {
public:
    ShaderFileWatcher() = default;
    ~ShaderFileWatcher();

    /// 启动监视（指定 shaders 根目录）
    void start(const std::string& shaderDir);

    /// 停止监视
    void stop();

    /// 主线程调用：返回自上次 poll 以来需要重编的 .slang 文件列表（含依赖方）
    /// 返回的是相对于 shaderDir 的路径（如 "Shadow.slang", "postprocess/Composite.slang"）
    std::vector<std::string> pollChanges();

    /// 是否正在运行
    bool isRunning() const { return m_running; }

private:
    void watchThread();
    void scanAndBuildDeps();
    void parseIncludes(const std::string& slangFile);
    std::set<std::string> expandDependents(const std::set<std::string>& changed);

    std::string m_shaderDir;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // 文件修改时间记录
    std::map<std::string, std::filesystem::file_time_type> m_lastWriteTimes;

    // 反向依赖表：被依赖文件 → 依赖它的文件集合
    std::map<std::string, std::set<std::string>> m_reverseDeps;

    // 变化队列（watchThread 写，主线程读）
    std::mutex m_mutex;
    std::set<std::string> m_pendingChanges;
};

} // namespace QymEngine
