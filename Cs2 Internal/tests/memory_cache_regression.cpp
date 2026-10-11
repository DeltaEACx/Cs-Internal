// Runs only in its own test EXE. No game, mapper, hooks, or UI runtime is used.
// Compile/link the production engine.cpp so these checks exercise the real cache.
#include "features/visuals/engine.hpp"
#include "nlohmann/json.hpp"
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

extern "C" __declspec(thread) int _Init_thread_epoch;

namespace {
constexpr std::size_t MarkerSize=48,MarkerCount=16,ThreadCount=8;
// Written at runtime, so the expected pattern has no duplicate constant in .rdata.
alignas(64) volatile unsigned char markers[MarkerCount][MarkerSize]{};
using Json=nlohmann::json;
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::array<unsigned char,MarkerSize> Bytes(unsigned seed){
    std::array<unsigned char,MarkerSize> result{};
    std::uint32_t value=0x75D63AF1u^seed;
    for(auto& byte:result){value^=value<<13;value^=value>>17;value^=value<<5;byte=static_cast<unsigned char>(value>>11);}
    return result;
}
void Install(std::size_t marker,const std::array<unsigned char,MarkerSize>& bytes){
    for(std::size_t i=0;i<MarkerSize;++i)markers[marker][i]=bytes[i];
}
std::uintptr_t Address(std::size_t marker){return reinterpret_cast<std::uintptr_t>(&markers[marker][0]);}
std::string Pattern(const std::string& module,const std::array<unsigned char,MarkerSize>& bytes){
    constexpr char digits[]="0123456789ABCDEF";
    auto result=module+":";
    for(auto byte:bytes){result+=digits[byte>>4];result+=digits[byte&15];result+=' ';}
    return result;
}
// Deliberately corrupt only this process's test-thread guard epoch. Restore it
// before assertions, reporting or thread teardown. No loader TLS state is altered.
std::uintptr_t ResolveWithForeignEpoch(std::string_view expression){
    struct Restore {int original=_Init_thread_epoch;~Restore(){_Init_thread_epoch=original;}} restore;
    _Init_thread_epoch=std::numeric_limits<int>::max();
    return memory::resolve_cached(expression);
}
Json Run(bool concurrentFirst,const std::string& module){
    Json report={{"implementation","production memory::resolve_cached / engine.cpp"},
                 {"mode",concurrentFirst?"concurrent-first-use":"foreign-epoch-first-use"},
                 {"foreignEpoch",std::numeric_limits<int>::max()},{"checks",Json::array()}};
    auto& checks=report["checks"];
    std::array<std::string,MarkerCount> patterns;
    for(std::size_t i=0;i<MarkerCount;++i){const auto bytes=Bytes(static_cast<unsigned>(i+1));patterns[i]=Pattern(module,bytes);if(i!=1)Install(i,bytes);}
    // Ensure the fixture address is genuine PE data and uniquely discoverable.
    Require(memory::resolve_pattern(patterns[0])==Address(0),"Fixture pattern must resolve to its exact PE data address");
    if(concurrentFirst){
        std::barrier gate(static_cast<std::ptrdiff_t>(ThreadCount));
        std::array<std::uintptr_t,ThreadCount> results{};std::vector<std::thread> workers;
        for(std::size_t i=0;i<ThreadCount;++i)workers.emplace_back([&,i]{gate.arrive_and_wait();results[i]=ResolveWithForeignEpoch(patterns[0]);});
        for(auto& worker:workers)worker.join();
        for(auto result:results)Require(result==Address(0),"Concurrent cache first use with foreign epochs failed");
        checks.push_back("Eight simultaneous first calls initialize one usable cache despite foreign CRT epochs");
    }else{
        const auto epoch=_Init_thread_epoch;
        Require(ResolveWithForeignEpoch(patterns[0])==Address(0),"Cache first use under foreign CRT epoch failed");
        Require(_Init_thread_epoch==epoch,"Test thread CRT epoch was not restored");
        checks.push_back("First cache call succeeds with test-thread CRT epoch INT_MAX and restores the epoch");
    }
    // A successful entry is cached even if its scan bytes later change.
    markers[0][0]^=0x5Au;
    Require(memory::resolve_pattern(patterns[0])==0,"Mutated fixture should no longer match a fresh scan");
    for(int i=0;i<64;++i)Require(memory::resolve_cached(patterns[0])==Address(0),"Successful cache entry was not retained");
    checks.push_back("Successful resolution remains cached across 64 calls after source bytes change");

    Require(memory::resolve_cached(patterns[1])==0,"Absent marker unexpectedly resolved");
    Require(memory::resolve_cached(patterns[1])==0,"Repeated absent marker unexpectedly resolved");
    Install(1,Bytes(2));
    Require(memory::resolve_cached(patterns[1])==Address(1),"An early miss prevented later successful resolution");
    checks.push_back("Two misses remain retryable when the marker becomes available later");

    std::atomic<unsigned> failures{};std::barrier gate(static_cast<std::ptrdiff_t>(ThreadCount));
    std::vector<std::thread> workers;
    for(std::size_t i=0;i<ThreadCount;++i)workers.emplace_back([&,i]{
        gate.arrive_and_wait();
        for(int pass=0;pass<32;++pass){
            if(ResolveWithForeignEpoch(patterns[i+2])!=Address(i+2))++failures;
            if(memory::resolve_cached(patterns[0])!=Address(0))++failures;
            if(memory::resolve_cached(patterns[1])!=Address(1))++failures;
        }
    });
    for(auto& worker:workers)worker.join();
    Require(failures==0,"Concurrent cache lookups or insertions returned an incorrect address");
    checks.push_back("Eight threads complete 768 mixed cache reads and distinct-key resolutions");
    Require(memory::resolve_cached("invalid expression")==0,"Invalid expression should fail closed");
    Require(memory::resolve_cached("velora_cache_absent_module.dll:13 27 39 4D")==0,"Absent module should fail closed");
    checks.push_back("Invalid expression and absent module return zero");
    const auto duplicated=Bytes(85);Install(14,duplicated);Install(15,duplicated);
    const auto ambiguous=Pattern(module,duplicated);
    Require(memory::resolve_pattern(ambiguous)==0,"Duplicate signature was incorrectly accepted");
    Install(15,Bytes(86));
    Require(memory::resolve_pattern(ambiguous)==Address(14),"Removing a duplicate failed to restore the exact unique match");
    auto transformed=Pattern(module,duplicated);transformed+="+10";
    Require(memory::resolve_pattern(transformed)==Address(14)+0x10,"Hexadecimal match adjustment changed");
    const auto dereference=Pattern(module,duplicated)+"^?? ?? ?? ?? ?? ?? ?? ??";
    std::uintptr_t first=Address(2),second=Address(3);
    std::memcpy(const_cast<unsigned char*>(&markers[15][0]),&first,sizeof(first));
    Require(memory::resolve_pattern(dereference)==first,"Pointer-operand transform did not read the live value");
    // A separate unique marker followed by a live pointer exercises ^ and ~.
    const auto pointerPattern=Pattern(module,duplicated)+"+30~";
    Require(memory::resolve_pattern(pointerPattern)==first,"Dereference did not read the live target pointer");
    std::memcpy(const_cast<unsigned char*>(&markers[15][0]),&second,sizeof(second));
    Require(memory::resolve_pattern(pointerPattern)==second,"Fresh pattern resolution retained an obsolete dereferenced object");
    Require(memory::resolve_pattern(dereference)==second,"Pointer-operand transform retained an obsolete object");
    checks.push_back("BMH preserves duplicate refusal, exact adjustments, and live transformed dereferences");
    report["ok"]=true;return report;
}
}

int main(int argc,char** argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    const bool concurrent=argc>1&&std::string_view(argv[1])=="--concurrent-first";
    try{
        char path[32768]{};Require(GetModuleFileNameA(nullptr,path,static_cast<DWORD>(std::size(path)))>0,"Read fixture module path");
        const auto module=std::filesystem::path(path).filename().string();
        const auto report=Run(concurrent,module);std::cout<<report.dump(2)<<std::endl;return 0;
    }catch(const std::exception& error){std::cerr<<Json{{"ok",false},{"error",error.what()}}.dump(2)<<std::endl;return 1;}
}
