#pragma once
#include <string>

class Path
{
public:
    static void InitFromArgv(char* argv);

    static std::string GetExecutablePath();
    static std::string GetExecutableDirectory();

    static std::string Combine(const std::string& Dir, const std::string& Name);

private:
    static std::string ExecutablePath;
    static std::string ExecutableDirectory;
};
