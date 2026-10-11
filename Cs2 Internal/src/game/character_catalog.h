#pragma once
#include <string>
#include <vector>
namespace character_catalog
{
    struct Agent
    {
        std::string name, model;
    };
    bool IsAgentPath(const std::string &path);
    bool Installed(const std::string &path);
    std::vector<Agent> Discover();
} // namespace character_catalog
