#include "platform/windows.h"
#include "core/runtime_paths.h"
#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace runtime_paths
{
    namespace fs = std::filesystem;
    fs::path NativePath(fs::path path)
    {
        if (path.empty())
            return {};
        path.make_preferred();
        const auto &given = path.native();
        if (given.starts_with(L"\\\\?\\") || given.starts_with(L"\\\\.\\"))
            return path;
        path = fs::absolute(path).lexically_normal();
        path.make_preferred();
        const auto &absolute = path.native();
        if (absolute.starts_with(L"\\\\"))
            return fs::path(L"\\\\?\\UNC\\" + absolute.substr(2));
        return fs::path(L"\\\\?\\" + absolute);
    }
    namespace
    {
        std::mutex rootMutex;
        fs::path chosenRoot;
        std::atomic<unsigned long> nextTemporaryFile{};

        fs::path Environment(const wchar_t *name)
        {
            wchar_t value[32768]{};
            const DWORD count = GetEnvironmentVariableW(name, value, 32768);
            return count && count < 32768 ? fs::path(value) : fs::path{};
        }
        fs::path ModulePath(HMODULE module)
        {
            wchar_t value[32768]{};
            const DWORD count = GetModuleFileNameW(module, value, 32768);
            return count && count < 32768 ? NativePath(fs::path(value)) : fs::path{};
        }
        bool HasCatalog(const fs::path &root)
        {
            std::error_code ec;
            return !root.empty() && fs::is_regular_file(NativePath(root / L"game_assets/item_catalog.json"), ec) &&
                   fs::is_regular_file(NativePath(root / L"game_assets/image_index.json"), ec);
        }
        fs::path CanonicalRoot(const fs::path &root)
        {
            std::error_code ec;
            const auto result = fs::weakly_canonical(NativePath(root), ec);
            return ec ? fs::path{} : NativePath(result);
        }
        fs::path NormalizeRoot(const fs::path &input, bool allowExtractedParent = false)
        {
            if (input.empty())
                return {};
            std::error_code ec;
            auto root = NativePath(input);
            if (fs::is_regular_file(root, ec))
                root = root.parent_path();
            if (root.filename() == L"game_assets")
                root = root.parent_path();
            for (const auto &candidate : {root, root / L"cs2_assets", root / L"Cs2Internal/cs2_assets",
                                          root / L"Cs2 Internal/cs2_assets"})
                if (HasCatalog(candidate))
                    return CanonicalRoot(candidate);
            // Inspect only the immediate children of a selected extracted-package parent.
            if (allowExtractedParent)
            {
                fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                unsigned inspected = 0;
                while (!ec && it != end && inspected++ < 64)
                {
                    if (it->is_directory(ec))
                        for (const auto &candidate : {it->path(), it->path() / L"cs2_assets"})
                            if (HasCatalog(candidate))
                                return CanonicalRoot(candidate);
                    it.increment(ec);
                }
            }
            return {};
        }
        fs::path SearchParents(fs::path directory)
        {
            for (int level = 0; level < 6 && !directory.empty(); ++level)
            {
                if (const auto root = NormalizeRoot(directory); !root.empty())
                    return root;
                const auto parent = directory.parent_path();
                if (parent == directory)
                    break;
                directory = parent;
            }
            return {};
        }
        fs::path SavedRootFile()
        {
            const auto local = Environment(L"LOCALAPPDATA");
            return local.empty() ? fs::path{} : NativePath(local / L"CS2Internal/asset-root.txt");
        }
        fs::path ReadSavedRoot()
        {
            const auto file = SavedRootFile();
            if (file.empty())
                return {};
            struct Reader
            {
                HANDLE handle;
                ~Reader() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
            } input{CreateFileW(file.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            LARGE_INTEGER bytes{};
            if (input.handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(input.handle, &bytes) ||
                bytes.QuadPart < 0 || bytes.QuadPart > 128 * 1024)
                return {};
            std::string text(static_cast<std::size_t>(bytes.QuadPart), '\0');
            DWORD count = 0;
            if (!ReadFile(input.handle, text.data(), static_cast<DWORD>(text.size()), &count, nullptr) ||
                count != text.size())
                return {};
            if (text.starts_with("\xEF\xBB\xBF"))
                text.erase(0, 3);
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
                text.pop_back();
            try
            {
                return NormalizeRoot(fs::path(std::u8string(text.begin(), text.end())), true);
            }
            catch (const std::exception &)
            {
                return {};
            }
        }
        fs::path ResolveRoot(const fs::path &selected, const fs::path &configured, const fs::path &module,
                             const fs::path &executable, const fs::path &working, const fs::path &saved)
        {
            for (const auto &root : {selected, configured})
                if (const auto normalized = NormalizeRoot(root, true); !normalized.empty())
                    return normalized;
            for (const auto &directory : {module, executable, working})
                if (const auto root = SearchParents(directory); !root.empty())
                    return root;
            return NormalizeRoot(saved, true);
        }
        bool PersistRoot(const fs::path &root, std::string *error)
        {
            fs::path temporary;
            try
            {
                const auto file = SavedRootFile();
                if (file.empty())
                    throw std::runtime_error("LOCALAPPDATA is unavailable.");
                fs::create_directories(file.parent_path());
                temporary = file;
                temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(nextTemporaryFile.fetch_add(1));
                const auto encoded = NativePath(root).u8string();
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char *>(encoded.data()), encoded.size());
                output.put('\n');
                output.close();
                if (!output || !MoveFileExW(temporary.c_str(), file.c_str(),
                                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("The saved asset path could not be written.");
                return true;
            }
            catch (const std::exception &exception)
            {
                std::error_code ec;
                if (!temporary.empty())
                    fs::remove(temporary, ec);
                if (error)
                    *error = std::string("Assets are usable, but their folder could not be remembered: ") + exception.what();
                return false;
            }
        }
    } // namespace
    fs::path ModuleDirectory()
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module))
            return {};
        return NativePath(ModulePath(module).parent_path());
    }
    fs::path DataRoot()
    {
        fs::path selected;
        {
            std::lock_guard lock(rootMutex);
            selected = chosenRoot;
        }
        std::error_code ec;
        const auto working = fs::current_path(ec);
        const auto root = ResolveRoot(selected, Environment(L"CS2_ASSET_ROOT"), ModuleDirectory(),
                                      ModulePath(nullptr).parent_path(), ec ? fs::path{} : working, {});
        if (!root.empty())
            return root;
        if (const auto saved = ReadSavedRoot(); !saved.empty())
            return saved;
        throw std::runtime_error("Assets missing. Extract the complete package with cs2_assets beside the DLL, or choose its assets folder.");
    }
    bool UseDataRoot(const fs::path &root, std::string *error)
    {
        if (error)
            error->clear();
        try
        {
            const auto normalized = NormalizeRoot(root, true);
            if (normalized.empty())
            {
                if (error)
                    *error = "Select a folder containing cs2_assets with item_catalog.json and image_index.json.";
                return false;
            }
            std::lock_guard lock(rootMutex);
            if (!PersistRoot(normalized, error))
                return false;
            chosenRoot = normalized;
            return true;
        }
        catch (const std::exception &exception)
        {
            if (error)
                *error = exception.what();
            return false;
        }
    }
    fs::path GameDirectory()
    {
        if (const HMODULE client = GetModuleHandleW(L"client.dll"))
        {
            const auto file = ModulePath(client);
            if (!file.empty())
                return NativePath(file.parent_path().parent_path().parent_path());
        }
        const auto configured = Environment(L"CS2_GAME_DIR");
        std::error_code ec;
        return !configured.empty() && fs::is_regular_file(NativePath(configured / L"gameinfo.gi"), ec)
                   ? NativePath(configured) : fs::path{};
    }
} // namespace runtime_paths
