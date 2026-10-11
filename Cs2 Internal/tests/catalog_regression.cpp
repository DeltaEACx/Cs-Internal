// Exercise the real loader, filters, preview decoder and settings queue without CS2.
#include "ui/catalog_ui.cpp"
#include <iostream>
#include <stdexcept>
#include <chrono>

void Check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main()
{
    try
    {
        // File-discovery fixture: no installed Steam library or game writes required.
        wchar_t temporary[32768]{};
        Check(GetTempPathW(32768, temporary) != 0, "Temporary folder unavailable");
        const auto fixture = fs::path(temporary) / (L"CS2-native-test-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(fixture / L"models/custom/banana");
        fs::create_directories(fixture / L"models/player/test_agent");
        std::ofstream(fixture / L"gameinfo.gi") << "test fixture";
        std::ofstream(fixture / L"models/custom/banana/banana.vmdl_c") << "discovery fixture";
        std::ofstream(fixture / L"models/player/test_agent/test_agent.vmdl_c") << "discovery fixture";
        SetEnvironmentVariableW(L"CS2_GAME_DIR", fixture.c_str());
        const auto root = (runtime_paths::DataRoot() / L"game_assets");
        Check(root.filename() == L"game_assets", "Missing helper did not return saved catalog");
        const auto loadStart = std::chrono::steady_clock::now();
        g_data = LoadDataset(root);
        std::cout << "Catalog dataset ready in "
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count()
                  << " ms\n";
        Check(g_data->items.size() == 2059 && g_data->finishes.size() == 2196, "Incomplete saved catalog");
        int knives = 0, previews = 0;
        for (const auto &item : g_data->items)
        {
            if (item.category != "knife")
                continue;
            ++knives;
            Check(custom_models::IsKnife(item.definition), "Catalog knife missing from definition registry");
            Check(item.model == custom_models::StockModel(item.definition), "Stock knife model path mismatch");
            Check(std::any_of(item.finishes.begin(), item.finishes.end(),
                              [&](auto i) { return g_data->finishes[i].paintkit == 0; }),
                  "Knife lacks selectable stock finish");
            for (auto i : item.finishes)
                Check(!g_data->finishes[i].legacy, "Knife selects alternate gun mesh group");
        }
        Check(knives == 23, "Incomplete knife selector");
        for (std::size_t i = 0; i < g_data->items.size(); ++i)
        {
            const auto &item = g_data->items[i];
            if (item.category != "weapon" && item.category != "knife" && item.category != "agent" &&
                item.category != "glove")
                continue;
            const auto image = g_data->imageByKey.find(RowImage({RowKind::Item, i}));
            Check(image != g_data->imageByKey.end(), "Selectable item has no usable thumbnail");
            Check(fs::is_regular_file(runtime_paths::NativePath(root / L"png" / Wide(g_data->images[image->second].relativePng))),
                  "Selectable item thumbnail file missing");
        }
        for (const auto &finish : g_data->finishes)
        {
            const auto image = g_data->imageByKey.find(finish.preferredImage);
            if (image == g_data->imageByKey.end())
                continue;
            Check(fs::is_regular_file(runtime_paths::NativePath(root / L"png" / Wide(g_data->images[image->second].relativePng))),
                  "Finish PNG is missing");
            ++previews;
        }
        g_category = Category::Knives;
        RebuildRows();
        const auto banana = std::find_if(g_rows.begin(), g_rows.end(), [](const auto &row) {
            return row.kind == RowKind::CustomModel &&
                   std::string_view(custom_models::kModels[row.index].id) == "banana";
        });
        Check(banana != g_rows.end() && g_rows.size() == 24, "Banana missing from knife changer");
        const Row bananaRow = *banana;
        skin_changer::Selection choice;
        choice.definition = 500;
        choice.knife = choice.customModel = true;
        choice.model = custom_models::kModels[bananaRow.index].path;
        Check(skin_changer::CustomModelAvailable(choice.model), "Banana compiled asset is not installed");
        Check(skin_changer::Save(choice), "Custom knife rejected");
        skin_changer::Selection loaded;
        Check(skin_changer::GetSelection(42, true, loaded) && loaded.model == choice.model,
              "CT knife does not share the knife selection");
        Check(skin_changer::GetSelection(59, true, loaded) && loaded.definition == 500,
              "T knife does not share the knife selection");
        choice.knife = false;
        Check(!skin_changer::Save(choice), "Custom knife accepted in gun slot");
        choice.knife = true;
        choice.customModel = false;
        choice.model = custom_models::StockModel(500);
        Check(skin_changer::Save(choice), "Cannot switch banana back to stock knife");
        choice.model = "weapons/../../invalid.vmdl";
        Check(!skin_changer::Save(choice), "Unknown knife model accepted");
        choice.definition = 501;
        Check(!skin_changer::Save(choice), "Undefined knife ID accepted");
        choice = {};
        choice.gloves = true;
        choice.definition = 5030;
        choice.paintkit = 10018;
        choice.seed = 71;
        choice.wear = .15f;
        Check(skin_changer::Save(choice), "Glove selection rejected");
        Check(skin_changer::GetCosmetic(false, 0, loaded) && loaded.definition == 5030 && loaded.seed == 71,
              "Glove settings did not round-trip");
        choice.statTrak = true;
        Check(!skin_changer::Save(choice), "Gloves accepted StatTrak");
        choice = {};
        choice.agentModel = true;
        choice.definition = 0;
        choice.customModel = true;
        choice.team = 3;
        choice.model = "models/player/test_agent/test_agent.vmdl";
        Check(skin_changer::Save(choice), "Installed custom agent rejected");
        Check(skin_changer::GetCosmetic(true, 3, loaded) && loaded.model == choice.model, "CT agent settings missing");
        Check(!skin_changer::GetCosmetic(true, 2, loaded), "Agent selection leaked into other team");
        Check(character_catalog::Discover().size() == 1, "Custom agent scan did not find fixture");
        choice.model = "models/player/test_agent/test_agent.mdl";
        Check(!skin_changer::Save(choice), "Source 1 agent accepted");
        choice.model = "models/player/../../outside.vmdl";
        Check(!skin_changer::Save(choice), "Agent traversal accepted");
        const auto agent = std::find_if(g_data->items.begin(), g_data->items.end(),
                                        [](const auto &item) { return item.category == "agent" && item.team == 2; });
        Check(agent != g_data->items.end(), "Catalog agent teams not loaded");
        SelectRow({RowKind::Item, static_cast<std::size_t>(agent - g_data->items.begin())});
        Check(g_openAgent && g_agentChoice.team == 2 && g_agentChoice.model == agent->model,
              "Agent card did not open correct settings");
        g_category = Category::Finishes;
        g_finishDefinition = -1;
        RebuildRows();
        Check(std::count_if(g_rows.begin(), g_rows.end(), [](auto row) { return row.kind == RowKind::Finish; }) == 2196,
              "All skins view drops finishes");
        ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL level{};
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                          device.GetAddressOf(), &level, nullptr)),
              "Cannot create preview test device");
        EnsureDevice(device.Get());
        g_decodeBudget = 2;
        auto *image = GetThumbnail(custom_models::kModels[bananaRow.index].previewKey);
        Check(image && image->view && image->error.empty(), "Banana preview cannot decode");
        const auto &finish = g_data->finishes.front();
        image = GetThumbnail(finish.preferredImage);
        Check(image && image->view && image->error.empty(), "Saved skin preview cannot decode");
        std::cout << "PASS: " << g_data->finishes.size() << " finishes, " << knives << " knives, " << previews
                  << " preview paths; native paths, filtering, knives, gloves, team-specific agents, custom discovery "
                     "and PNG decode\n";
        g_thumbnails.clear();
        g_thumbnailDevice.Reset();

        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';

        return 1;
    }
}
