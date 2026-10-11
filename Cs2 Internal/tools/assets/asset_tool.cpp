#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include "nlohmann/json.hpp"
#include "core/runtime_paths.h"

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;
void Check(bool ok, const char *text)
{
    if (!ok)
        throw std::runtime_error(text);
}
void Hr(HRESULT hr)
{
    Check(SUCCEEDED(hr), "Image conversion failed.");
}
fs::path Utf8(const std::string &text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}
std::string Text(const fs::path &path)
{
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
Json Read(const fs::path &path)
{
    std::ifstream in(runtime_paths::NativePath(path));
    Check(bool(in), "Required asset manifest is missing.");
    return Json::parse(in);
}
void JsonFile(const fs::path &input, const Json &data)
{
    const auto path = runtime_paths::NativePath(input);
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << data.dump(2);
    Check(bool(out), "Could not write manifest.");
}
fs::path Relative(const std::string &text)
{
    auto path = Utf8(text);
    Check(!path.empty() && !path.has_root_path(), "Asset path must be relative.");
    for (const auto &part : path)
        Check(part != L".." && part.native().find(L':') == std::wstring::npos, "Asset path escapes package.");
    return path.make_preferred();
}
void Copy(const fs::path &source, const fs::path &destination)
{
    const auto from = runtime_paths::NativePath(source), to = runtime_paths::NativePath(destination);
    Check(fs::is_regular_file(from), "Required package file is missing.");
    fs::create_directories(to.parent_path());
    if (fs::weakly_canonical(from) == fs::weakly_canonical(to))
        return;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    fs::last_write_time(to, fs::last_write_time(from));
}
void Thumbnail(const fs::path &source, const fs::path &destination, UINT maxDimension, bool opaque = false)
{
    const auto from = runtime_paths::NativePath(source), to = runtime_paths::NativePath(destination);
    Check(fs::is_regular_file(from), "Required image is missing from the source assets.");
    fs::create_directories(to.parent_path());
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> format, unmasked;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> output;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    Hr(factory->CreateDecoderFromFilename(from.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                          &decoder));
    Hr(decoder->GetFrame(0, &frame));
    UINT width = 0, height = 0;
    Hr(frame->GetSize(&width, &height));
    Check(width && height, "Empty image.");
    const float scale = std::min(1.f, float(maxDimension) / std::max(width, height));
    width = std::max(1u, UINT(width * scale));
    height = std::max(1u, UINT(height * scale));
    Hr(factory->CreateBitmapScaler(&scaler));
    if (opaque)
    {
        Hr(factory->CreateFormatConverter(&unmasked));
        Hr(unmasked->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
    }
    Hr(scaler->Initialize(opaque ? static_cast<IWICBitmapSource *>(unmasked.Get())
                                 : static_cast<IWICBitmapSource *>(frame.Get()),
                          width, height, WICBitmapInterpolationModeFant));
    Hr(factory->CreateFormatConverter(&format));
    Hr(format->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeCustom));
    Hr(factory->CreateStream(&stream));
    Hr(stream->InitializeFromFilename(to.c_str(), GENERIC_WRITE));
    Hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    Hr(encoder->CreateNewFrame(&output, nullptr));
    Hr(output->Initialize(nullptr));
    Hr(output->SetSize(width, height));
    auto pixelFormat = GUID_WICPixelFormat32bppBGRA;
    Hr(output->SetPixelFormat(&pixelFormat));
    Hr(output->WriteSource(format.Get(), nullptr));
    Hr(output->Commit());
    Hr(encoder->Commit());
}
std::uint64_t Hash(const fs::path &path)
{
    std::ifstream in(runtime_paths::NativePath(path), std::ios::binary);
    Check(bool(in), "Cannot read package resource.");
    std::uint64_t hash = 14695981039346656037ull;
    char buffer[65536];
    while (in)
    {
        in.read(buffer, sizeof(buffer));
        for (std::streamsize i = 0; i < in.gcount(); ++i)
        {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    Check(!in.bad(), "Could not finish reading package resource.");
    return hash;
}
fs::path Executable()
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    Check(length && length < 32768, "Could not locate AssetTool.exe.");
    return runtime_paths::NativePath(executable);
}
void WriteText(const fs::path &path, const char *text)
{
    std::ofstream out(runtime_paths::NativePath(path), std::ios::binary);
    out << text;
    Check(bool(out), "Could not write setup instructions.");
}
void Verify(const fs::path &input)
{
    const auto package = runtime_paths::NativePath(input);
    const auto manifest = Read(package / L"package-manifest.json");
    Check(manifest.at("version") == 1 && manifest.at("hash_algorithm") == "fnv1a64",
          "Unsupported package manifest.");
    std::set<fs::path> checked;
    const auto root = fs::weakly_canonical(package);
    for (const auto &file : manifest.at("files"))
    {
        const auto relative = Relative(file.at("path").get<std::string>());
        Check(checked.insert(relative).second, "Duplicate package manifest entry.");
        const auto path = runtime_paths::NativePath(package / relative);
        const auto resolved = fs::weakly_canonical(path).lexically_relative(root);
        Check(!resolved.empty() && *resolved.begin() != L"..", "Package resource escapes its folder.");
        if (!fs::is_regular_file(path) || fs::file_size(path) != file.at("bytes").get<std::uintmax_t>() ||
            Hash(path) != file.at("hash").get<std::uint64_t>())
            throw std::runtime_error("Package verification failed: " + Text(relative) +
                                     ". Extract the complete ZIP again.");
    }
    for (const auto *required : {L"DeltaEAC.dll", L"AssetTool.exe", L"Setup.cmd", L"cs2_assets/game_assets/item_catalog.json",
                                 L"cs2_assets/game_assets/image_index.json", L"cs2_assets/preview_models/index.json",
                                 L"cs2_assets/game_files/index.json"})
        Check(checked.count(fs::path(required).make_preferred()) != 0, "Package manifest omits a required file.");
    std::cout << "Verified " << checked.size() << " package files.\n";
}
void Pack(const fs::path &sourceInput, const fs::path &outputInput, const fs::path &dllInput)
{
    const auto source = runtime_paths::NativePath(sourceInput), output = runtime_paths::NativePath(outputInput),
               dll = runtime_paths::NativePath(dllInput);
    Check(!fs::exists(output) || (fs::is_directory(output) && fs::is_empty(output)),
          "Package output must be an empty folder. Choose a new output folder.");
    Check(fs::is_regular_file(dll), "The freshly built DLL is missing.");
    const auto assets = fs::is_regular_file(runtime_paths::NativePath(source / L"game_assets/item_catalog.json")) ? source
                        : fs::is_regular_file(runtime_paths::NativePath(source / L"cs2_assets/game_assets/item_catalog.json"))
                            ? source / L"cs2_assets"
                            : source.parent_path() / L"cs2_assets";
    const auto target = output / L"cs2_assets";
    auto catalog = Read(assets / L"game_assets/item_catalog.json"),
         index = Read(assets / L"game_assets/image_index.json");
    std::set<std::string> keys;
    for (const auto &item : catalog.at("item_definitions"))
        for (const auto &key : item.at("image_paths"))
            keys.insert(key.get<std::string>());
    for (const auto &finish : catalog.at("compatible_combinations"))
    {
        keys.insert(finish.value("preferred_image_path", std::string{}));
        if (finish.contains("images"))
            for (const auto &[variant, images] : finish["images"].items())
                for (const auto &image : images)
                    keys.insert(image.get<std::string>());
    }
    Json images = Json::array();
    unsigned count = 0;
    for (auto image : index.at("images"))
    {
        if (!keys.count(image.value("archive_path", std::string{})))
            continue;
        const auto relative = Relative(image.at("png_path").get<std::string>());
        const auto from = runtime_paths::NativePath(assets / L"game_assets/png" / relative);
        if (!fs::is_regular_file(from))
            throw std::runtime_error("Catalog image is missing: " + Text(from));
        Thumbnail(from, target / L"game_assets/png" / relative, 512);
        images.push_back(image);
        ++count;
        if (count % 250 == 0)
            std::cout << "Catalog thumbnails: " << count << std::endl;
    }
    index["images"] = images;
    index["image_count"] = images.size();
    index["exported_image_count"] = images.size();
    index["extracted_image_count"] = images.size();
    JsonFile(target / L"game_assets/image_index.json", index);
    JsonFile(target / L"game_assets/item_catalog.json", catalog);
    const auto preview = Read(assets / L"preview_models/index.json");
    std::set<fs::path> textures;
    for (const auto &[model, name] : preview.items())
    {
        const auto relative = Relative(name.get<std::string>()),
                   file = runtime_paths::NativePath(assets / L"preview_models" / relative);
        std::ifstream in(file, std::ios::binary);
        std::uint32_t header[5]{};
        in.read(reinterpret_cast<char *>(header), 20);
        Check(in.good() && header[0] == 0x46546c67 && header[3] < 32 * 1024 * 1024, "Invalid bundled GLB.");
        std::string text(header[3], '\0');
        in.read(text.data(), text.size());
        Check(in.good(), "Truncated GLB.");
        const auto glb = Json::parse(text);
        Copy(file, target / L"preview_models" / relative);
        if (glb.contains("images"))
            for (const auto &image : glb["images"])
                if (image.contains("uri"))
                    textures.insert(Relative(image["uri"].get<std::string>()));
    }
    count = 0;
    for (const auto &texture : textures)
    {
        Thumbnail(assets / L"preview_models" / texture, target / L"preview_models" / texture, 512, true);
        if (++count % 100 == 0)
            std::cout << "Model textures: " << count << '/' << textures.size() << std::endl;
    }
    JsonFile(target / L"preview_models/index.json", preview);
    const auto agentThumbnails = runtime_paths::NativePath(assets / L"preview_models/thumbnails");
    if (fs::is_directory(agentThumbnails))
        for (const auto &file : fs::directory_iterator(agentThumbnails))
            if (file.is_regular_file() && file.path().extension() == L".png")
                Copy(file.path(), target / L"preview_models/thumbnails" / file.path().filename());
    for (fs::recursive_directory_iterator it(assets / L"custom_models"), end; it != end; ++it)
    {
        if (it.depth() > 3)
        {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() &&
            (it->path().filename() == L"preview.png" || it->path().filename() == L"ak74_wings_preview.png"))
            Thumbnail(it->path(), target / it->path().lexically_relative(assets), 512);
    }
    const auto gameSource = fs::is_directory(assets / L"game_files")
                                ? assets / L"game_files"
                                : runtime_paths::NativePath(assets / L"custom_models/ak74_wings/engine/game/csgo");
    Json resources = Json::array();
    for (const auto *prefix : {L"models/custom", L"materials/custom"})
        for (fs::recursive_directory_iterator it(runtime_paths::NativePath(gameSource / prefix)), end; it != end; ++it)
            if (it->is_regular_file() && it->path().extension().native().ends_with(L"_c"))
            {
                const auto relative = it->path().lexically_relative(gameSource);
                Copy(it->path(), target / L"game_files" / relative);
                resources.push_back(
                    {{"path", Text(relative)}, {"bytes", fs::file_size(it->path())}, {"hash", Hash(it->path())}});
            }
    JsonFile(target / L"game_files/index.json", resources);
    Copy(dll, output / L"DeltaEAC.dll");
    Copy(Executable(), output / L"AssetTool.exe");
    WriteText(output / L"Setup.cmd", "@echo off\r\nsetlocal\r\n\"%~dp0AssetTool.exe\" --setup\r\n"
                                   "set \"setup_status=%errorlevel%\"\r\necho.\r\npause\r\nexit /b %setup_status%\r\n");
    WriteText(output / L"README.txt",
              "1. Extract the whole ZIP to a permanent folder. Do not run from inside the ZIP.\r\n"
              "2. Double-click Setup.cmd once for each Windows user. It registers this folder and installs custom resources into CS2.\r\n"
              "3. Use DeltaEAC.dll from this folder. Keep cs2_assets and AssetTool.exe with it.\r\n"
              "If you move the folder, run Setup.cmd again. No global environment variable is needed.\r\n"
              "If Steam detection fails, run: AssetTool.exe --setup \"D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\"\r\n"
              "Check the extracted files with: AssetTool.exe --verify\r\n"
              "Requires 64-bit Windows and the supported CS2 game version.\r\n");
    Json files = Json::array();
    for (const auto &entry : fs::recursive_directory_iterator(output))
        if (entry.is_regular_file())
            files.push_back({{"path", Text(entry.path().lexically_relative(output))},
                             {"bytes", entry.file_size()}, {"hash", Hash(entry.path())}});
    JsonFile(output / L"package-manifest.json", {{"version", 1}, {"hash_algorithm", "fnv1a64"}, {"files", files}});
    Verify(output);
    std::cout << "Packaged " << images.size() << " catalog images, " << preview.size() << " 3D models and "
              << resources.size() << " custom resources.\n";
}
fs::path FindGame()
{
    wchar_t configured[32768]{};
    if (GetEnvironmentVariableW(L"CS2_GAME_DIR", configured, 32768) &&
        fs::is_regular_file(runtime_paths::NativePath(fs::path(configured) / L"gameinfo.gi")))
        return runtime_paths::NativePath(configured);
    wchar_t steam[32768]{};
    DWORD bytes = sizeof(steam);
    Check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam,
                       &bytes) == ERROR_SUCCESS,
          "Steam not found. Pass the game/csgo folder as an argument.");
    std::vector<fs::path> libraries{runtime_paths::NativePath(steam)};
    std::ifstream in(runtime_paths::NativePath(fs::path(steam) / L"steamapps/libraryfolders.vdf"));
    std::string text((std::istreambuf_iterator<char>(in)), {});
    const std::regex pattern("\"path\"\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
    {
        auto value = (*it)[1].str();
        size_t pos = 0;
        while ((pos = value.find("\\\\", pos)) != std::string::npos)
            value.replace(pos, 2, "\\");
        libraries.push_back(runtime_paths::NativePath(Utf8(value)));
    }
    for (const auto &library : libraries)
    {
        const auto game = runtime_paths::NativePath(library / L"steamapps/common/Counter-Strike Global Offensive/game/csgo");
        if (fs::is_regular_file(game / L"gameinfo.gi"))
            return game;
    }
    throw std::runtime_error("CS2 not found in Steam libraries. Pass the game/csgo folder as an argument.");
}
void Install(const fs::path &packageInput, const fs::path &gameInput)
{
    const auto package = runtime_paths::NativePath(packageInput), game = runtime_paths::NativePath(gameInput);
    Check(fs::is_regular_file(game / L"gameinfo.gi"), "Target must be the CS2 game/csgo folder.");
    const auto origin = runtime_paths::NativePath(package / L"cs2_assets/game_files");
    const auto resources = Read(origin / L"index.json");
    // Validate the whole plan before writing any destination.
    for (const auto &item : resources)
    {
        const auto name = item.at("path").get<std::string>();
        const auto relative = Relative(name);
        Check(name.starts_with("models/custom/") || name.starts_with("materials/custom/"),
              "Unexpected install namespace.");
        Check(relative.extension().native().ends_with(L"_c"), "Only compiled resources may be installed.");
        Check(fs::file_size(origin / relative) == item.at("bytes").get<std::uintmax_t>() &&
                  Hash(origin / relative) == item.at("hash").get<std::uint64_t>(),
              "Package resource failed verification.");
        const auto resolved = fs::weakly_canonical(game / relative), root = fs::weakly_canonical(game);
        const auto below = resolved.lexically_relative(root);
        Check(!below.empty() && *below.begin() != L"..", "Install path escaped game directory.");
    }
    PWSTR local = nullptr;
    Hr(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local));
    const auto backup = fs::path(local) / L"CS2Internal/backups" / std::to_wstring(GetTickCount64());
    CoTaskMemFree(local);
    for (const auto &item : resources)
    {
        const auto relative = Relative(item.at("path"));
        const auto target = game / relative;
        if (fs::is_regular_file(target) && Hash(target) != item.at("hash").get<std::uint64_t>())
            Copy(target, backup / relative);
        Copy(origin / relative, target);
        Check(Hash(target) == item.at("hash").get<std::uint64_t>(), "Installed resource failed readback.");
    }
    std::wcout << L"Installed " << resources.size() << L" custom resources into " << game << L'\n';
}
int wmain(int argc, wchar_t **argv)
{
    try
    {
        Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if ((argc == 4 || argc == 5) && std::wstring_view(argv[1]) == L"--package")
            Pack(fs::absolute(argv[2]), fs::absolute(argv[3]),
                 argc == 5 ? fs::absolute(argv[4]) : fs::absolute(argv[2]) / L"DLL/DeltaEAC.dll");
        else if ((argc == 2 || argc == 3) && std::wstring_view(argv[1]) == L"--verify")
            Verify(argc == 3 ? fs::absolute(argv[2]) : Executable().parent_path());
        else if ((argc == 2 || argc == 3) && std::wstring_view(argv[1]) == L"--setup")
        {
            const auto package = Executable().parent_path();
            Verify(package);
            std::string error;
            if (!runtime_paths::UseDataRoot(package / L"cs2_assets", &error))
                throw std::runtime_error(error);
            std::cout << "Registered catalog and preview assets for this Windows user.\n";
            try
            {
                Install(package, argc == 3 ? fs::absolute(argv[2]) : FindGame());
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error(std::string("Catalog assets are registered, but custom game resources were not installed: ") +
                                         e.what() + "\nRun AssetTool.exe --setup with your game/csgo folder to finish setup.");
            }
        }
        else if ((argc == 2 || argc == 3) && std::wstring_view(argv[1]) == L"--install")
            Install(Executable().parent_path(), argc == 3 ? fs::absolute(argv[2]) : FindGame());
        else
            throw std::runtime_error("Usage: AssetTool --package source-folder output-folder [built-DLL] | --setup [game/csgo] | --verify [package-folder] | --install [game/csgo]");
        CoUninitialize();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
