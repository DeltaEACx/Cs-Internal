// Standalone probe DLL only. Production injection and hooks are deliberately absent.
// Rust's embedded entry points are exported by gui_embedded_probe.def.
#include <Windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
