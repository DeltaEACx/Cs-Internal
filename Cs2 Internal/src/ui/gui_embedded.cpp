#include "platform/windows.h"
#include "ui/gui_embedded.h"
#include "ui/gui_bridge.h"
#include <cstdint>
#include <cstring>
#include <process.h>

extern "C" {
using EmbeddedRequest=int32_t(__cdecl*)(void*,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
int32_t velora_gui_run(EmbeddedRequest,void*);
void velora_gui_stop();
void velora_gui_toggle();
int32_t velora_gui_running();
size_t velora_gui_last_error(uint8_t*,size_t);
}

namespace gui_embedded {
namespace {
SRWLOCK lifecycle=SRWLOCK_INIT;
HANDLE threadHandle{};
constexpr size_t maximumMessage=8u*1024u*1024u;
struct Lock {
    Lock(){AcquireSRWLockExclusive(&lifecycle);}
    ~Lock(){ReleaseSRWLockExclusive(&lifecycle);}
};

int32_t __cdecl Request(void*,const uint8_t* input,size_t length,
                       uint8_t* output,size_t capacity,size_t* written) noexcept {
    if(!input||!output||!written||!length||length>=maximumMessage) return -1;
    *written=0;
    try {
        const auto response=gui_bridge::Request(std::string(reinterpret_cast<const char*>(input),length));
        if(response.empty()||response.size()>capacity) return -2;
        std::memcpy(output,response.data(),response.size());
        *written=response.size();
        return 0;
    } catch(...) {return -3;}
}

unsigned __stdcall Run(void*) {
    const auto result=velora_gui_run(Request,nullptr);
    if(result!=0) {
        char error[1024]{};
        velora_gui_last_error(reinterpret_cast<uint8_t*>(error),sizeof(error));
        OutputDebugStringA("[DeltaEAC] Embedded GUI stopped: ");
        OutputDebugStringA(error);
        OutputDebugStringA("\n");
    }
    return result==0?0:1;
}
}

bool Start() {
    const Lock lock;
    if(threadHandle) {
        const auto state=WaitForSingleObject(threadHandle,0);
        if(state==WAIT_TIMEOUT) return true;
        if(state!=WAIT_OBJECT_0) return false;
        CloseHandle(threadHandle);
        threadHandle=nullptr;
    }
    // Wry retains window-class callbacks and its bounded async executor for the
    // process lifetime. Retain this normally loaded image before spawning UI.
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&Start),&module)) return false;
    threadHandle=reinterpret_cast<HANDLE>(_beginthreadex(nullptr,0,Run,nullptr,0,nullptr));
    return threadHandle!=nullptr;
}

bool Stop(unsigned long timeoutMs) {
    const Lock lock;
    if(!threadHandle) return true;
    const auto deadline=GetTickCount64()+timeoutMs;
    bool requested=false;
    for(;;) {
        const auto state=WaitForSingleObject(threadHandle,0);
        if(state==WAIT_OBJECT_0) {
            CloseHandle(threadHandle);
            threadHandle=nullptr;
            return true;
        }
        if(state!=WAIT_TIMEOUT||GetTickCount64()>=deadline) return false;
        // ACTIVE is published only after Rust initializes its stop latch. This
        // also handles Stop arriving before the new thread enters Rust.
        if(!requested&&velora_gui_running()) {velora_gui_stop();requested=true;}
        WaitForSingleObject(threadHandle,10);
    }
}

void Toggle(){velora_gui_toggle();}
bool Running(){return velora_gui_running()!=0;}
bool Stopped(){const Lock lock;return !threadHandle||WaitForSingleObject(threadHandle,0)==WAIT_OBJECT_0;}
}
