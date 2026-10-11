// Discovery fixtures exercise DLL, executable, working and remembered folders.
#include "core/runtime_paths.cpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
    void Check(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
    struct EnvironmentGuard
    {
        const wchar_t *name;
        std::filesystem::path previous;
        explicit EnvironmentGuard(const wchar_t *key) : name(key), previous(runtime_paths::Environment(key)) {}
        ~EnvironmentGuard() { SetEnvironmentVariableW(name, previous.empty() ? nullptr : previous.c_str()); }
    };
    void Catalog(const std::filesystem::path &root)
    {
        std::filesystem::create_directories(runtime_paths::NativePath(root / L"game_assets"));
        std::ofstream(runtime_paths::NativePath(root / L"game_assets/item_catalog.json")) << "{}";
        std::ofstream(runtime_paths::NativePath(root / L"game_assets/image_index.json")) << "{}";
    }
    void MeasureRootLookup(const char *label)
    {
        constexpr int lookups = 500;
        const auto first = runtime_paths::DataRoot();
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < lookups; ++index)
            Check(runtime_paths::DataRoot() == first, "Repeated asset lookup changed without a root update");
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "Asset root " << label << ": " << lookups << " lookups in " << elapsed << " ms\n";
    }
    void Cleanup(const std::filesystem::path &fixture)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto temporary = fs::weakly_canonical(runtime_paths::NativePath(fs::temp_directory_path()), ec);
        const auto target = fs::weakly_canonical(runtime_paths::NativePath(fixture), ec);
        const auto relative = target.lexically_relative(temporary);
        if (!ec && target.is_absolute() && relative.filename() ==
                    (L"CS2-path-tests-" + std::to_wstring(GetCurrentProcessId())) &&
            relative.parent_path().empty())
            fs::remove_all(target, ec);
    }
}

int main()
{
    namespace fs = std::filesystem;
    using namespace runtime_paths;
    const auto fixture = NativePath(fs::weakly_canonical(fs::temp_directory_path()) /
                                   (L"CS2-path-tests-" + std::to_wstring(GetCurrentProcessId())));
    EnvironmentGuard local(L"LOCALAPPDATA"), configured(L"CS2_ASSET_ROOT");
    try
    {
        // This temporary user's folder is the only persistence target throughout the test.
        Check(SetEnvironmentVariableW(L"LOCALAPPDATA", NativePath(fixture / L"test-user/Local Data").c_str()), "Cannot isolate LocalAppData");
        SetEnvironmentVariableW(L"CS2_ASSET_ROOT", nullptr);
        Check(NativePath({}).empty(), "Empty native path changed meaning");
        Check(NativePath(L"C:/Folder With Spaces/sub/../file.png").native() == L"\\\\?\\C:\\Folder With Spaces\\file.png",
              "Drive native path normalization failed");
        Check(NativePath(L"\\\\server\\share\\folder/file.png").native() == L"\\\\?\\UNC\\server\\share\\folder\\file.png",
              "UNC native path normalization failed");
        Check(NativePath(L"\\\\?\\UNC\\server\\share\\folder/file.png").native() == L"\\\\?\\UNC\\server\\share\\folder\\file.png",
              "Already extended UNC path was prefixed twice");
        Check(NativePath(L"\\\\?\\C:\\folder/file.png").native() == L"\\\\?\\C:\\folder\\file.png",
              "Already extended drive path retained forward slashes");
        const auto unicode = NativePath(fixture / L"\u041f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044c \u65e5\u672c\u8a9e/package/cs2_assets");
        const auto other = NativePath(fixture / L"different-user/package/cs2_assets");
        const auto executable = NativePath(fixture / L"standalone executable/cs2_assets");
        const auto working = NativePath(fixture / L"other working directory/cs2_assets");
        for (const auto &root : {unicode, other, executable, working})
            Catalog(root);
        MeasureRootLookup("bundled discovery");
        Check(NormalizeRoot(unicode) == unicode, "Unicode assets folder failed");
        Check(NormalizeRoot(unicode / L"game_assets") == unicode, "game_assets normalization failed");
        Check(NormalizeRoot(unicode.parent_path()) == unicode, "Package normalization failed");
        Check(NormalizeRoot(unicode.parent_path().parent_path(), true) == unicode, "Extracted parent normalization failed");
        const auto nestedModule = unicode.parent_path() / L"bin/x64/Release";
        Check(ResolveRoot({}, {}, nestedModule, {}, {}, {}) == unicode, "DLL ancestor discovery failed");
        Check(ResolveRoot({}, fixture / L"missing override", nestedModule, {}, {}, {}) == unicode, "Stale environment blocked bundled assets");
        Check(ResolveRoot({}, other, nestedModule, {}, {}, {}) == other, "Valid explicit environment ignored");
        Check(ResolveRoot({}, {}, {}, executable.parent_path(), {}, {}) == executable, "Executable discovery failed");
        Check(ResolveRoot({}, {}, {}, {}, working.parent_path(), {}) == working, "Working directory discovery failed");
        Check(ResolveRoot({}, {}, {}, {}, {}, other) == other, "Remembered root discovery failed");
        Check(ResolveRoot({}, {}, fixture / L"absent", {}, {}, {}).empty(), "Missing folder was accepted");
        fs::remove(NativePath(other / L"game_assets/image_index.json"));
        Check(NormalizeRoot(other).empty(), "Incomplete catalog was accepted");
        Catalog(other);
        std::string error;
        Check(UseDataRoot(unicode.parent_path(), &error) && error.empty(), "Cannot remember selected Unicode package");
        Check(ReadSavedRoot() == unicode, "Remembered UTF-8 path did not round-trip");
        SetEnvironmentVariableW(L"CS2_ASSET_ROOT", other.c_str());
        Check(DataRoot() == unicode, "Selected folder did not immediately override valid environment");
        MeasureRootLookup("selected folder");
        Check(!UseDataRoot(fixture / L"missing", &error) && !error.empty(), "Invalid chosen folder was accepted");
        Check(DataRoot() == unicode && ReadSavedRoot() == unicode, "Invalid selection replaced valid remembered folder");
        std::atomic<bool> failed{};
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i)
            readers.emplace_back([&] {
                try
                {
                    for (int j = 0; j < 40; ++j)
                    {
                        const auto root = DataRoot();
                        if (root != unicode && root != other)
                            failed = true;
                    }
                }
                catch (...) { failed = true; }
            });
        const bool changed = UseDataRoot(other, &error);
        for (auto &reader : readers)
            reader.join();
        Check(changed && !failed && DataRoot() == other && ReadSavedRoot() == other, "Concurrent discovery used an invalid root");
        auto longRoot = fixture / L"long Unicode \u041f\u0435\u0440\u0435\u043d\u043e\u0441";
        for (int i = 0; i < 9; ++i)
            longRoot /= L"folder with spaces and enough length";
        longRoot = NativePath(longRoot / L"cs2_assets");
        Catalog(longRoot);
        const auto png = NativePath(longRoot / L"game_assets/png/panorama/images/econ/default_generated/weapon_test_skin_light_png.png");
        Check(png.native().size() > 260, "Long path fixture was too short");
        fs::create_directories(png.parent_path());
        constexpr char pngHeader[]{char(0x89), 'P', 'N', 'G', '\r', '\n', char(0x1a), '\n'};
        std::ofstream image(png, std::ios::binary);
        image.write(pngHeader, sizeof(pngHeader));
        image.close();
        Check(image.good(), "Cannot write a real long-path mock PNG");
        const fs::path ordinaryLongRoot(longRoot.native().substr(4));
        Check(UseDataRoot(ordinaryLongRoot, &error), "Cannot remember long Unicode assets folder without an extended prefix");
        Check(DataRoot() == longRoot && ReadSavedRoot() == longRoot, "Long persisted native path did not round-trip");
        const auto readPath = NativePath(DataRoot() / L"game_assets/png/panorama/images/econ/default_generated/weapon_test_skin_light_png.png");
        std::ifstream readImage(readPath, std::ios::binary);
        char readHeader[sizeof(pngHeader)]{};
        readImage.read(readHeader, sizeof(readHeader));
        Check(readImage.good() && std::equal(std::begin(pngHeader), std::end(pngHeader), std::begin(readHeader)),
              "Cannot read an actual long-path mock PNG through the chosen root");
        readImage.close();
        Check(UseDataRoot(other, &error), "Cannot restore selected root after long path test");
        const auto unusableLocal = fixture / L"not-a-directory";
        std::ofstream(unusableLocal) << "fixture";
        SetEnvironmentVariableW(L"LOCALAPPDATA", unusableLocal.c_str());
        Check(!UseDataRoot(unicode, &error) && error.find("could not be remembered") != std::string::npos,
              "Persistence failure did not return a clear error");
        Check(DataRoot() == other, "Persistence failure changed the selected root");
        Cleanup(fixture);
        std::cout << "PASS: Unicode/spaces, actual long-path file reads, UNC normalization, stale overrides, DLL/executable/CWD lookup, saved UTF-8 folder, missing assets, atomic persistence and concurrent reads\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        Cleanup(fixture);
        return 1;
    }
}
