#pragma once
#include <filesystem>
#include <string>

namespace runtime_paths
{
    // Normalize the final joined path before filesystem or Win32 I/O (including long/UNC paths).
    std::filesystem::path NativePath(std::filesystem::path path);
    std::filesystem::path ModuleDirectory();
    std::filesystem::path DataRoot();
    // Accept a package, cs2_assets, or game_assets folder and remember it for this user.
    bool UseDataRoot(const std::filesystem::path &root, std::string *error = nullptr);
    std::filesystem::path GameDirectory();
} // namespace runtime_paths
