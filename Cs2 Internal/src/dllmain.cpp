#include "platform/windows.h"
#include "ui/overlay.h"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    // Device/hook initialization runs after the loader lock is released.
    if (reason == DLL_PROCESS_ATTACH)
        return overlay::Start();
    // StopOverlay disables the overlay. Embedded Tauri pins this image for its
    // process-lifetime window callbacks; stopping does not permit unmapping it.
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI StartOverlay(LPVOID)
{
    return overlay::Start();
}
extern "C" __declspec(dllexport) BOOL WINAPI StopOverlay(LPVOID)
{
    return overlay::Stop();
}
// 0 = stopped, 1 = starting, 2 = hooks installed, 3 = stopping, 4 = failed.
extern "C" __declspec(dllexport) DWORD WINAPI GetOverlayState()
{
    return overlay::State();
}
