#pragma once
#include "platform/windows.h"
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace native_startup {
struct FileIdentity {
    std::uint32_t volume{},indexHigh{},indexLow{};
    std::uint64_t size{},creation{},write{},change{};
    bool operator==(const FileIdentity&) const = default;
};
inline std::optional<FileIdentity> Identity(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION information{};FILE_BASIC_INFO basic{};
    if(!GetFileInformationByHandle(file,&information)||
        !GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic)))return {};
    const auto time=[](FILETIME value){return (std::uint64_t{value.dwHighDateTime}<<32)|value.dwLowDateTime;};
    return FileIdentity{information.dwVolumeSerialNumber,information.nFileIndexHigh,information.nFileIndexLow,
        (std::uint64_t{information.nFileSizeHigh}<<32)|information.nFileSizeLow,
        time(information.ftCreationTime),time(information.ftLastWriteTime),static_cast<std::uint64_t>(basic.ChangeTime.QuadPart)};
}
class DigestCache {
    struct Entry { std::uintptr_t module{};std::wstring path;FileIdentity identity;std::string digest; };
    SRWLOCK mutex=SRWLOCK_INIT;
    std::vector<Entry> entries;
    std::uint64_t fullReads{},bytesRead{},cacheHits{};
    struct Guard {
        SRWLOCK& lock;
        explicit Guard(SRWLOCK& value):lock(value){AcquireSRWLockExclusive(&lock);}
        ~Guard(){ReleaseSRWLockExclusive(&lock);}
    };
public:
    struct Statistics { std::uint64_t fullReads{},bytesRead{},cacheHits{}; };
    Statistics Stats(){const Guard guard(mutex);return {fullReads,bytesRead,cacheHits};}
    std::optional<std::string> Read(std::uintptr_t module,const std::wstring& path) {
        // Sharing reads/deletion permits normal loaded-image inspection and
        // replacement detection, while excluding a concurrent writer.
        struct File {
            HANDLE value{};
            ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
        } file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
        if(file.value==INVALID_HANDLE_VALUE)return {};
        const auto before=Identity(file.value);
        if(!before)return {};
        const Guard guard(mutex);
        for(const auto& entry:entries)if(entry.module==module&&entry.path==path&&entry.identity==*before) {
            const auto after=Identity(file.value);
            if(!after||*after!=*before)return {};
            ++cacheHits;return entry.digest;
        }
        struct Hash {
            BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE value{};
            ~Hash(){if(value)BCryptDestroyHash(value);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
        } hash;
        if(BCryptOpenAlgorithmProvider(&hash.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
            BCryptCreateHash(hash.algorithm,&hash.value,nullptr,0,nullptr,0,0)<0)return {};
        ++fullReads;
        std::array<unsigned char,65536> buffer{};
        std::uint64_t readBytes{};
        for(;;) {
            DWORD count{};
            if(!ReadFile(file.value,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr))return {};
            if(!count)break;
            readBytes+=count;bytesRead+=count;
            if(BCryptHashData(hash.value,buffer.data(),count,0)<0)return {};
        }
        std::array<unsigned char,32> bytes{};
        if(BCryptFinishHash(hash.value,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0)return {};
        const auto after=Identity(file.value);
        if(!after||*after!=*before||readBytes!=before->size)return {};
        constexpr char digits[]="0123456789abcdef";
        std::string digest;digest.reserve(64);
        for(const auto value:bytes){digest.push_back(digits[value>>4]);digest.push_back(digits[value&15]);}
        std::erase_if(entries,[&](const Entry& entry){return entry.module==module&&entry.path==path;});
        if(entries.size()>=64)entries.erase(entries.begin());
        entries.push_back({module,path,*before,digest});return digest;
    }
};
// Inline ODR-shared storage keeps all feature translation units on one cache.
// Explicit Win32 initialization avoids foreign CRT/TLS local-static guards.
inline constinit INIT_ONCE digestCacheOnce=INIT_ONCE_STATIC_INIT;
inline BOOL CALLBACK InitializeDigestCache(PINIT_ONCE,PVOID,PVOID* context) noexcept {
    try {*context=new DigestCache;return TRUE;}catch(...){return FALSE;}
}
inline DigestCache* SharedDigestCache() {
    PVOID context{};
    return InitOnceExecuteOnce(&digestCacheOnce,InitializeDigestCache,nullptr,&context)?
        static_cast<DigestCache*>(context):nullptr;
}
inline bool MatchesModule(std::uintptr_t module,std::string_view expected) {
    if(!module||expected.size()!=64||!std::ranges::all_of(expected,[](char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return false;
    struct Module {
        HMODULE value{};
        ~Module(){if(value)FreeLibrary(value);}
    } retained;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(module),&retained.value)||
        reinterpret_cast<std::uintptr_t>(retained.value)!=module)return false;
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(retained.value,path.data(),static_cast<DWORD>(path.size()));
    if(!length||length>=path.size())return false;
    try {
        std::wstring nativePath(path.data(),length);
        if(!nativePath.starts_with(L"\\\\?\\"))nativePath=nativePath.starts_with(L"\\\\")?
            L"\\\\?\\UNC\\"+nativePath.substr(2):L"\\\\?\\"+nativePath;
        const auto cache=SharedDigestCache();
        if(!cache)return false;
        const auto digest=cache->Read(module,nativePath);
        return digest&&*digest==expected;
    } catch(...) {return false;}
}
}
