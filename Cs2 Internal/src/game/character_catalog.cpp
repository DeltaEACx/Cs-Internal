#include "platform/windows.h"
#include "game/character_catalog.h"
#include "core/runtime_paths.h"
#include <algorithm>
#include <filesystem>

namespace character_catalog
{
    namespace fs = std::filesystem;
    bool IsAgentPath(const std::string &path)
    {
        if (path.empty() || !path.ends_with(".vmdl") || path.find("..") != std::string::npos ||
            path.find_first_of(":\\\r\n\"") != std::string::npos)
            return false;
        return path.starts_with("agents/") || path.starts_with("characters/models/") ||
               path.starts_with("models/player/") || path.starts_with("models/custom/agents/");
    }
    bool Installed(const std::string &path)
    {
        if (!IsAgentPath(path))
            return false;
        const auto game = runtime_paths::GameDirectory();
        if (game.empty())
            return false;
        const auto value = path + "_c";
        std::error_code ec;
        const auto resolved = fs::weakly_canonical(
            runtime_paths::NativePath(game / fs::path(std::u8string(value.begin(), value.end()))), ec);
        if (ec)
            return false;
        const auto relative = resolved.lexically_relative(fs::weakly_canonical(game, ec));
        if (ec || relative.empty() || *relative.begin() == L"..")
            return false;
        return fs::is_regular_file(resolved, ec);
    }
    std::vector<Agent> Discover()
    {
        std::vector<Agent> result;
        const auto game = runtime_paths::GameDirectory();
        if (game.empty())
            return result;
        for (const auto *folder : {L"agents", L"characters/models", L"models/player", L"models/custom/agents"})
        {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(runtime_paths::NativePath(game / folder), fs::directory_options::skip_permission_denied, ec),
                 end;
                 it != end && !ec; it.increment(ec))
            {
                if (it->path().extension() != L".vmdl_c" || !it->is_regular_file(ec))
                    continue;
                auto path = it->path().lexically_relative(game);
                path.replace_extension(L".vmdl");
                const auto bytes = path.generic_u8string();
                const std::string model(bytes.begin(), bytes.end());
                if (!Installed(model))
                    continue;
                const auto stem = path.stem().u8string();
                std::string name(stem.begin(), stem.end());
                std::replace(name.begin(), name.end(), '_', ' ');
                result.push_back({name, model});
                if (result.size() >= 4096)
                    return result;
            }
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
        return result;
    }
} // namespace character_catalog
