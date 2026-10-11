#include "platform/windows.h"
#include "ui/overlay.h"
#include "ui/menu_shell.h"
#include "ui/catalog_ui.h"
#include "ui/gui_bridge.h"
#include "ui/gui_embedded.h"
#include "core/runtime_paths.h"
#include "game/econ_schema.h"

#include "features/cosmetics/skin_changer.h"
#include "game/game_access.h"
#include "ui/agent_preview.h"
#include "features/view/view_settings.h"
#include "features/visuals/visuals.h"
#include "features/visuals/gun_images.h"
#include "features/combat/hvh.h"
#include "features/combat/backtrack.h"
#include "features/movement/movement.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"
#include <algorithm>
#include <d3d11.h>
#include <dxgi.h>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
    using ResizeFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    SRWLOCK g_renderLock = SRWLOCK_INIT;
    SRWLOCK g_lifecycleLock = SRWLOCK_INIT;
    volatile LONG g_state = 0, g_stop = 0, g_callbacks = 0;
    HANDLE g_worker = nullptr;
    DWORD g_workerId = 0;
    PresentFn g_present = nullptr;
    ResizeFn g_resize = nullptr;
    void *g_presentTarget = nullptr;
    void *g_resizeTarget = nullptr;
    bool g_minHookReady = false, g_presentCreated = false, g_resizeCreated = false;
    IDXGISwapChain *g_swapChain = nullptr; // Identity only, no ownership.
    ID3D11Device *g_device = nullptr;
    ID3D11DeviceContext *g_deviceContext = nullptr;
    ID3D11RenderTargetView *g_renderTarget = nullptr;
    HWND g_window = nullptr;
    PVOID volatile g_windowProc = nullptr;
    bool g_subclassInstalled = false;
    UINT g_bufferWidth = 0, g_bufferHeight = 0;
    ImGuiContext *g_imgui = nullptr;
    bool g_win32Ready = false, g_dx11Ready = false, g_resizing = false;
    bool g_menuOpen = false, g_insertDown = false, g_endDown = false;
    bool g_agentPreviewOpen = false;
    int g_menuPage{};
    econ_schema::Snapshot g_econSnapshot{};
    ULONGLONG g_nextSchemaRead = 0;

    struct CallbackScope
    {
        CallbackScope()
        {
            InterlockedIncrement(&g_callbacks);
        }
        ~CallbackScope()
        {
            InterlockedDecrement(&g_callbacks);
        }
    };
    struct RenderScope
    {
        RenderScope()
        {
            AcquireSRWLockExclusive(&g_renderLock);
        }
        ~RenderScope()
        {
            ReleaseSRWLockExclusive(&g_renderLock);
        }
    };
    struct ContextScope
    {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        explicit ContextScope(ImGuiContext *context)
        {
            ImGui::SetCurrentContext(context);
        }
        ~ContextScope()
        {
            ImGui::SetCurrentContext(previous);
        }
    };
    template <typename T> void Release(T *&object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    }
    void Log(const char *message)
    {
        OutputDebugStringA("[CS2 overlay] ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
    bool IsStopping()
    {
        return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
    }
    WNDPROC OriginalWindowProc()
    {
        return reinterpret_cast<WNDPROC>(InterlockedCompareExchangePointer(&g_windowProc, nullptr, nullptr));
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        CallbackScope callback;
        const WNDPROC original = OriginalWindowProc();
        bool consume = false;
        // NewFrame can send window messages recursively. Never make the message
        // pump wait for the render thread, or recursively acquire this lock.
        if (!IsStopping() && TryAcquireSRWLockExclusive(&g_renderLock))
        {
            if (g_imgui && window == g_window)
            {
                ContextScope context(g_imgui);
                ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
                ImGuiIO &io = ImGui::GetIO();
                if (message == WM_KILLFOCUS)
                {
                    for (bool &key : io.KeysDown)
                        key = false;
                    for (bool &button : io.MouseDown)
                        button = false;
                }
                const bool mouse = (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT ||
                                   message == WM_SETCURSOR;
                const bool keyboard = message >= WM_KEYFIRST && message <= WM_KEYLAST;
                consume = gui_bridge::MenuVisible() && (mouse || keyboard);
            }
            ReleaseSRWLockExclusive(&g_renderLock);
        }
        if (consume)
        {
            // Foreground raw input requires DefWindowProc for cleanup.
            if (message == WM_INPUT)
                return DefWindowProcW(window, message, wParam, lParam);
            if (message == WM_SETCURSOR || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                message == WM_XBUTTONDBLCLK)
                return TRUE;
            return 0;
        }
        return original ? CallWindowProcW(original, window, message, wParam, lParam)
                        : DefWindowProcW(window, message, wParam, lParam);
    }

    bool RestoreWindowProc()
    {
        if (!g_subclassInstalled)
            return true;
        if (!IsWindow(g_window))
        {
            g_subclassInstalled = false;
            return true;
        }
        const WNDPROC original = OriginalWindowProc();
        if (!original)
            return false;
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
        // A later subclass may still call ours. Do not overwrite that component.
        if (current != WindowProc)
            return false;
        SetLastError(0);
        if (!SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original)) && GetLastError() != 0)
            return false;
        g_subclassInstalled = false;
        return true;
    }

    void DestroyRenderer()
    {
        gui_bridge::DestroyCapture();
        agent_preview::Shutdown();
        catalog_ui::Shutdown();
        g_econSnapshot = {};
        g_nextSchemaRead = 0;
        ImGuiContext *previous = ImGui::GetCurrentContext();
        if (g_imgui)
        {
            ImGui::SetCurrentContext(g_imgui);
            if (g_dx11Ready)
                ImGui_ImplDX11_Shutdown();
            if (g_win32Ready)
                ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(g_imgui);
            ImGui::SetCurrentContext(previous == g_imgui ? nullptr : previous);
        }
        g_imgui = nullptr;
        g_dx11Ready = false;
        g_win32Ready = false;
        Release(g_renderTarget);
        Release(g_deviceContext);
        Release(g_device);
        g_swapChain = nullptr;
        g_bufferWidth = g_bufferHeight = 0;
        // Keep the original proc valid for callbacks preceding restoration.
    }

    bool CreateRenderTarget(IDXGISwapChain *swapChain)
    {
        ID3D11Texture2D *buffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
            return false;
        const HRESULT result = g_device->CreateRenderTargetView(buffer, nullptr, &g_renderTarget);
        if (SUCCEEDED(result))
        {
            D3D11_TEXTURE2D_DESC description{};
            buffer->GetDesc(&description);
            g_bufferWidth = description.Width;
            g_bufferHeight = description.Height;
        }
        Release(buffer);
        return SUCCEEDED(result);
    }

    void ApplyMenuStyle()
    {
        menu_style::Initialize();
    }

    bool InitializeRenderer(IDXGISwapChain *swapChain)
    {
        DXGI_SWAP_CHAIN_DESC description{};
        if (FAILED(swapChain->GetDesc(&description)) || !IsWindow(description.OutputWindow) ||
            !IsWindowVisible(description.OutputWindow))
            return false;
        DWORD processId = 0;
        GetWindowThreadProcessId(description.OutputWindow, &processId);
        if (processId != GetCurrentProcessId())
            return false;
        if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
            return false;
        g_device->GetImmediateContext(&g_deviceContext);
        if (!g_deviceContext || !CreateRenderTarget(swapChain))
        {
            DestroyRenderer();
            return false;
        }

        ImGuiContext *previous = ImGui::GetCurrentContext();
        IMGUI_CHECKVERSION();
        g_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_imgui);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
        ApplyMenuStyle();
        g_win32Ready = ImGui_ImplWin32_Init(description.OutputWindow);
        if (g_win32Ready)
            g_dx11Ready = ImGui_ImplDX11_Init(g_device, g_deviceContext);
        if (!g_win32Ready || !g_dx11Ready || !ImGui_ImplDX11_CreateDeviceObjects())
        {
            ImGui::SetCurrentContext(previous);
            DestroyRenderer();
            return false;
        }
        ImGui::SetCurrentContext(previous);
        // Publish the original proc before the UI thread sees the replacement.
        g_window = description.OutputWindow;
        InterlockedExchangePointer(&g_windowProc, reinterpret_cast<PVOID>(GetWindowLongPtrW(g_window, GWLP_WNDPROC)));
        SetLastError(0);
        const LONG_PTR original = SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc));
        if (!original && GetLastError() != 0)
        {
            DestroyRenderer();
            return false;
        }
        InterlockedExchangePointer(&g_windowProc, reinterpret_cast<PVOID>(original));
        g_subclassInstalled = true;
        g_swapChain = swapChain;
        Log("ImGui initialized on the game swap chain.");
        return true;
    }

    void UpdateInput()
    {
        const bool foreground = GetForegroundWindow() == g_window;
        hvh::SetInputState(gui_bridge::MenuVisible(), static_cast<float>(g_bufferWidth), static_cast<float>(g_bufferHeight));
        ImGuiIO &io = ImGui::GetIO();
        const bool nativeInput = gui_bridge::MenuVisible();
        io.MouseDrawCursor = false;
        // Also support the SDL input path; wheel/text still use window messages.
        for (int key = 0; key < 256; ++key)
            io.KeysDown[key] = foreground && nativeInput && (GetAsyncKeyState(key) & 0x8000) != 0;
        const int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        for (int index = 0; index < 5; ++index)
            io.MouseDown[index] = foreground && nativeInput && (GetAsyncKeyState(buttons[index]) & 0x8000) != 0;
    }

    void RefreshSchema()
    {
        const ULONGLONG now=GetTickCount64();
        if(now>=g_nextSchemaRead)
        {
            g_econSnapshot=econ_schema::Read();
            catalog_ui::SetLiveDefinitionIndices(g_econSnapshot.available?g_econSnapshot.itemDefIds.data():nullptr,
                                                 g_econSnapshot.itemDefIds.size());
            g_nextSchemaRead=now+1000;
        }
    }

    void DrawPage(int page)
    {
        RefreshSchema();
            if(page<5)catalog_ui::Draw(g_device,static_cast<catalog_ui::Page>(page));
            else if(page==5)view_settings::Draw();
            else if(page==23)hvh::DrawMenu();
            else if(page==24)hvh::DrawAntiAimMenu();
            else if(page==25)movement::DrawMenu();
            else if(page==26)visual_features::DrawMenu(visual_features::Page::PenetrableSurfaces);
            else if(page==27)backtrack::DrawMenu();
            else if(page==28)visual_features::DrawMenu(visual_features::Page::Local);
            else if(page==29){
                bool enabled=false;
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled,true);
                ImGui::Checkbox("Unavailable in this build",&enabled);
                ImGui::PopItemFlag();
            }
            else if(page==30)visuals::gun_images::Menu(visuals::gun_images::MenuMode::Camo);
            else visual_features::DrawMenu(static_cast<visual_features::Page>(page-6));
        // Keep the former extra pages accessible inside the requested hierarchy.
        const auto extra=[&](const char* label,int id){
            if(ImGui::TreeNode(label)){ImGui::PushID(id);DrawPage(id);ImGui::PopID();ImGui::TreePop();}
        };
        if(page==23){extra("Surfaces",26);extra("History",27);}
        if(page==6)extra("Glow & radar",18);
        if(page==5)extra("Visibility",19);
    }

    void DrawMenu()
    {
        if (g_menuOpen) menu_shell::Draw(g_menuPage,g_menuOpen,g_agentPreviewOpen,DrawPage);
        if (catalog_ui::ConsumeAgentPreviewRequest())
            g_agentPreviewOpen = true;
        if (g_menuOpen)
        {
            g_agentPreviewOpen=true;
            agent_preview::Draw(g_device, g_deviceContext, &g_agentPreviewOpen);
        }
    }

    // The vendored renderer clears these stages without saving them.
    struct AuxiliaryShaders
    {
        ID3D11HullShader *hull = nullptr;
        ID3D11DomainShader *domain = nullptr;
        ID3D11ComputeShader *compute = nullptr;
        ID3D11ClassInstance *hullInstances[256]{}, *domainInstances[256]{}, *computeInstances[256]{};
        UINT hullCount = 256, domainCount = 256, computeCount = 256;
        AuxiliaryShaders()
        {
            g_deviceContext->HSGetShader(&hull, hullInstances, &hullCount);
            g_deviceContext->DSGetShader(&domain, domainInstances, &domainCount);
            g_deviceContext->CSGetShader(&compute, computeInstances, &computeCount);
        }
        ~AuxiliaryShaders()
        {
            g_deviceContext->HSSetShader(hull, hullInstances, hullCount);
            g_deviceContext->DSSetShader(domain, domainInstances, domainCount);
            g_deviceContext->CSSetShader(compute, computeInstances, computeCount);
            for (UINT i = 0; i < hullCount; ++i)
                Release(hullInstances[i]);
            for (UINT i = 0; i < domainCount; ++i)
                Release(domainInstances[i]);
            for (UINT i = 0; i < computeCount; ++i)
                Release(computeInstances[i]);
            Release(hull);
            Release(domain);
            Release(compute);
        }
    };

    struct OutputMergerState
    {
        ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView *depth = nullptr;
        ID3D11UnorderedAccessView *outputUavs[64]{}, *computeUavs[64]{};
        UINT initialCounts[64]{};
        UINT targetCount = 0, uavSlots = 0;
        OutputMergerState()
        {
            g_deviceContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                if (targets[i])
                    targetCount = i + 1;
            const D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
            uavSlots = level >= D3D_FEATURE_LEVEL_11_1
                           ? 64
                           : (level >= D3D_FEATURE_LEVEL_11_0 ? D3D11_PS_CS_UAV_REGISTER_COUNT : 0);
            for (auto &count : initialCounts)
                count = UINT(-1); // Preserve append counters.
            if (uavSlots)
            {
                if (targetCount < uavSlots)
                    g_deviceContext->OMGetRenderTargetsAndUnorderedAccessViews(
                        0, nullptr, nullptr, targetCount, uavSlots - targetCount, outputUavs + targetCount);
                // Binding the back-buffer RTV can also unbind a conflicting CS UAV.
                g_deviceContext->CSGetUnorderedAccessViews(0, uavSlots, computeUavs);
            }
        }
        ~OutputMergerState()
        {
            if (uavSlots)
            {
                if (targetCount < uavSlots)
                    g_deviceContext->OMSetRenderTargetsAndUnorderedAccessViews(
                        targetCount, targets, depth, targetCount, uavSlots - targetCount, outputUavs + targetCount,
                        initialCounts + targetCount);
                else
                    g_deviceContext->OMSetRenderTargets(targetCount, targets, depth);
                g_deviceContext->CSSetUnorderedAccessViews(0, uavSlots, computeUavs, initialCounts);
            }
            else
                g_deviceContext->OMSetRenderTargets(targetCount, targets, depth);
            for (auto &target : targets)
                Release(target);
            for (auto &uav : outputUavs)
                Release(uav);
            for (auto &uav : computeUavs)
                Release(uav);
            Release(depth);
        }
    };

    void RenderFrame(IDXGISwapChain *swapChain)
    {
        RenderScope lock;
        if (IsStopping() || g_resizing)
            return;
        if (g_swapChain && g_swapChain != swapChain)
        {
            DXGI_SWAP_CHAIN_DESC description{};
            if (FAILED(swapChain->GetDesc(&description)) || description.OutputWindow != g_window)
                return;
            if (!RestoreWindowProc())
                return;
            DestroyRenderer();
        }
        if (!g_imgui && !InitializeRenderer(swapChain))
            return;
        if (!g_renderTarget && !CreateRenderTarget(swapChain))
            return;
        ContextScope context(g_imgui);
        UpdateInput();
        if (IsStopping())
            return;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGuiIO &io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
            return;
        // Render and project using back-buffer pixels, including when the game's
        // client size and render resolution differ. Convert the mouse likewise.
        io.MousePos.x *= static_cast<float>(g_bufferWidth) / io.DisplaySize.x;
        io.MousePos.y *= static_cast<float>(g_bufferHeight) / io.DisplaySize.y;
        io.DisplaySize = ImVec2(static_cast<float>(g_bufferWidth), static_cast<float>(g_bufferHeight));
        gui_bridge::Process(DrawPage, g_device, g_deviceContext);
        ImGui::NewFrame();
        visual_features::Render();
        // Settings and character preview are rendered by the Tauri companion.
        // The retained ImGui context serves only scene overlays and semantic capture.
        hvh::SetInputState(gui_bridge::MenuVisible(), io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Render();
        // Supplement the old backend's state backup with targets, UAVs, HS/DS/CS.
        AuxiliaryShaders shaders;
        OutputMergerState output;
        g_deviceContext->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain *swapChain, UINT interval, UINT flags)
    {
        CallbackScope callback;
        if (!IsStopping() && !(flags & DXGI_PRESENT_TEST))
            RenderFrame(swapChain);
        // Present can wait on the message pump. Do not hold the render lock here.
        const HRESULT result = g_present(swapChain, interval, flags);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
        {
            RenderScope lock;
            if (swapChain == g_swapChain && RestoreWindowProc())
                DestroyRenderer();
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain *swapChain, UINT count, UINT width, UINT height,
                                         DXGI_FORMAT format, UINT flags)
    {
        CallbackScope callback;
        bool selected;
        {
            RenderScope lock;
            selected = swapChain == g_swapChain;
            if (selected)
            {
                g_resizing = true;
                Release(g_renderTarget);
            }
        }
        const HRESULT result = g_resize(swapChain, count, width, height, format, flags);
        if (selected)
        {
            RenderScope lock;
            g_resizing = false; // Next Present recreates the RTV, including on failure.
        }
        return result;
    }

    bool FindDx11Methods()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"Cs2OverlayDx11Probe";
        if (!RegisterClassExW(&windowClass))
            return false;
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 2, 2, nullptr, nullptr,
                                      instance, nullptr);
        if (!window)
        {
            UnregisterClassW(windowClass.lpszClassName, instance);
            return false;
        }
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 1;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain *chain = nullptr;
        ID3D11Device *device = nullptr;
        ID3D11DeviceContext *context = nullptr;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        HRESULT result =
            D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                          &description, &chain, &device, nullptr, &context);
        if (FAILED(result))
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &description, &chain, &device, nullptr, &context);
        if (SUCCEEDED(result))
        {
            void **methods = *reinterpret_cast<void ***>(chain);
            g_presentTarget = methods[8];
            g_resizeTarget = methods[13];
        }
        Release(context);
        Release(device);
        Release(chain);
        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        return SUCCEEDED(result);
    }

    bool InstallHooks()
    {
        if (!g_presentTarget && !FindDx11Methods())
        {
            Log("Could not create the DX11 probe device.");
            return false;
        }
        if (!g_minHookReady)
        {
            const MH_STATUS initialization = MH_Initialize();
            if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED)
                return false;
            g_minHookReady = true;
        }
        MH_STATUS status;
        if (!g_presentCreated)
        {
            status = MH_CreateHook(g_presentTarget, reinterpret_cast<void *>(PresentHook),
                                   reinterpret_cast<void **>(&g_present));
            if (status != MH_OK)
            {
                Log(MH_StatusToString(status));
                return false;
            }
            g_presentCreated = true;
        }
        if (!g_resizeCreated)
        {
            status = MH_CreateHook(g_resizeTarget, reinterpret_cast<void *>(ResizeHook),
                                   reinterpret_cast<void **>(&g_resize));
            if (status != MH_OK)
            {
                Log(MH_StatusToString(status));
                return false;
            }
            g_resizeCreated = true;
        }
        if (MH_QueueEnableHook(g_presentTarget) != MH_OK || MH_QueueEnableHook(g_resizeTarget) != MH_OK)
            return false;
        status = MH_ApplyQueued();
        if (status != MH_OK)
        {
            Log(MH_StatusToString(status));
            return false;
        }
        return true;
    }

    bool Cleanup()
    {
        InterlockedExchange(&g_stop, 1);
        gui_bridge::Stop();
        // Cancel native requests before waiting for the UI callback worker.
        // A timed-out stop retains all render state for a later cleanup retry.
        if (!gui_embedded::Stop())
            return false;
        if (!hvh::Stop())
            return false;
        if (!visual_features::Stop())
            return false;
        if (!view_settings::Stop())
            return false;
        if (!skin_changer::Stop())
            return false;
        game_access::verifiedClient = 0;
        if (g_presentCreated)
        {
            const MH_STATUS status = MH_DisableHook(g_presentTarget);
            if (status != MH_OK && status != MH_ERROR_DISABLED)
                return false;
        }
        if (g_resizeCreated)
        {
            const MH_STATUS status = MH_DisableHook(g_resizeTarget);
            if (status != MH_OK && status != MH_ERROR_DISABLED)
                return false;
        }
        {
            RenderScope lock;
            if (!RestoreWindowProc())
            {
                Log("A later window subclass still references this overlay; keep the image mapped.");
                return false;
            }
        }
        while (InterlockedCompareExchange(&g_callbacks, 0, 0) != 0)
            Sleep(1);
        {
            RenderScope lock;
            DestroyRenderer();
            g_window = nullptr;
            g_resizing = false;
        }
        // A callback may have entered a detour before incrementing g_callbacks.
        // Keep disabled hooks/trampolines and the original WndProc alive for the
        // mapped image's lifetime. Stopping is not permission to unmap the image.
        return true;
    }

    void OnGameFrame()
    {
        view_settings::Tick();
        agent_preview::Capture();
    }

    DWORD WINAPI Worker(LPVOID)
    {
        // Bootstrap the WebView and native features concurrently. The bridge keeps
        // feature requests blocked until both sides have finished initializing.
        gui_bridge::Start(true);
        const bool guiStarted=gui_embedded::Start();
        const auto guiDeadline=GetTickCount64()+20000;
        if(guiStarted)gui_bridge::Loading(20,"Connecting to CS2");
        const ULONGLONG deadline = GetTickCount64() + 30000;
        while (guiStarted&&!GetModuleHandleW(L"client.dll") && !IsStopping() &&
               !gui_embedded::Stopped() && GetTickCount64() < deadline)
            Sleep(100);
        if(guiStarted&&!IsStopping())gui_bridge::Loading(35,"Loading renderer");
        bool installed = guiStarted&&!IsStopping()&&!gui_embedded::Stopped() &&
                         GetModuleHandleW(L"client.dll") && InstallHooks();
        if (installed)
        {
            gui_bridge::Loading(50,"Loading features");
            skin_changer::SetStageCallback(OnGameFrame);
            game_access::verifiedClient =
                skin_changer::Install() ? reinterpret_cast<game_access::Address>(GetModuleHandleW(L"client.dll")) : 0;
            view_settings::Install();
            gui_bridge::Loading(70,"Loading visuals");
            visual_features::Install(game_access::verifiedClient.load() != 0);
            skin_changer::SetBeforeStageCallback(visual_features::BeforeStage);
            skin_changer::SetAfterStageCallback(visual_features::AfterStage);
            gui_bridge::Loading(90,"Loading combat");
            if (!hvh::Install()) Log("Gameplay input hook initialization failed.");
            Log(hvh::Status());
            // The bridge releases the menu as soon as the second side arrives.
            gui_bridge::FinishLoading();
            while(!gui_bridge::GuiReady()&&!IsStopping()&&!gui_embedded::Stopped()&&GetTickCount64()<guiDeadline)
                Sleep(10);
            installed=gui_bridge::GuiReady()&&!IsStopping()&&!gui_embedded::Stopped();
        }
        const bool startupFailed = !installed && !IsStopping();
        if (installed)
        {
            InterlockedExchange(&g_state, 2);
            Log("Hooks installed. INSERT opens the menu; END stops the overlay.");
            while (!IsStopping())
            {
                DWORD foregroundProcess{};
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
                const bool foreground = foregroundProcess == GetCurrentProcessId();
                const bool insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
                const bool end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
                if (foreground && insert && !g_insertDown)
                {
                    if (gui_embedded::Running()) gui_embedded::Toggle();
                    else gui_embedded::Start();
                }
                if (foreground && end && !g_endDown)
                    InterlockedExchange(&g_stop, 1);
                g_insertDown = insert;
                g_endDown = end;
                Sleep(20);
            }
        }
        else if (startupFailed)
            Log("Startup failed; check renderer and debugger output.");
        InterlockedExchange(&g_state, 3);
        const bool clean = Cleanup();
        InterlockedExchange(&g_state, clean && !startupFailed ? 0 : 4);
        return clean ? 0 : 1;
    }
} // namespace

BOOL overlay::Start()
{
    // Embedded Tauri depends on Windows loader TLS, activation contexts and a
    // retained module. Refuse unsupported mapping before any feature hooks.
    HMODULE registeredModule{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&overlay::Start), &registeredModule))
        return FALSE;
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (g_worker && WaitForSingleObject(g_worker, 0) == WAIT_TIMEOUT)
    {
        const BOOL running = !IsStopping();
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return running;
    }
    if (g_worker)
    {
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_imgui || g_subclassInstalled || !gui_embedded::Stopped())
    {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return FALSE;
    }
    g_menuOpen = false;
    g_insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    g_endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_state, 1);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, &g_workerId);
    if (!g_worker)
        InterlockedExchange(&g_state, 4);
    const BOOL started = g_worker != nullptr;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return started;
}

BOOL overlay::Stop()
{
    AcquireSRWLockExclusive(&g_lifecycleLock);
    InterlockedExchange(&g_stop, 1);
    if (!g_worker)
    {
        BOOL clean = !g_imgui && !g_subclassInstalled && gui_embedded::Stopped();
        if (!clean)
        {
            clean = Cleanup();
            InterlockedExchange(&g_state, clean ? 0 : 4);
        }
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return clean;
    }
    // Loader/mapper control thread only: never call from DllMain, WndProc,
    // or a render callback, since cleanup waits for those callbacks to finish.
    const DWORD wait = GetCurrentThreadId() == g_workerId ? WAIT_FAILED : WaitForSingleObject(g_worker, 10000);
    BOOL clean = wait == WAIT_OBJECT_0 && !g_imgui && !g_subclassInstalled && gui_embedded::Stopped();
    // Allow retry after another component removes a later window subclass.
    if (wait == WAIT_OBJECT_0 && !clean)
    {
        clean = Cleanup();
        InterlockedExchange(&g_state, clean ? 0 : 4);
    }
    if (wait == WAIT_OBJECT_0)
    {
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return clean;
}

DWORD overlay::State()
{
    return static_cast<DWORD>(InterlockedCompareExchange(&g_state, 0, 0));
}
