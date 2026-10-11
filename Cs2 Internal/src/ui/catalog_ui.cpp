#include "platform/windows.h"
#include "ui/catalog_ui.h"
#include "ui/gui_capture.h"

#include "core/runtime_paths.h"
#include "game/character_catalog.h"
#include "features/cosmetics/skin_changer.h"
#include "game/game_access.h"
#include "game/custom_models.h"
#include "imgui/imgui.h"
#include "nlohmann/json.hpp"

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    std::string g_previewAgent;
    bool g_agentPreviewRequested = false;
    using Json = nlohmann::json;
    using Microsoft::WRL::ComPtr;
    namespace fs = std::filesystem;
    constexpr std::size_t kThumbnailLimit = 96;
    constexpr UINT kMaximumThumbnailDimension = 512;
    constexpr int kDecodesPerFrame = 4;

    std::wstring Wide(const std::string &value)
    {
        if (value.empty())
            return {};
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
        if (!size)
            return {};
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            size);
        return result;
    }
    std::string Lower(std::string value)
    {
        for (char &character : value)
            if (static_cast<unsigned char>(character) < 128)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return value;
    }
    std::string Text(const Json &object, const char *field)
    {
        const auto found = object.find(field);
        return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
    }
    int Integer(const Json &object, const char *field, int fallback = -1)
    {
        const auto found = object.find(field);
        if (found == object.end() || !found->is_number_integer())
            return fallback;
        const auto value = found->get<std::int64_t>();
        return value >= INT_MIN && value <= INT_MAX ? static_cast<int>(value) : fallback;
    }
    std::vector<std::string> Strings(const Json &array)
    {
        std::vector<std::string> values;
        if (array.is_array())
            for (const auto &value : array)
                if (value.is_string())
                    values.push_back(value.get<std::string>());
        return values;
    }

    struct ImageRecord
    {
        std::string key, relativePng;
    };
    struct ItemRecord
    {
        int definition = -1, team = 0;
        std::string name, schemaName, category, type, rarity, model, search;
        std::vector<std::string> imageKeys;
        std::vector<std::size_t> finishes;
    };
    struct FinishRecord
    {
        int definition = -1, paintkit = -1;
        bool legacy = false;
        std::string name, category, rarity, preferredImage, search;
        std::unordered_map<std::string, std::vector<std::string>> images;
    };
    struct Dataset
    {
        fs::path root;
        std::vector<ItemRecord> items;
        std::vector<FinishRecord> finishes;
        std::vector<ImageRecord> images;
        std::unordered_map<int, std::size_t> itemById;
        std::unordered_map<std::string, std::size_t> imageByKey;
        int paintkitCount = 0;
    };
    struct Thumbnail
    {
        ComPtr<ID3D11ShaderResourceView> view;
        UINT width = 0, height = 0;
        std::uint64_t lastFrame = 0;
        std::string error;
    };
    enum class Category
    {
        Guns,
        Knives,
        Agents,
        Gloves,
        Finishes,
        CustomAgents
    };
    enum class RowKind
    {
        Item,
        Finish,
        CustomModel,
        CustomAgent
    };
    struct Row
    {
        RowKind kind = RowKind::Item;
        std::size_t index = 0;
    };

    std::unique_ptr<Dataset> g_data, g_pending;
    std::thread g_loader;
    std::mutex g_loadMutex;
    std::atomic<bool> g_loading{false};
    std::string g_pendingError, g_error;
    std::unordered_map<std::string, Thumbnail> g_thumbnails;
    ComPtr<ID3D11Device> g_thumbnailDevice;
    std::uint64_t g_frame = 0;
    int g_decodeBudget = 0, g_page = 0, g_pageSize = 24;
    Category g_category = Category::Guns;
    int g_finishDefinition = -1;
    bool g_started = false, g_filterDirty = true, g_hasSelection = false;
    bool g_liveAvailable = false, g_onlyLive = false;
    std::unordered_set<int> g_liveIds;
    char g_search[256]{};
    char g_assetFolder[32768]{};
    Row g_selected;
    std::vector<Row> g_rows;
    struct Navigation
    {
        Category category;
        int page, definition;
        std::string search;
    };
    std::vector<Navigation> g_history;
    int g_restorePage = -1;
    bool g_openSkin = false, g_popupStatTrak = false, g_popupCustom = false;
    int g_popupKills = 0;
    int g_popupSeed = 0;
    float g_popupWear = 0.01f;
    std::size_t g_popupFinish = 0;
    std::string g_popupError;
    std::vector<character_catalog::Agent> g_customAgents;
    skin_changer::Selection g_agentChoice;
    bool g_openAgent = false, g_customAgentsScanned = false;
    bool g_captureSkinOpen = false, g_captureAgentOpen = false;
    Category g_tabCategory = Category::Guns;

    // Capture has its own modal visibility: a schema scan must never open an
    // equip dialog until the user selects a catalog row. Native popups retain
    // their usual ImGui lifetime and input behavior.
    class CatalogModal
    {
    public:
        CatalogModal(const char *label, bool &requested, bool &captureOpen, ImVec2 size)
            : captured_(gui_capture::active != nullptr), captureOpen_(captureOpen)
        {
            if (requested)
            {
                if (captured_) captureOpen_ = true;
                else ImGui::OpenPopup(label);
                requested = false;
            }
            if (captured_)
            {
                if (!captureOpen_) return;
                ImGui::PushID(label);
                gui_capture::active->BeginGroup(label, "modal");
                visible_ = true;
                return;
            }
            ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
            bool open = true;
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(.075f, .070f, .080f, 1.0f));
            visible_ = ImGui::BeginPopupModal(label, &open, ImGuiWindowFlags_NoCollapse);
            ImGui::PopStyleColor();
        }
        ~CatalogModal()
        {
            if (!visible_) return;
            if (captured_)
            {
                gui_capture::active->EndGroup();
                ImGui::PopID();
            }
            else ImGui::EndPopup();
        }
        explicit operator bool() const { return visible_; }
        void Close()
        {
            if (captured_) captureOpen_ = false;
            else ImGui::CloseCurrentPopup();
        }
    private:
        bool captured_, visible_ = false;
        bool &captureOpen_;
    };

    fs::path NormalizeRoot(fs::path root)
    {
        root = runtime_paths::NativePath(root);
        std::error_code error;
        if (fs::is_regular_file(root / L"item_catalog.json", error))
            return root;
        if (fs::is_regular_file(root / L"game_assets" / L"item_catalog.json", error))
            return root / L"game_assets";
        if (root.filename() == L"png" && fs::is_regular_file(root.parent_path() / L"item_catalog.json", error))
            return root.parent_path();
        return root;
    }
    Json ReadJson(const fs::path &path)
    {
        const auto file = runtime_paths::NativePath(path);
        std::error_code error;
        const auto size = fs::file_size(file, error);
        if (error || size > 128ull * 1024 * 1024)
            throw std::runtime_error("Catalog file is missing or too large.");
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Catalog file could not be opened.");
        return Json::parse(stream);
    }
    bool SafeRelative(const std::string &value)
    {
        const fs::path path(Wide(value));
        if (value.empty() || path.empty() || path.is_absolute() || path.has_root_name())
            return false;
        for (const auto &component : path)
            if (component == L".." || component.native().find(L':') != std::wstring::npos)
                return false;
        return true;
    }
    std::unique_ptr<Dataset> LoadDataset(const fs::path &chosenRoot)
    {
        auto data = std::make_unique<Dataset>();
        data->root = NormalizeRoot(chosenRoot);
        const Json catalog = ReadJson(data->root / L"item_catalog.json");
        if (Integer(catalog, "schema_version") != 1 || !catalog.contains("item_definitions") ||
            !catalog["item_definitions"].is_array() || !catalog.contains("compatible_combinations") ||
            !catalog["compatible_combinations"].is_array())
            throw std::runtime_error("Unsupported item catalog format.");
        const auto kits = catalog.find("paintkits");
        data->paintkitCount = kits != catalog.end() && kits->is_array() ? static_cast<int>(kits->size()) : 0;
        std::unordered_set<int> legacyKits;
        if (kits != catalog.end() && kits->is_array())
            for (const auto &kit : *kits)
            {
                const auto legacy = kit.find("use_legacy_model");
                if (legacy != kit.end() && (*legacy == true || *legacy == 1 || *legacy == "1"))
                    legacyKits.insert(Integer(kit, "paintkit_id"));
            }
        std::error_code error;
        const bool fullIndex = fs::is_regular_file(data->root / L"image_index.json", error);
        const Json index = fullIndex ? ReadJson(data->root / L"image_index.json") : catalog;
        const auto imageList = index.find("images");
        if (imageList == index.end() || !imageList->is_array())
            throw std::runtime_error("Image index is missing.");
        for (const auto &raw : *imageList)
        {
            if (!raw.is_object())
                continue;
            ImageRecord image;
            image.key = Text(raw, "archive_path");
            image.relativePng = Text(raw, "png_path");
            if (image.key.empty() || !SafeRelative(image.relativePng) || data->imageByKey.count(image.key))
                continue;
            data->imageByKey.emplace(image.key, data->images.size());
            data->images.push_back(std::move(image));
        }
        for (const auto &raw : catalog["item_definitions"])
        {
            if (!raw.is_object())
                continue;
            ItemRecord item;
            item.definition = Integer(raw, "definition_index");
            if (item.definition < 0 || data->itemById.count(item.definition))
                continue;
            item.name = Text(raw, "name");
            item.schemaName = Text(raw, "schema_name");
            item.category = Text(raw, "category");
            item.type = Text(raw, "type");
            item.rarity = Text(raw, "item_rarity");
            item.model = Text(raw, "model_player");
            if (const auto fields = raw.find("raw_fields"); fields != raw.end() && fields->contains("used_by_classes"))
            {
                const auto &teams = (*fields)["used_by_classes"];
                if (teams.contains("counter-terrorists"))
                    item.team = 3;
                else if (teams.contains("terrorists"))
                    item.team = 2;
            }
            if (const auto found = raw.find("image_paths"); found != raw.end())
                item.imageKeys = Strings(*found);
            item.search =
                Lower(item.name + " " + item.schemaName + " " + item.type + " " + std::to_string(item.definition));
            data->itemById.emplace(item.definition, data->items.size());
            data->items.push_back(std::move(item));
        }
        for (const auto &raw : catalog["compatible_combinations"])
        {
            if (!raw.is_object())
                continue;
            FinishRecord finish;
            finish.definition = Integer(raw, "definition_index");
            finish.paintkit = Integer(raw, "paintkit_id");
            finish.legacy = legacyKits.count(finish.paintkit) != 0 && !custom_models::IsKnife(finish.definition);
            const auto owner = data->itemById.find(finish.definition);
            if (owner == data->itemById.end() || finish.paintkit < 0)
                continue;
            finish.name = Text(raw, "name");
            finish.category = Text(raw, "category");
            finish.rarity = Text(raw, "paintkit_rarity");
            finish.preferredImage = Text(raw, "preferred_image_path");
            if (const auto found = raw.find("images"); found != raw.end() && found->is_object())
                for (const auto &[variant, list] : found->items())
                    finish.images[variant] = Strings(list);
            finish.search =
                Lower(finish.name + " " + std::to_string(finish.definition) + " " + std::to_string(finish.paintkit));
            data->items[owner->second].finishes.push_back(data->finishes.size());
            data->finishes.push_back(std::move(finish));
        }
        return data;
    }
    void BeginLoad(const fs::path &root)
    {
        if (g_loading)
            return;
        if (g_loader.joinable())
            g_loader.join();
        g_loading = true;
        g_error.clear();
        try
        {
            g_loader = std::thread([root]() {
                std::unique_ptr<Dataset> pending;
                std::string error;
                try
                {
                    pending = LoadDataset(root.empty() ? (runtime_paths::DataRoot() / L"game_assets") : root);
                }
                catch (const std::exception &exception)
                {
                    error = exception.what();
                }
                {
                    std::lock_guard<std::mutex> lock(g_loadMutex);
                    g_pending = std::move(pending);
                    g_pendingError = std::move(error);
                }
                g_loading = false;
            });
        }
        catch (const std::exception &)
        {
            g_loading = false;
            g_error = "Catalog loading thread could not start.";
        }
    }
    void PollLoad()
    {
        if (g_loading || !g_loader.joinable())
            return;
        g_loader.join();
        std::lock_guard<std::mutex> lock(g_loadMutex);
        if (g_pending)
        {
            g_data = std::move(g_pending);
            g_thumbnails.clear();
            g_filterDirty = true;
            g_hasSelection = false;
            g_page = 0;
            g_finishDefinition = -1;
            g_history.clear();
            g_openSkin = false;
            g_openAgent = false;
            g_captureSkinOpen = g_captureAgentOpen = false;
        }
        g_error = std::move(g_pendingError);
    }

    struct ComApartment
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComApartment()
        {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
        bool ready() const
        {
            return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        }
    };
    bool DecodeThumbnail(ID3D11Device *device, const fs::path &path, Thumbnail &thumbnail)
    {
        const auto file = runtime_paths::NativePath(path);
        ComApartment apartment;
        if (!device || !apartment.ready())
        {
            thumbnail.error = "Image decoder unavailable";
            return false;
        }
        std::error_code error;
        const auto fileSize = fs::file_size(file, error);
        if (error || fileSize > 128ull * 1024 * 1024)
        {
            thumbnail.error = "PNG file unavailable";
            return false;
        }
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf()))) ||
            FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
        {
            thumbnail.error = "PNG could not be decoded";
            return false;
        }
        UINT width = 0, height = 0;
        if (FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 32768 || height > 32768)
        {
            thumbnail.error = "Image dimensions unsupported";
            return false;
        }
        const double scale =
            (std::min)(1.0, static_cast<double>(kMaximumThumbnailDimension) / (std::max)(width, height));
        thumbnail.width = (std::max)(1u, static_cast<UINT>(std::round(width * scale)));
        thumbnail.height = (std::max)(1u, static_cast<UINT>(std::round(height * scale)));
        IWICBitmapSource *source = frame.Get();
        if (width != thumbnail.width || height != thumbnail.height)
        {
            if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
                FAILED(
                    scaler->Initialize(frame.Get(), thumbnail.width, thumbnail.height, WICBitmapInterpolationModeFant)))
            {
                thumbnail.error = "Image resize failed";
                return false;
            }
            source = scaler.Get();
        }
        if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
            FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
        {
            thumbnail.error = "PNG format unsupported";
            return false;
        }
        const UINT stride = thumbnail.width * 4;
        std::vector<unsigned char> pixels(static_cast<std::size_t>(stride) * thumbnail.height);
        if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
        {
            thumbnail.error = "PNG pixels unavailable";
            return false;
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = thumbnail.width;
        description.Height = thumbnail.height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(), stride, 0};
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&description, &initial, texture.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, thumbnail.view.GetAddressOf())))
        {
            thumbnail.error = "Preview texture unavailable";
            return false;
        }
        return true;
    }
    void EnsureDevice(ID3D11Device *device)
    {
        if (g_thumbnailDevice.Get() == device)
            return;
        g_thumbnails.clear();
        g_thumbnailDevice = device;
    }
    Thumbnail *GetThumbnail(const std::string &key)
    {
        if (!g_data || !g_thumbnailDevice)
            return nullptr;
        auto found = g_thumbnails.find(key);
        if (found != g_thumbnails.end())
        {
            found->second.lastFrame = g_frame;
            return &found->second;
        }
        if (g_decodeBudget <= 0)
            return nullptr;
        fs::path customPreview;
        if (const auto *model = custom_models::FindPreview(key))
            customPreview = runtime_paths::DataRoot() / Wide(model->previewFile);
        else if (key.starts_with("@agent/") && character_catalog::IsAgentPath(key.substr(7)))
        {
            auto name = key.substr(7);
            for (char &c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    c = '_';
            customPreview = runtime_paths::DataRoot() / L"preview_models/thumbnails" / Wide(name + ".png");
            auto sidecar = runtime_paths::GameDirectory() / Wide(key.substr(7));
            sidecar.replace_extension(L".png");
            std::error_code ec;
            if (fs::is_regular_file(runtime_paths::NativePath(sidecar), ec))
                customPreview = sidecar;
        }
        if (!customPreview.empty())
        {
            if (g_thumbnails.size() >= kThumbnailLimit)
            {
                auto oldest = g_thumbnails.end();
                for (auto candidate = g_thumbnails.begin(); candidate != g_thumbnails.end(); ++candidate)
                    if (candidate->second.lastFrame != g_frame &&
                        (oldest == g_thumbnails.end() || candidate->second.lastFrame < oldest->second.lastFrame))
                        oldest = candidate;
                if (oldest == g_thumbnails.end())
                    return nullptr;
                g_thumbnails.erase(oldest);
            }
            --g_decodeBudget;
            Thumbnail preview;
            preview.lastFrame = g_frame;
            DecodeThumbnail(g_thumbnailDevice.Get(), customPreview, preview);
            return &g_thumbnails.emplace(key, std::move(preview)).first->second;
        }
        const auto image = g_data->imageByKey.find(key);
        if (image == g_data->imageByKey.end())
            return nullptr;
        if (g_thumbnails.size() >= kThumbnailLimit)
        {
            auto oldest = g_thumbnails.end();
            for (auto candidate = g_thumbnails.begin(); candidate != g_thumbnails.end(); ++candidate)
                if (candidate->second.lastFrame != g_frame &&
                    (oldest == g_thumbnails.end() || candidate->second.lastFrame < oldest->second.lastFrame))
                    oldest = candidate;
            // Never release an SRV already referenced by this frame's draw list.
            if (oldest == g_thumbnails.end())
                return nullptr;
            g_thumbnails.erase(oldest);
        }
        --g_decodeBudget;
        Thumbnail thumbnail;
        thumbnail.lastFrame = g_frame;
        DecodeThumbnail(g_thumbnailDevice.Get(),
                        g_data->root / L"png" / Wide(g_data->images[image->second].relativePng), thumbnail);
        return &g_thumbnails.emplace(key, std::move(thumbnail)).first->second;
    }
    const char *RowName(const Row &row)
    {
        if (row.kind == RowKind::CustomAgent)
            return g_customAgents[row.index].name.c_str();
        if (row.kind == RowKind::CustomModel)
            return custom_models::kModels[row.index].name;
        if (row.kind == RowKind::Item)
            return g_data->items[row.index].name.c_str();
        if (row.kind == RowKind::Finish)
            return g_data->finishes[row.index].name.c_str();
        return "";
    }
    std::string RowImage(const Row &row)
    {
        if (row.kind == RowKind::CustomAgent)
            return "@agent/" + g_customAgents[row.index].model;
        if (row.kind == RowKind::CustomModel)
        {
            const auto &model = custom_models::kModels[row.index];
            // Agent models (definitions 100-102) lack preview images; skip image rendering
            if (model.definition >= 100 && model.definition <= 102)
                return std::string{};
            return model.previewKey;
        }
        if (row.kind == RowKind::Finish)
            return g_data->finishes[row.index].preferredImage;
        const auto &item = g_data->items[row.index];
        for (const auto &key : item.imageKeys)
            if (g_data->imageByKey.count(key))
                return key;
        // Glove definitions have no base thumbnail; use an available finish.
        for (const auto index : item.finishes)
        {
            const auto &key = g_data->finishes[index].preferredImage;
            if (g_data->imageByKey.count(key))
                return key;
        }
        return {};
    }
    void RebuildRows()
    {
        g_rows.clear();
        if (!g_data)
            return;
        const std::string search = Lower(g_search);
        const auto matches = [&search](const std::string &text) {
            return search.empty() || text.find(search) != std::string::npos;
        };
        const auto live = [](int id) { return !g_liveAvailable || !g_onlyLive || g_liveIds.count(id) != 0; };
        const auto addCustom = [&]() {
            for (std::size_t index = 0; index < custom_models::kModels.size(); ++index)
            {
                const auto &model = custom_models::kModels[index];
                if (model.definition >= 100 && model.definition <= 102)
                    continue; // Replaced by actual installed-agent discovery.
                const std::string text = Lower(std::string(model.name) + " " + model.style + " " + model.weapon +
                                               " custom model " + model.id + " " + std::to_string(model.definition));
                const bool inSchema = g_data->itemById.count(model.definition);
                const bool modelMatch = g_category == Category::Knives
                                            ? custom_models::IsKnife(model.definition)
                                            : (g_finishDefinition < 0 || g_finishDefinition == model.definition);
                if (modelMatch && inSchema && live(model.definition) && matches(text))
                    g_rows.push_back({RowKind::CustomModel, index});
            }
        };
        const char *categories[] = {"weapon", "knife", "agent", "glove"};
        if (g_category <= Category::Gloves)
        {
            if (g_category == Category::Knives)
                addCustom();
            for (std::size_t index = 0; index < g_data->items.size(); ++index)
            {
                const auto &item = g_data->items[index];
                const bool categoryMatch = item.category == categories[static_cast<int>(g_category)];
                if (categoryMatch && live(item.definition) && matches(item.search))
                    g_rows.push_back({RowKind::Item, index});
            }
        }
        else if (g_category == Category::Finishes)
        {
            addCustom();
            for (std::size_t index = 0; index < g_data->finishes.size(); ++index)
            {
                const auto &finish = g_data->finishes[index];
                if ((g_finishDefinition < 0 || finish.definition == g_finishDefinition) && live(finish.definition) &&
                    matches(finish.search))
                    g_rows.push_back({RowKind::Finish, index});
            }
        }
        else if (g_category == Category::CustomAgents)
        {
            for (std::size_t i = 0; i < g_customAgents.size(); ++i)
                if (matches(Lower(g_customAgents[i].name + " " + g_customAgents[i].model)))
                    g_rows.push_back({RowKind::CustomAgent, i});
        }
        g_page = g_restorePage >= 0 ? g_restorePage : 0;
        g_restorePage = -1;
        g_filterDirty = false;
    }
    void PushNavigation()
    {
        g_history.push_back({g_category, g_page, g_finishDefinition, g_search});
    }
    void Back()
    {
        if (g_history.empty())
            return;
        const auto previous = g_history.back();
        g_history.pop_back();
        g_category = previous.category;
        g_restorePage = previous.page;
        g_finishDefinition = previous.definition;
        strncpy_s(g_search, previous.search.c_str(), _TRUNCATE);
        g_hasSelection = false;
        g_filterDirty = true;
    }
    void SelectRow(const Row &row)
    {
        g_captureSkinOpen = g_captureAgentOpen = false;
        g_selected = row;
        g_hasSelection = true;
        if (row.kind == RowKind::CustomAgent)
        {
            const auto &agent = g_customAgents[row.index];
            g_agentChoice = {};
            g_agentChoice.agentModel = g_agentChoice.customModel = true;
            g_agentChoice.team = skin_changer::CurrentTeam() == 2 ? 2 : 3;
            g_previewAgent = agent.model;
            g_agentChoice.name = agent.name;
            g_agentChoice.model = agent.model;
            g_openAgent = true;
            g_popupError.clear();
            return;
        }
        if (row.kind == RowKind::Item)
        {
            const auto &item = g_data->items[row.index];
            if (item.category == "agent")
            {
                g_agentChoice = {};
                g_agentChoice.agentModel = true;
                g_agentChoice.definition = item.definition;
                g_previewAgent = item.model;
                g_agentChoice.team = item.team ? item.team : 3;
                g_agentChoice.name = item.name;
                g_agentChoice.model = item.model;
                g_openAgent = true;
                g_popupError.clear();
                return;
            }
            if (!item.finishes.empty() || item.category == "weapon" || item.category == "knife")
            {
                PushNavigation();
                g_category = Category::Finishes;
                g_finishDefinition = item.definition;
                g_search[0] = '\0';
                g_filterDirty = true;
                g_hasSelection = false;
            }
        }
        else if (row.kind == RowKind::Finish || row.kind == RowKind::CustomModel)
        {
            g_popupCustom = row.kind == RowKind::CustomModel;
            g_popupFinish = row.index;
            g_popupError.clear();
            skin_changer::Selection choice;
            const int definition =
                g_popupCustom ? custom_models::kModels[row.index].definition : g_data->finishes[row.index].definition;
            const bool knife = custom_models::IsKnife(definition);
            const bool glove = !g_popupCustom && g_data->finishes[row.index].category == "glove";
            const bool saved = glove ? skin_changer::GetCosmetic(false, 0, choice)
                                     : skin_changer::GetSelection(definition, knife, choice);
            g_popupStatTrak = saved && choice.statTrak;
            g_popupKills = saved ? choice.kills : 0;
            g_popupSeed = saved ? choice.seed : 0;
            g_popupWear = saved ? choice.wear : 0.01f;
            g_openSkin = true;
        }
    }
    std::string CaptureImagePath(const std::string &key)
    {
        if (!g_data || key.empty()) return {};
        fs::path path;
        if (const auto *model = custom_models::FindPreview(key))
            path = runtime_paths::DataRoot() / Wide(model->previewFile);
        else if (key.starts_with("@agent/") && character_catalog::IsAgentPath(key.substr(7)))
        {
            auto name = key.substr(7);
            for (char &c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    c = '_';
            path = runtime_paths::DataRoot() / L"preview_models/thumbnails" / Wide(name + ".png");
            auto sidecar = runtime_paths::GameDirectory() / Wide(key.substr(7));
            sidecar.replace_extension(L".png");
            std::error_code error;
            if (fs::is_regular_file(runtime_paths::NativePath(sidecar), error)) path = sidecar;
        }
        else
        {
            const auto found = g_data->imageByKey.find(key);
            if (found == g_data->imageByKey.end()) return {};
            const auto &relative = g_data->images[found->second].relativePng;
            if (!SafeRelative(relative)) return {};
            path = g_data->root / L"png" / Wide(relative);
        }
        const auto utf8 = runtime_paths::NativePath(path).u8string();
        return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
    }
    bool ImageButton(const std::string &key, ImVec2 size, bool selected)
    {
        Thumbnail *thumbnail = GetThumbnail(key);
        const bool clicked = ImGui::InvisibleButton("##preview", size);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(minimum, maximum,
                            hovered ? IM_COL32(39, 24, 29, 255) : IM_COL32(20, 20, 24, 255), 5.0f);
        if (thumbnail && thumbnail->view)
        {
            const float scale = (std::min)(size.x / thumbnail->width, size.y / thumbnail->height);
            const ImVec2 drawn(thumbnail->width * scale, thumbnail->height * scale);
            const ImVec2 origin(minimum.x + (size.x - drawn.x) * 0.5f, minimum.y + (size.y - drawn.y) * 0.5f);
            draw->AddImage(reinterpret_cast<ImTextureID>(thumbnail->view.Get()), origin,
                           ImVec2(origin.x + drawn.x, origin.y + drawn.y));
        }
        else
        {
            const char *status = key.empty() || thumbnail ? "No preview" : "Loading...";
            draw->AddText(ImVec2(minimum.x + 8.0f, minimum.y + 8.0f), IM_COL32(153, 145, 149, 255), status);
        }
        draw->AddRect(minimum, maximum, selected ? IM_COL32(200, 48, 66, 255) :
                      hovered ? IM_COL32(115, 48, 60, 255) : IM_COL32(48, 43, 48, 255), 5.0f, 0,
                      selected ? 2.0f : 1.0f);
        if (selected)
            draw->AddLine(ImVec2(minimum.x + 12.f, maximum.y - 1.f),
                          ImVec2(maximum.x - 12.f, maximum.y - 1.f), IM_COL32(225, 62, 79, 255), 2.f);
        if (thumbnail && !thumbnail->error.empty() && hovered)
            ImGui::SetTooltip("%s", thumbnail->error.c_str());
        return clicked;
    }
    void DrawGrid(float width, float height)
    {
        if (gui_capture::active)
        {
            gui_capture::active->BeginGroup("Collection", "cards");
            const std::size_t begin = static_cast<std::size_t>(g_page) * g_pageSize;
            const std::size_t end = (std::min)(g_rows.size(), begin + g_pageSize);
            for (std::size_t position = begin; position < end; ++position)
            {
                const Row row = g_rows[position];
                ImGui::PushID(static_cast<int>(row.kind));
                ImGui::PushID(static_cast<int>(row.index));
                const bool selected = g_hasSelection && row.kind == g_selected.kind && row.index == g_selected.index;
                const auto path = CaptureImagePath(RowImage(row));
                if (gui_capture::active->Card(RowName(row), path.c_str(), selected, false)) SelectRow(row);
                ImGui::PopID();
                ImGui::PopID();
            }
            gui_capture::active->EndGroup();
            return;
        }
        if (ImGui::BeginChild("##catalog_grid", ImVec2(width, height), false))
        {
            const float available = ImGui::GetContentRegionAvail().x;
            const int columns = (std::max)(1, static_cast<int>(available / 180.0f));
            const float cardWidth =
                (std::max)(80.0f, (available - (columns - 1) * ImGui::GetStyle().ItemSpacing.x) / columns);
            constexpr float imageHeight = 116.f;
            const float cardHeight = imageHeight + ImGui::GetTextLineHeight() +
                                     ImGui::GetStyle().ItemSpacing.y + 20.f;
            const std::size_t begin = static_cast<std::size_t>(g_page) * g_pageSize;
            const std::size_t end = (std::min)(g_rows.size(), begin + g_pageSize);
            for (std::size_t position = begin; position < end; ++position)
            {
                const Row row = g_rows[position];
                ImGui::PushID(static_cast<int>(position));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,10));
                ImGui::BeginChild("##card", ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);
                const bool selected = g_hasSelection && row.kind == g_selected.kind && row.index == g_selected.index;
                if (ImageButton(RowImage(row), ImVec2((std::max)(1.0f, ImGui::GetContentRegionAvail().x), imageHeight),
                                selected))
                {
                    SelectRow(row);
                }
                ImGui::TextUnformatted(RowName(row));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", RowName(row));
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopID();
                if ((position - begin + 1) % columns && position + 1 != end)
                    ImGui::SameLine();
            }
        }
        ImGui::EndChild();
    }

    void DrawSkinPopup()
    {
        CatalogModal modal("Skin settings", g_openSkin, g_captureSkinOpen, {460, 520});
        if (!modal) return;
        if ((g_popupCustom && g_popupFinish >= custom_models::kModels.size()) ||
            (!g_popupCustom && g_popupFinish >= g_data->finishes.size()))
        {
            modal.Close();
            return;
        }
        const auto *customModel = g_popupCustom ? &custom_models::kModels[g_popupFinish] : nullptr;
        FinishRecord custom;
        if (customModel)
        {
            custom.definition = customModel->definition;
            custom.paintkit = 0;
            custom.category = custom_models::IsKnife(custom.definition) ? "knife" : "weapon";
            custom.name = customModel->name;
            custom.preferredImage = customModel->previewKey;
        }
        const auto &finish = g_popupCustom ? custom : g_data->finishes[g_popupFinish];
        const auto owner = g_data->itemById.find(finish.definition);
        if (owner == g_data->itemById.end())
        {
            modal.Close();
            return;
        }
        const auto &item = g_data->items[owner->second];
        ImGui::TextWrapped("%s", finish.name.c_str());
        if (gui_capture::active)
        {
            const auto path = CaptureImagePath(finish.preferredImage);
            gui_capture::active->Card(finish.name.c_str(), path.c_str(), false, true);
        }
        else ImageButton(finish.preferredImage, ImVec2(ImGui::GetContentRegionAvail().x, 190.0f), false);
        const bool applicable = item.category == "weapon" || item.category == "knife" || item.category == "glove";
        if (applicable)
        {
            if (!customModel)
            {
                ImGui::SliderFloat("Wear", &g_popupWear, 0.0f, 1.0f, "%.4f");
                ImGui::SliderInt("Seed", &g_popupSeed, 0, 1000);
            }
            if (item.category != "glove")
            {
                ImGui::Checkbox("StatTrak", &g_popupStatTrak);
                if (g_popupStatTrak)
                {
                    ImGui::SetNextItemWidth(180.0f);
                    ImGui::InputInt("Kills", &g_popupKills, 1, 100);
                    g_popupKills = (std::max)(0, g_popupKills);
                }
            }
            if (ImGui::Button("Equip finish", ImVec2(120.0f, 0.0f)))
            {
                skin_changer::Selection choice;
                choice.definition = item.definition;
                choice.paintkit = finish.paintkit;
                choice.name = finish.name;
                choice.knife = item.category == "knife";
                choice.gloves = item.category == "glove";
                choice.customModel = g_popupCustom;
                choice.model = customModel ? customModel->path : item.model;
                choice.legacy = finish.legacy;
                choice.statTrak = !choice.gloves && g_popupStatTrak;
                choice.kills = g_popupKills;
                choice.wear = g_popupWear;
                choice.seed = g_popupSeed;
                g_popupError = skin_changer::Save(choice) ? ""
                               : customModel && !skin_changer::CustomModelAvailable(choice.model)
                                   ? "Model not installed. Install the model pack."
                                   : "Could not save skin.";
                if (gui_capture::active && !g_popupError.empty()) gui_capture::active->ReportError(g_popupError.c_str());
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
            modal.Close();
        if (!applicable)
            ImGui::TextDisabled("Preview only");
        if (!g_popupError.empty())
            ImGui::TextWrapped("%s", g_popupError.c_str());
        if (g_popupError.empty() && customModel && !skin_changer::CustomModelAvailable(customModel->path))
            ImGui::TextWrapped("Model not installed. Install the model pack.");
    }

    void DrawKnifeChanger()
    {
        static int definition = 500;
        static std::string error;
        const auto selected = g_data->itemById.find(definition);
        const char *name =
            selected == g_data->itemById.end() ? "Select knife" : g_data->items[selected->second].name.c_str();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Knife model", name))
        {
            for (const auto &item : g_data->items)
            {
                if (item.category != "knife")
                    continue;
                ImGui::PushID(item.definition);
                if (ImGui::Selectable(item.name.c_str(), item.definition == definition))
                    definition = item.definition;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Apply stock knife"))
        {
            skin_changer::Selection choice;
            choice.definition = definition;
            choice.knife = true;
            if (const char *model = custom_models::StockModel(definition))
                choice.model = model;
            error = skin_changer::Save(choice) ? "" : "Could not save knife.";
            if (gui_capture::active && !error.empty()) gui_capture::active->ReportError(error.c_str());
        }
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        ImGui::Separator();
    }
    void DrawQueuedAgentSelections()
    {
        for (const int team : {3, 2})
        {
            skin_changer::Selection choice;
            const auto *label = team == 3 ? "CT" : "T";
            if (!skin_changer::GetCosmetic(true, team, choice))
            {
                ImGui::TextDisabled("%s  /  Default", label);
                continue;
            }
            const auto name = choice.name.empty() ? catalog_ui::AgentName(choice.model) : choice.name;
            ImGui::TextWrapped("%s  /  %s", label, name.c_str());
        }
        ImGui::Separator();
    }
    void DrawAgentPopup()
    {
        CatalogModal modal("Agent settings", g_openAgent, g_captureAgentOpen, {460, 300});
        if (!modal) return;
        ImGui::TextWrapped("%s", g_agentChoice.name.c_str());
        if (g_agentChoice.customModel)
        {
            ImGui::RadioButton("Counter-Terrorists", &g_agentChoice.team, 3);
            ImGui::SameLine();
            ImGui::RadioButton("Terrorists", &g_agentChoice.team, 2);
        }
        else
            ImGui::TextDisabled("%s", g_agentChoice.team == 3 ? "Counter-Terrorists" : "Terrorists");
        if (ImGui::Button("Equip agent"))
        {
            g_popupError = skin_changer::Save(g_agentChoice) ? "" : "Could not save agent. Check model and team.";
            if (gui_capture::active && !g_popupError.empty()) gui_capture::active->ReportError(g_popupError.c_str());
        }
        ImGui::SameLine();
        if (gui_capture::active)
        {
            if (ImGui::Button("Preview agent")) catalog_ui::PreviewAgent(g_agentChoice.model);
            ImGui::SameLine();
        }
        if (ImGui::Button("Back to items"))
            modal.Close();
        if (!g_popupError.empty())
            ImGui::TextWrapped("%s", g_popupError.c_str());
    }
} // namespace

std::string catalog_ui::AgentName(const std::string &model)
{
    if (g_data)
        for (const auto &item : g_data->items)
            if (item.category == "agent" && game_access::SameModelPath(item.model, model))
                return item.name;
    for (const auto &agent : g_customAgents)
        if (game_access::SameModelPath(agent.model, model))
            return agent.name;
    return fs::path(model).stem().string();
}
std::string catalog_ui::SelectedAgentPreview()
{
    return g_previewAgent;
}
void catalog_ui::PreviewAgent(const std::string &model)
{
    if (character_catalog::IsAgentPath(model))
    {
        g_previewAgent = model;
        g_agentPreviewRequested = true;
    }
}
bool catalog_ui::ConsumeAgentPreviewRequest()
{
    const bool requested = g_agentPreviewRequested;
    g_agentPreviewRequested = false;
    return requested;
}
void catalog_ui::PreviewEquippedAgent()
{
    g_previewAgent.clear();
}
std::string catalog_ui::WeaponName(int definition)
{
    if (g_data)
        if (const auto found = g_data->itemById.find(definition); found != g_data->itemById.end())
            return g_data->items[found->second].name;
    return "Weapon " + std::to_string(definition);
}
void catalog_ui::DrawEquippedFinish(ID3D11Device *device, int definition, int paintkit, const std::string &model)
{
    g_frame = static_cast<std::uint64_t>(ImGui::GetFrameCount());
    EnsureDevice(device);
    if (!g_data)
        return;
    if (const auto *custom = custom_models::Find(model))
    {
        g_decodeBudget = (std::max)(g_decodeBudget, 1);
        if (auto *image = GetThumbnail(custom->previewKey); image && image->view)
        {
            ImGui::Image(image->view.Get(), ImVec2(96, 64));
            ImGui::SameLine();
        }
        ImGui::TextWrapped("%s", custom->name);
        return;
    }
    const FinishRecord *finish = nullptr;
    for (const auto &entry : g_data->finishes)
        if (entry.definition == definition && entry.paintkit == paintkit)
        {
            finish = &entry;
            break;
        }
    if (!finish)
        return;
    g_decodeBudget = (std::max)(g_decodeBudget, 1);
    if (auto *image = GetThumbnail(finish->preferredImage); image && image->view)
    {
        const float height = 64.f, width = height * image->width / image->height;
        ImGui::Image(image->view.Get(), ImVec2(width, height));
        ImGui::SameLine();
    }
    ImGui::TextWrapped("%s", finish->name.c_str());
}

void catalog_ui::Draw(ID3D11Device *device, Page page)
{
    if ((!device && !gui_capture::active) || !ImGui::GetCurrentContext())
        return;
    if (!gui_capture::active) EnsureDevice(device);
    g_frame = static_cast<std::uint64_t>(ImGui::GetFrameCount());
    g_decodeBudget = kDecodesPerFrame;
    if (!g_started)
    {
        g_started = true;
        BeginLoad({});
    }
    PollLoad();
    if (g_loading)
        ImGui::TextUnformatted("Loading...");
    if (!g_error.empty())
    {
        ImGui::TextWrapped("Catalog could not load: %s", g_error.c_str());
        if (ImGui::Button("Retry") && !g_loading)
            BeginLoad({});
        ImGui::InputTextWithHint("##AssetFolder", "Path to the extracted asset folder", g_assetFolder,
                                 sizeof(g_assetFolder));
        ImGui::SameLine();
        if (ImGui::Button("Load assets") && !g_loading)
        {
            std::string folder = g_assetFolder;
            const auto first = folder.find_first_not_of(" \t\r\n\"");
            const auto last = folder.find_last_not_of(" \t\r\n\"");
            folder = first == std::string::npos ? std::string{} : folder.substr(first, last - first + 1);
            std::string message;
            if (runtime_paths::UseDataRoot(fs::path(Wide(folder)), &message))
                BeginLoad({});
            else
            {
                g_error = std::move(message);
                if (gui_capture::active) gui_capture::active->ReportError(g_error.c_str());
            }
        }
    }
    if (!g_data)
        return;

    const Category categories[]={Category::Guns,Category::Knives,Category::Gloves,Category::Agents,Category::CustomAgents};
    const auto category=categories[std::clamp(static_cast<int>(page),0,4)];
    if(g_tabCategory!=category)
    {
        g_tabCategory=g_category=category;
        g_finishDefinition=-1;
        g_search[0]='\0';
        g_filterDirty=true;
        g_hasSelection=false;
        g_history.clear();
        g_openSkin=g_openAgent=false;
        g_captureSkinOpen=g_captureAgentOpen=false;
    }
    if (!g_history.empty())
    {
        if (ImGui::Button("Back to items"))
            Back();
        ImGui::SameLine();
    }
    if (g_category == Category::Finishes && g_finishDefinition >= 0)
    {
        const auto owner = g_data->itemById.find(g_finishDefinition);
        ImGui::Text("%s / Skins",
                    owner != g_data->itemById.end() ? g_data->items[owner->second].name.c_str() : "Weapon");
    }
    if (g_category == Category::CustomAgents)
    {
        if (!g_customAgentsScanned || ImGui::Button("Refresh"))
        {
            g_customAgents = character_catalog::Discover();
            g_customAgentsScanned = true;
            g_filterDirty = true;
        }
    }
    if (g_category == Category::Knives)
        DrawKnifeChanger();
    if (g_category == Category::Agents || g_category == Category::CustomAgents)
        DrawQueuedAgentSelections();
    if (gui_capture::active && ImGui::Button("Preview equipped"))
    {
        PreviewEquippedAgent();
        g_agentPreviewRequested = true;
    }
    ImGui::SetNextItemWidth((std::max)(100.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputTextWithHint("##catalog_search", "Search collection...", g_search, sizeof(g_search)))
        g_filterDirty = true;
    if (g_liveAvailable)
    {
        if (ImGui::Checkbox("Compatible only", &g_onlyLive))
            g_filterDirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only show items supported by the current game build.");
    }
    if (g_filterDirty)
        RebuildRows();
    const int pageCount = (std::max)(1, static_cast<int>((g_rows.size() + g_pageSize - 1) / g_pageSize));
    g_page = (std::clamp)(g_page, 0, pageCount - 1);
    if (ImGui::SmallButton("Previous") && g_page > 0)
        --g_page;
    ImGui::SameLine();
    if (ImGui::SmallButton("Next") && g_page + 1 < pageCount)
        ++g_page;
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", g_page + 1, pageCount);
    if (g_rows.empty())
        ImGui::TextUnformatted(
            g_category == Category::CustomAgents && g_customAgents.empty() ? "No custom agents found." : "No results.");
    DrawGrid(0.0f, (std::max)(100.0f, ImGui::GetContentRegionAvail().y));
    DrawSkinPopup();
    DrawAgentPopup();
}

void catalog_ui::SetLiveDefinitionIndices(const int *indices, std::size_t count)
{
    std::unordered_set<int> updated;
    if (indices)
        for (std::size_t index = 0; index < count; ++index)
            updated.insert(indices[index]);
    const bool available = indices != nullptr;
    if (g_liveAvailable == available && g_liveIds == updated)
        return;
    g_liveIds = std::move(updated);
    g_liveAvailable = available;
    g_filterDirty = true;
}

void catalog_ui::Shutdown()
{
    g_previewAgent.clear();
    g_agentPreviewRequested = false;
    if (g_loader.joinable())
        g_loader.join();
    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        g_pending.reset();
        g_pendingError.clear();
    }
    g_thumbnails.clear();
    g_thumbnailDevice.Reset();
    g_data.reset();
    g_rows.clear();
    g_history.clear();
    g_restorePage = -1;
    g_openSkin = false;
    g_openAgent = false;
    g_captureSkinOpen = g_captureAgentOpen = false;
    g_customAgentsScanned = false;
    g_customAgents.clear();
    g_tabCategory = g_category = Category::Guns;
    g_error.clear();
    g_loading = false;
    g_started = false;
    g_hasSelection = false;
    g_filterDirty = true;
    g_liveIds.clear();
    g_liveAvailable = g_onlyLive = false;
    g_page = 0;
    g_finishDefinition = -1;
}
