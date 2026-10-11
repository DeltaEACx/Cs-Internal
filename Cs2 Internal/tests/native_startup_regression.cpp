#include "core/module_digest.hpp"
#include "core/pattern_match.hpp"
#include <barrier>
#include <atomic>
#include <climits>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" __declspec(thread) int _Init_thread_epoch;
bool PeerModuleMatch(std::uintptr_t module,std::string_view digest);
namespace {
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using Clock=std::chrono::steady_clock;
double Milliseconds(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
std::vector<std::size_t> Scalar(std::span<const std::uint8_t> bytes,std::span<const int> pattern) {
    std::vector<std::size_t> result;
    const auto anchor=std::find_if(pattern.begin(),pattern.end(),[](int value){return value>=0;});
    if(anchor==pattern.end()||bytes.size()<pattern.size())return result;
    const auto offset=static_cast<std::size_t>(anchor-pattern.begin());
    for(std::size_t i{};i<=bytes.size()-pattern.size();++i) {
        if(bytes[i+offset]!=pattern[offset])continue;
        bool match=true;
        for(std::size_t j{};j<pattern.size();++j)if(pattern[j]>=0&&bytes[i+j]!=pattern[j]){match=false;break;}
        if(match)result.push_back(i);
    }
    return result;
}
std::vector<std::size_t> Fast(std::span<const std::uint8_t> bytes,std::span<const int> pattern) {
    std::vector<std::size_t> result;
    native_startup::PatternMatcher(pattern).Scan(bytes,[&](std::size_t offset){result.push_back(offset);return true;});
    return result;
}
void MatcherChecks() {
    std::uint32_t random=0x573A1975;
    const auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(int trial=0;trial<500;++trial) {
        std::vector<std::uint8_t> bytes(137);for(auto& value:bytes)value=static_cast<std::uint8_t>(next());
        const auto length=1+next()%40;
        const auto start=next()%(bytes.size()-length+1);
        std::vector<int> pattern;for(unsigned i{};i<length;++i)pattern.push_back(next()%3?bytes[start+i]:-1);
        pattern[next()%length]=bytes[start+next()%length];
        Check(Scalar(bytes,pattern)==Fast(bytes,pattern),"BMH wildcard matches differ from the prior scalar matcher");
    }
    const std::vector<std::uint8_t> repeated(10,0xaa);
    const std::vector<int> overlap{0xaa,0xaa,-1,0xaa};
    Check(Fast(repeated,overlap)==Scalar(repeated,overlap)&&Fast(repeated,overlap).size()==7,"Overlapping matches were lost");
    unsigned matches{};
    Check(!native_startup::PatternMatcher(overlap).Scan(repeated,[&](std::size_t){return ++matches<2;})&&matches==2,
          "Ambiguity refusal did not observe the second overlapping match");
    Check(Fast({},overlap).empty()&&Fast(repeated,std::vector<int>(20,0xaa)).empty(),"Short section crossed its bounds");
    Check(Fast(repeated,std::vector<int>{-1,-1}).empty(),"All-wildcard pattern did not fail closed");
    std::vector<std::uint8_t> image(32u*1024u*1024u);
    for(std::size_t i{};i<image.size();++i)image[i]=i%4==0?0x48:0x90;
    std::vector<std::vector<int>> patterns;
    for(unsigned marker{};marker<8;++marker) {
        std::vector<int> pattern(40);pattern[0]=0x48;
        for(unsigned i=1;i<5;++i)pattern[i]=-1;
        for(unsigned i=5;i<pattern.size();++i)pattern[i]=static_cast<std::uint8_t>(next());
        const auto at=1000+marker*3000000;
        for(unsigned i{};i<pattern.size();++i)image[at+i]=pattern[i]<0?0x27:static_cast<std::uint8_t>(pattern[i]);
        patterns.push_back(pattern);
    }
    auto begin=Clock::now();std::vector<std::vector<std::size_t>> expected;
    for(const auto& pattern:patterns)expected.push_back(Scalar(image,pattern));
    const auto scalarMs=Milliseconds(begin);begin=Clock::now();
    for(std::size_t i{};i<patterns.size();++i)Check(Fast(image,patterns[i])==expected[i]&&expected[i].size()==1,"Benchmark matcher changed its unique results");
    std::cout<<"Matcher: 8 signatures over 32 MiB, scalar="<<scalarMs<<" ms, exact-run BMH="<<Milliseconds(begin)<<" ms\n";
}
struct Files {
    std::filesystem::path path,replacement,backup;
    Files(){
        std::array<wchar_t,32768> temp{};Check(GetTempPathW(static_cast<DWORD>(temp.size()),temp.data())>0,"Read TEMP path");
        path=std::filesystem::path(temp.data())/(L"deltaeac-native-startup-"+std::to_wstring(GetCurrentProcessId())+L".bin");
        replacement=path.wstring()+L".replacement";backup=path.wstring()+L".backup";
    }
    ~Files(){std::error_code error;for(const auto& item:{path,replacement,backup})std::filesystem::remove(item,error);}
    void Write(const std::filesystem::path& file,std::string_view data){std::ofstream stream(file,std::ios::binary|std::ios::trunc);stream.write(data.data(),data.size());Check(bool(stream),"Write hash fixture");}
};
void DigestChecks() {
    constexpr auto abc="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    Files files;native_startup::DigestCache cache;
    Check(!cache.Read(1,files.path.wstring()),"Missing file was unexpectedly verified");
    files.Write(files.path,"abc");
    Check(cache.Read(1,files.path.wstring())==abc,"SHA-256 known vector or retry after missing file failed");
    for(int repeat=0;repeat<16;++repeat)Check(cache.Read(1,files.path.wstring())==abc,"Stable file digest changed");
    Check(cache.Stats().fullReads==1&&cache.Stats().cacheHits==16,"Repeated validation reread the full module file");
    HANDLE metadata=CreateFileW(files.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    FILETIME creation{},access{},write{};Check(metadata!=INVALID_HANDLE_VALUE&&GetFileTime(metadata,&creation,&access,&write),"Read fixture write time");CloseHandle(metadata);
    Sleep(2);files.Write(files.path,"abd");
    metadata=CreateFileW(files.path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(metadata!=INVALID_HANDLE_VALUE&&SetFileTime(metadata,nullptr,nullptr,&write),"Restore same-size fixture write time");CloseHandle(metadata);
    const auto changed=cache.Read(1,files.path.wstring());
    Check(changed&&*changed!=abc&&cache.Stats().fullReads==2,"Same-size change with restored write time reused a stale digest");
    Check(cache.Read(2,files.path.wstring())==changed&&cache.Stats().fullReads==3,"Changed loaded-module identity reused an old entry");
    files.Write(files.replacement,"abc");
    std::filesystem::rename(files.path,files.backup);std::filesystem::rename(files.replacement,files.path);
    Check(cache.Read(1,files.path.wstring())==abc&&cache.Stats().fullReads==4,"Replaced file identity retained a previous digest");
    HANDLE writer=CreateFileW(files.path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(writer!=INVALID_HANDLE_VALUE,"Open concurrent writer fixture");
    const bool unavailable=!cache.Read(1,files.path.wstring());CloseHandle(writer);
    Check(unavailable&&cache.Read(1,files.path.wstring())==abc,"Concurrent writer was accepted or its failure became permanent");
    native_startup::DigestCache concurrent;std::barrier gate(8);std::atomic<unsigned> failures{};std::vector<std::thread> workers;
    for(unsigned i{};i<8;++i)workers.emplace_back([&]{gate.arrive_and_wait();if(concurrent.Read(3,files.path.wstring())!=abc)++failures;});
    for(auto& worker:workers)worker.join();
    Check(!failures&&concurrent.Stats().fullReads==1,"Concurrent validators duplicated the first full-file read");
    std::string payload(32u*1024u*1024u,'K');files.Write(files.path,payload);
    auto begin=Clock::now();std::optional<std::string> digest;
    for(int repeat=0;repeat<9;++repeat){native_startup::DigestCache uncached;digest=uncached.Read(4,files.path.wstring());Check(bool(digest),"Baseline full hash failed");}
    const auto uncachedMs=Milliseconds(begin);native_startup::DigestCache measured;begin=Clock::now();
    for(int repeat=0;repeat<9;++repeat)Check(measured.Read(4,files.path.wstring())==digest,"Cached benchmark digest changed");
    const auto stats=measured.Stats();Check(stats.fullReads==1&&stats.bytesRead==payload.size()&&stats.cacheHits==8,"Hash benchmark failed to reduce physical full reads");
    std::cout<<"Hash: 9 validations of 32 MiB, full reads 9 -> "<<stats.fullReads<<", uncached="<<uncachedMs<<" ms, cached="<<Milliseconds(begin)<<" ms\n";
}
void SharedModuleChecks() {
    const auto module=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::array<wchar_t,32768> path{};Check(GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()))>0,"Read current module path");
    native_startup::DigestCache reference;const auto digest=reference.Read(module,path.data());Check(bool(digest),"Hash actual loaded test module");
    std::barrier gate(8);std::atomic<unsigned> failures{};std::vector<std::thread> workers;
    for(unsigned i{};i<8;++i)workers.emplace_back([&,i]{
        gate.arrive_and_wait();const int epoch=_Init_thread_epoch;_Init_thread_epoch=INT_MAX;
        const bool matched=i%2?PeerModuleMatch(module,*digest):native_startup::MatchesModule(module,*digest);
        _Init_thread_epoch=epoch;if(!matched)++failures;
    });
    for(auto& worker:workers)worker.join();
    const auto stats=native_startup::SharedDigestCache()->Stats();
    Check(!failures&&stats.fullReads==1&&stats.cacheHits==7,"ODR-shared cache or concurrent foreign-CRT first initialization failed");
    Check(!PeerModuleMatch(module,std::string(64,'0'))&&!native_startup::MatchesModule(1,*digest),"Expected digest mismatch or invalid loaded-module identity was accepted");
}
}
int main(){try{MatcherChecks();DigestChecks();SharedModuleChecks();std::cout<<"Native startup regression: PASS\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
