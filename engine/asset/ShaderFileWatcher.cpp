#include "asset/ShaderFileWatcher.h"
#include <regex>
#include <fstream>
#include <iostream>
#include <chrono>

namespace fs = std::filesystem;

namespace QymEngine {

ShaderFileWatcher::~ShaderFileWatcher()
{
    stop();
}

void ShaderFileWatcher::start(const std::string& shaderDir)
{
#ifdef __ANDROID__
    // Android APK assets 只读，不支持热重载
    return;
#endif
    if (m_running) return;
    m_shaderDir = shaderDir;

    // 初始扫描：记录所有文件时间戳 + 构建依赖表
    scanAndBuildDeps();

    m_running = true;
    m_thread = std::thread(&ShaderFileWatcher::watchThread, this);
}

void ShaderFileWatcher::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<std::string> ShaderFileWatcher::pollChanges()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pendingChanges.empty())
        return {};
    std::vector<std::string> result(m_pendingChanges.begin(), m_pendingChanges.end());
    m_pendingChanges.clear();
    return result;
}

void ShaderFileWatcher::watchThread()
{
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!m_running) break;

        std::set<std::string> directlyChanged;

        try {
            for (auto& entry : fs::recursive_directory_iterator(m_shaderDir)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".slang") continue;

                std::string relPath = fs::relative(entry.path(), m_shaderDir).string();
                // 统一路径分隔符
                std::replace(relPath.begin(), relPath.end(), '\\', '/');

                auto writeTime = entry.last_write_time();
                auto it = m_lastWriteTimes.find(relPath);
                if (it == m_lastWriteTimes.end()) {
                    // 新文件
                    m_lastWriteTimes[relPath] = writeTime;
                    directlyChanged.insert(relPath);
                    parseIncludes(relPath);
                } else if (it->second != writeTime) {
                    // 修改过的文件
                    it->second = writeTime;
                    directlyChanged.insert(relPath);
                    // 重新解析该文件的依赖
                    parseIncludes(relPath);
                }
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "[ShaderFileWatcher] Error scanning: " << e.what() << std::endl;
        }

        if (!directlyChanged.empty()) {
            // 展开依赖方
            auto allAffected = expandDependents(directlyChanged);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingChanges.insert(allAffected.begin(), allAffected.end());
        }
    }
}

void ShaderFileWatcher::scanAndBuildDeps()
{
    m_lastWriteTimes.clear();
    m_reverseDeps.clear();

    try {
        for (auto& entry : fs::recursive_directory_iterator(m_shaderDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".slang") continue;

            std::string relPath = fs::relative(entry.path(), m_shaderDir).string();
            std::replace(relPath.begin(), relPath.end(), '\\', '/');

            m_lastWriteTimes[relPath] = entry.last_write_time();
            parseIncludes(relPath);
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[ShaderFileWatcher] Error initial scan: " << e.what() << std::endl;
    }
}

void ShaderFileWatcher::parseIncludes(const std::string& relPath)
{
    // 先清除该文件旧的依赖记录
    for (auto& [dep, dependents] : m_reverseDeps) {
        dependents.erase(relPath);
    }

    std::string fullPath = m_shaderDir + "/" + relPath;
    std::ifstream file(fullPath);
    if (!file.is_open()) return;

    // 匹配 #include "xxx" 和 import xxx
    std::regex includeRegex(R"REGEX(#include\s+"([^"]+)")REGEX");
    std::regex importRegex(R"REGEX(import\s+(\S+)\s*;)REGEX");

    std::string line;
    while (std::getline(file, line)) {
        std::smatch match;
        std::string depFile;

        if (std::regex_search(line, match, includeRegex)) {
            depFile = match[1].str();
        } else if (std::regex_search(line, match, importRegex)) {
            // import 语句的模块名转为文件路径
            depFile = match[1].str();
            std::replace(depFile.begin(), depFile.end(), '.', '/');
            depFile += ".slang";
        }

        if (!depFile.empty()) {
            // 规范化依赖路径（相对于 shaderDir）
            std::replace(depFile.begin(), depFile.end(), '\\', '/');
            // 如果是相对于当前文件的路径，转换为相对于 shaderDir 的路径
            auto parentDir = fs::path(relPath).parent_path().string();
            std::replace(parentDir.begin(), parentDir.end(), '\\', '/');
            std::string depRelPath = parentDir.empty() ? depFile : parentDir + "/" + depFile;

            // 注册反向依赖：depRelPath 被修改时，relPath 也需要重编
            m_reverseDeps[depRelPath].insert(relPath);
        }
    }
}

std::set<std::string> ShaderFileWatcher::expandDependents(const std::set<std::string>& changed)
{
    std::set<std::string> result = changed;
    std::set<std::string> queue = changed;

    // BFS 展开传递依赖
    while (!queue.empty()) {
        std::set<std::string> next;
        for (auto& file : queue) {
            auto it = m_reverseDeps.find(file);
            if (it != m_reverseDeps.end()) {
                for (auto& dependent : it->second) {
                    if (result.find(dependent) == result.end()) {
                        result.insert(dependent);
                        next.insert(dependent);
                    }
                }
            }
        }
        queue = std::move(next);
    }
    return result;
}

} // namespace QymEngine
