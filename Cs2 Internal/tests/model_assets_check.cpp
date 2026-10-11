#include "render/preview_scene.h"
#include "core/runtime_paths.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 2;
    const auto root = runtime_paths::NativePath(argv[1]);
    std::ifstream input(root / L"index.json");
    const auto index = nlohmann::json::parse(input);
    unsigned failed = 0, agents = 0, weapons = 0;
    for (const auto &[path, file] : index.items())
    {
        try
        {
            const auto name = file.get<std::string>();
            const auto asset = root / std::filesystem::path(std::u8string(name.begin(), name.end()));
            const bool agent = path.starts_with("agents/") || path.starts_with("characters/models/") ||
                               path.starts_with("models/player/") || path.starts_with("models/custom/agents/");
            for (const auto *pose : agent ? std::vector<const char *>{"idle_rifle", "idle_pistol", "idle_knife"}
                                          : std::vector<const char *>{""})
            {
                const auto model =
                    preview_scene::Load(asset, pose, agent, root / L"agents_models_ctm_sas_ctm_sas_vmdl.glb");
                if (agent && (!model.hasHand || !model.posed))
                    throw std::runtime_error("Missing hand / holding pose");
                for (const auto &part : model.parts)
                {
                    if (!part.texture.empty() && !std::filesystem::is_regular_file(runtime_paths::NativePath(part.texture)))
                        throw std::runtime_error("Missing model texture");
                    for (const auto &vertex : part.vertices)
                        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
                            !std::isfinite(vertex.position.z))
                            throw std::runtime_error("Non-finite vertex");
                }
            }
            agent ? ++agents : ++weapons;
        }
        catch (const std::exception &e)
        {
            ++failed;
            std::cerr << path << ": " << e.what() << '\n';
        }
    }
    std::cout << "Checked "Bo << agents << " agents (three poses each), " << weapons << " weapon models; " << failed
              << " failures\n";
    return failed ? 1 : 0;
}
