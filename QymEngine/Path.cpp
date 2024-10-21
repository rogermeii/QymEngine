#include "Path.h"

std::string Path::ExecutablePath;
std::string Path::ExecutableDirectory;

void Path::InitFromArgv(char* argv)
{
    ExecutablePath = argv;
    const size_t last_slash_idx = ExecutablePath.rfind('\\');
    if (std::string::npos != last_slash_idx)
    {
        ExecutableDirectory = ExecutablePath.substr(0, last_slash_idx);
    }
}

std::string Path::GetExecutablePath()
{
    return ExecutablePath;
}

std::string Path::GetExecutableDirectory()
{
    return ExecutableDirectory;
}

std::string Path::Combine(const std::string& Dir, const std::string& Name)
{
    if (Dir.empty())
        return Name;
    if (Name.empty())
        return Dir;

    std::string Tmp = Dir;
    if (Tmp[Tmp.length() - 1] != '\\')
        Tmp += '\\';

    return Tmp + Name;
}
