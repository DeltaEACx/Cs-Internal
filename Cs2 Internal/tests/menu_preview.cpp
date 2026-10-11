// Render the production menu on an offscreen WARP device. No game is loaded,
// no hooks are installed, and native schema polling is disabled.
#include "support/render_capture.h"
// WIC's RPC headers define this legacy keyword macro.
#ifdef small
#undef small
#endif
#include "ui/overlay.cpp"
#include "features/visuals/settings.hpp"
#include "features/visuals/effects/config.hpp"
#include "features/visuals/effects/world.hpp"
#include "imgui/imgui_internal.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <thread>

namespace menu_preview {
constexpr const char *windowName = "DeltaEAC###VeloraMain";
enum class Layout { Normal, Compact, Short, Narrow, Minimum };

struct Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    ImVec2 windowPosition{40, 32}, windowSize{1060, 748};
    bool widgetFixture = false;
    bool enabled = true, disabled = true, mixed = true, secondary = false;
    float amount = 45.f, angle = -30.f, minimum = 0.f, maximum = 100.f;
    float compact = .5f, vector[3]{-.5f, 0.f, .75f};
    int count = 8, shape = 1, buttonPresses = 0;
    bool sectionOpen = true;
    std::map<std::string, ImRect> widgetBounds;
    std::map<std::string, ImGuiID> widgetIds;
    std::vector<ImDrawVert> toggleVertices, sliderVertices;

    Renderer() {
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                         nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)),
              "WARP device");
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DeltaTime = 1.f / 60.f;
        io.MousePos = {-1000, -1000};
        io.KeyMap[ImGuiKey_Enter] = VK_RETURN;
        io.KeyMap[ImGuiKey_Escape] = VK_ESCAPE;
        io.KeyMap[ImGuiKey_A] = 'A';
        io.KeyMap[ImGuiKey_Backspace] = VK_BACK;
        io.KeyMap[ImGuiKey_Delete] = VK_DELETE;
        io.KeyMap[ImGuiKey_LeftArrow] = VK_LEFT;
        io.KeyMap[ImGuiKey_RightArrow] = VK_RIGHT;
        menu_style::Initialize();
        Check(ImGui_ImplDX11_Init(device.Get(), context.Get()), "Production ImGui renderer");
        g_device = device.Get();
        g_deviceContext = context.Get();
        g_menuOpen = true;
        g_agentPreviewOpen = false;
        g_nextSchemaRead = (std::numeric_limits<ULONGLONG>::max)();
        Resize(Layout::Normal);
    }

    ~Renderer() {
        agent_preview::Shutdown();
        catalog_ui::Shutdown();
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_device = nullptr;
        g_deviceContext = nullptr;
    }

    void Resize(Layout layout) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        view.Reset(); target.Reset(); texture.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        switch (layout) {
        case Layout::Normal: desc.Width=1140;desc.Height=812;windowPosition={40,32};windowSize={1060,748};break;
        case Layout::Compact: desc.Width=932;desc.Height=688;windowPosition={24,24};windowSize={884,640};break;
        case Layout::Short: desc.Width=1000;desc.Height=500;windowPosition={12,12};windowSize={976,476};break;
        case Layout::Narrow: desc.Width=640;desc.Height=720;windowPosition={12,12};windowSize={616,696};break;
        case Layout::Minimum: desc.Width=1000;desc.Height=384;windowPosition={12,12};windowSize={976,360};break;
        }
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "Menu texture");
        Check(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)), "Menu render target");
        Check(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)), "Menu capture view");
        ImGui::GetIO().DisplaySize = {static_cast<float>(desc.Width), static_cast<float>(desc.Height)};
    }

    void Frame(ImVec2 mouse = {-1000, -1000}, bool down = false, bool lockPosition = true, bool render = true) {
        auto &io = ImGui::GetIO();
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        if (ImGui::FindWindowByName(windowName)) {
            if(lockPosition)ImGui::SetWindowPos(windowName, windowPosition, ImGuiCond_Always);
            ImGui::SetWindowSize(windowName, windowSize, ImGuiCond_Always);
        }
        if (widgetFixture) DrawWidgetFixture(); else DrawMenu();
        ImGui::Render();
        Check(g_menuOpen && g_agentPreviewOpen, "Character preview must stay enabled beside the menu");
        Check(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 300,
              "Production menu draw data is empty");
        if (!render) return;
        const float background[]{.024f, .026f, .036f, 1.f};
        context->ClearRenderTargetView(target.Get(), background);
        auto *rtv = target.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    ImGuiWindow *Child(const char *name) {
        auto *main = ImGui::FindWindowByName(windowName);
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && window->ParentWindow == main && std::strstr(window->Name, name)) return window;
        return nullptr;
    }

    bool CatalogReady() {
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::strstr(window->Name, "##catalog_grid")) return true;
        return false;
    }

    void Capture(int page, const std::filesystem::path &output, Layout layout = Layout::Normal) {
        Resize(layout);
        g_menuPage = page;
        Frame();
        if (auto *controls = Child("/Controls_")) ImGui::SetScrollY(controls, 0);
        // Capture the finished page rather than an in-progress tab/control fade.
        for (int frame = 0; frame < 45; ++frame) Frame({-1000, -1000}, false, true, frame == 44);
        if (page == 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!CatalogReady() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                Frame();
            }
            Check(CatalogReady(), "Production catalog did not load");
            // Thumbnail decode budget is intentionally spread over frames.
            for (int frame = 0; frame < 40; ++frame) Frame();
        }
        auto *main = ImGui::FindWindowByName(windowName);
        Check(main && main->Size.x == windowSize.x && main->Size.y == windowSize.y,
              "Production menu did not honor preview dimensions");
        auto *controls = Child("/Controls_");
        Check(controls && controls->WindowPadding.x == 22.f && controls->WindowPadding.y == 18.f,
              "The borderless controls panel lost its content padding");
        Check(g_menuPage == page, "Page unexpectedly changed during capture");
        SavePng(device.Get(), context.Get(), view.Get(), output.c_str());
        std::cout << "Captured " << output.filename().string() << '\n';
    }

    void Click(const ImVec2 &position) {
        Frame(position); Frame(position, true); Frame(position, false); Frame(position);
    }

    void RecordWidget(const char *label) {
        widgetBounds[label] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        widgetIds[label] = ImGui::GetItemID();
    }

    void DrawWidgetFixture() {
        ImGui::SetNextWindowPos({150, 24}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({840, 764}, ImGuiCond_Always);
        ImGui::Begin("Controls###WidgetFixture", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("Controls");
        ImGui::Separator();
        auto *draw = ImGui::GetWindowDrawList();
        const auto toggleStart = draw->VtxBuffer.Size;
        ImGui::Checkbox("Enabled", &enabled); RecordWidget("Enabled");
        toggleVertices.assign(draw->VtxBuffer.begin() + toggleStart, draw->VtxBuffer.end());
        ImGui::SameLine(240); ImGui::Checkbox("Off", &secondary); RecordWidget("Off");
        ImGui::SameLine(440);
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * .45f);
        ImGui::Checkbox("Unavailable", &disabled); RecordWidget("Unavailable");
        ImGui::PopStyleVar(); ImGui::PopItemFlag();
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        ImGui::Checkbox("Mixed selection", &mixed); RecordWidget("Mixed selection");
        ImGui::PopItemFlag();
        ImGui::Spacing();
        ImGui::PushItemWidth(390);
        const auto sliderStart = draw->VtxBuffer.Size;
        ImGui::SliderFloat("Opacity", &amount, 0.f, 100.f, "%.1f%%"); RecordWidget("Opacity");
        sliderVertices.assign(draw->VtxBuffer.begin() + sliderStart, draw->VtxBuffer.end());
        ImGui::SliderInt("Samples", &count, 0, 20, "%d samples"); RecordWidget("Samples");
        ImGui::SliderFloat("Rotation", &angle, -180.f, 180.f, "%.1f deg"); RecordWidget("Rotation");
        ImGui::SliderFloat("Minimum", &minimum, 0.f, 100.f, "%.0f%%");
        ImGui::SliderFloat("Maximum", &maximum, 0.f, 100.f, "%.0f%%");
        ImGui::PushItemWidth(72);
        ImGui::SliderFloat("Compact", &compact, 0.f, 1.f, "%.2f"); RecordWidget("Compact");
        ImGui::PopItemWidth();
        ImGui::SameLine(252);
        ImGui::PushItemWidth(220);
        ImGui::SliderFloat3("Vector", vector, -1.f, 1.f, "%.1f");
        ImGui::PopItemWidth();
        static const char *shapes[]{"Full", "Cornered", "Rounded"};
        ImGui::Combo("Box style", &shape, shapes, 3); RecordWidget("Box style");
        ImGui::PopItemWidth();
        ImGui::Spacing();
        if (ImGui::Button("Apply", {118, 32})) ++buttonPresses;
        RecordWidget("Apply");
        ImGui::SameLine();
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * .45f);
        if (ImGui::Button("Unavailable action", {174, 32})) ++buttonPresses;
        RecordWidget("Unavailable action");
        ImGui::PopStyleVar(); ImGui::PopItemFlag();
        ImGui::Spacing();
        sectionOpen = ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen);
        RecordWidget("Appearance");
        if (sectionOpen) {
            static float tint[]{.886f, .192f, .270f, 1.f};
            static char name[64] = "Custom profile";
            ImGui::PushItemWidth(390);
            ImGui::ColorEdit4("Accent", tint);
            ImGui::InputText("Profile", name, sizeof(name));
            ImGui::PopItemWidth();
        }
        ImGui::End();
    }

    void ProductionToggle() {
        const auto saved = visuals::g_settings;
        Resize(Layout::Normal);
        g_menuPage = 6;
        visuals::g_settings.player_box = false;
        Frame(); Frame();
        auto *controls = Child("/Controls_");
        Check(controls != nullptr, "Player ESP controls missing for switch test");
        ImGui::SetScrollY(controls, 0); Frame(); Frame();
        const auto rowId = ImHashStr("Box", 0, controls->ID);
        const auto boxId = ImHashStr("Box", 0, rowId);
        auto position = FindControl(controls, boxId, controls->WorkRect.Min.x + 10.f);
        Click(position);
        Check(visuals::g_settings.player_box, "Clicking the Box switch did not enable its real setting");
        position = FindControl(controls, boxId, controls->WorkRect.Min.x + ImGui::GetFrameHeight()
            + ImGui::GetStyle().ItemInnerSpacing.x + 8.f);
        Click(position);
        Check(!visuals::g_settings.player_box, "Clicking the Box label did not disable its real setting");
        Check(Popup(false) == nullptr, "Clicking the Box switch or label opened its options popup");
        visuals::g_settings = saved;
        Frame();
        std::cout << "PASS: production ESP switch and label change the real setting independently of its options\n";
    }

    void NativeControls(const std::filesystem::path &output) {
        Resize(Layout::Normal);
        widgetFixture = true;
        Frame(); Frame();
        Check(ImGui::GetStyle().CustomControls, "Control fixture must use the production custom style");
        const auto center = [&](const char *label) { return widgetBounds.at(label).GetCenter(); };
        const auto slider = [&](const char *label, float ratio) {
            const auto &bounds = widgetBounds.at(label);
            return ImVec2(bounds.Min.x + 390.f * ratio, bounds.Max.y - 9.f);
        };
        Click(center("Unavailable"));
        Check(disabled, "Disabled switch unexpectedly changed");
        Click(center("Unavailable action"));
        Check(buttonPresses == 0, "Disabled action unexpectedly fired");
        Click(center("Apply"));
        Check(buttonPresses == 1, "Custom action did not fire exactly once");
        Click(center("Mixed selection"));
        Check(!mixed, "Mixed switch no longer responds to clicks");
        Click(center("Appearance"));
        Check(!sectionOpen, "Custom section did not collapse");
        Click(center("Appearance"));
        Check(sectionOpen, "Custom section did not expand");

        Click(slider("Opacity", .24f));
        Check(amount > 0.f && amount < 45.f, "Float slider click did not update its value");
        auto start = slider("Opacity", .24f), farRight = slider("Opacity", 1.2f), farLeft = slider("Opacity", -.2f);
        Frame(start); Frame(start, true); Frame(farRight, true); Frame(farRight);
        Check(amount == 100.f, "Float slider drag did not clamp to its maximum");
        start = slider("Opacity", .3f);
        Frame(start); Frame(start, true); Frame(farLeft, true); Frame(farLeft);
        Check(amount == 0.f, "Float slider drag did not clamp to its minimum");
        Click(slider("Samples", .2f));
        Check(count > 0 && count < 8, "Integer slider click did not update its value");
        start = slider("Samples", .3f);
        Frame(start); Frame(start, true); Frame(slider("Samples", 1.2f), true); Frame();
        Check(count == 20, "Integer slider drag did not reach its maximum");
        start = slider("Samples", .3f);
        Frame(start); Frame(start, true); Frame(slider("Samples", -.2f), true); Frame();
        Check(count == 0, "Integer slider drag did not reach its minimum");
        Click(slider("Rotation", .25f));
        Check(angle < 0.f && angle > -180.f, "Signed slider lost its negative range");
        const auto compactBounds = widgetBounds.at("Compact");
        const ImVec2 compactStart{compactBounds.Min.x + 18.f, compactBounds.Max.y - 9.f};
        Click(compactStart);
        Check(compact > 0.f && compact < .5f, "Compact slider lost its usable track");
        Frame(compactStart); Frame(compactStart, true);
        Frame({compactBounds.Min.x + 96.f, compactStart.y}, true); Frame();
        Check(compact == 1.f, "Compact slider drag did not reach its maximum");
        Frame(compactStart); Frame(compactStart, true);
        Frame({compactBounds.Min.x - 24.f, compactStart.y}, true); Frame();
        Check(compact == 0.f, "Compact slider drag did not reach its minimum");

        // Use the same Ctrl-click and typed-text route available to menu users.
        // Text entry intentionally retains ImGui's existing out-of-range policy.
        const auto enterText = [&](const char *label, const char *text, bool useCtrl = true) {
            auto &io = ImGui::GetIO();
            auto position = slider(label, useCtrl ? .3f : .95f);
            if (!useCtrl) position.y = widgetBounds.at(label).Min.y + 10.f;
            Frame(position);
            io.KeyCtrl = useCtrl;
            Frame(position, true); Frame(position);
            Check(ImGui::TempInputTextIsActive(widgetIds.at(label)), "Slider did not activate native text input");
            io.KeyCtrl = false;
            io.AddInputCharactersUTF8(text);
            Frame(position);
            io.KeysDown[VK_RETURN] = true; Frame(position);
            io.KeysDown[VK_RETURN] = false; Frame(position);
        };
        enterText("Opacity", "62.5");
        Check(std::abs(amount - 62.5f) < .01f, "Float slider text entry ignored the typed value");
        enterText("Samples", "13");
        Check(count == 13, "Integer slider text entry ignored the typed value");
        enterText("Opacity", "54.5", false);
        Check(std::abs(amount - 54.5f) < .01f, "Clicking the slider value field did not allow editing");
        Frame(); Frame();
        for (int frame = 0; frame < 20; ++frame) Frame();
        for (int list = 0; list < ImGui::GetDrawData()->CmdListsCount; ++list)
            for (const auto &vertex : ImGui::GetDrawData()->CmdLists[list]->VtxBuffer)
                Check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "Control rendering emitted invalid coordinates");
        SavePng(device.Get(), context.Get(), view.Get(), output.c_str());
        std::cout << "Captured " << output.filename().string() << '\n';
        widgetFixture = false;
        Frame(); Frame();
        std::cout << "PASS: custom switches, disabled actions, collapsing sections, float/integer slider click/drag endpoints, Ctrl-click and direct value text entry\n";
    }

    void ControlAnimation(const std::filesystem::path &output) {
        struct Snapshot {
            std::vector<ImDrawVert> toggle, slider;
        };
        struct Difference { float pixels = 0.f; int color = 0; };
        const auto difference = [](const std::vector<ImDrawVert> &a, const std::vector<ImDrawVert> &b) {
            Check(a.size() == b.size() && !a.empty(), "Control animation changed its draw topology unexpectedly");
            Difference result;
            for (size_t i = 0; i < a.size(); ++i) {
                Check(std::isfinite(a[i].pos.x) && std::isfinite(a[i].pos.y)
                    && std::isfinite(b[i].pos.x) && std::isfinite(b[i].pos.y),
                    "Control animation emitted invalid coordinates");
                result.pixels = std::max({result.pixels, std::abs(a[i].pos.x - b[i].pos.x),
                                         std::abs(a[i].pos.y - b[i].pos.y)});
                for (int shift = 0; shift < 32; shift += 8)
                    result.color = std::max(result.color, std::abs(static_cast<int>((a[i].col >> shift) & 255)
                                                                  - static_cast<int>((b[i].col >> shift) & 255)));
            }
            return result;
        };
        const auto snapshot = [&]() { return Snapshot{toggleVertices, sliderVertices}; };
        const auto advance = [&](int frames, bool render = false) {
            for (int frame = 0; frame < frames; ++frame)
                Frame({-1000, -1000}, false, true, render && frame == frames - 1);
        };
        Resize(Layout::Normal);
        widgetFixture = true;
        auto &io = ImGui::GetIO();
        const float savedDelta = io.DeltaTime, savedAmount = amount;
        const bool savedEnabled = enabled;
        std::vector<Snapshot> equalTime;
        for (const int rate : {30, 60, 144}) {
            io.DeltaTime = 1.f / rate;
            enabled = false; amount = 20.f;
            advance(rate);
            enabled = true; amount = 80.f;
            advance(1);
            const auto first = snapshot();
            // One sixth of a second is an exact whole-frame interval at all
            // three rates, so comparisons measure elapsed time, not frame count.
            advance(rate / 6 - 1, rate == 60);
            equalTime.push_back(snapshot());
            if (rate == 60)
                SavePng(device.Get(), context.Get(), view.Get(), (output / L"velora_controls_transition.png").c_str());
            advance(rate, rate == 60);
            const auto settled = snapshot();
            Check(difference(first.toggle, settled.toggle).pixels > .5f,
                  "Toggle thumb snaps to its final position instead of animating");
            Check(difference(first.slider, settled.slider).pixels > .5f,
                  "Slider thumb snaps after a changed value instead of animating");
            Check(amount == 80.f, "Slider animation changed the configured value");
            advance(rate / 2);
            const auto later = snapshot();
            Check(difference(settled.toggle, later.toggle).pixels < .05f
                && difference(settled.slider, later.slider).pixels < .05f,
                "Control animation did not settle at a stable endpoint");
            if (rate == 60) {
                Frame();
                SavePng(device.Get(), context.Get(), view.Get(), (output / L"velora_controls_settled.png").c_str());
            }
        }
        for (size_t i = 1; i < equalTime.size(); ++i) {
            const auto toggle = difference(equalTime[0].toggle, equalTime[i].toggle);
            const auto slider = difference(equalTime[0].slider, equalTime[i].slider);
            Check(toggle.pixels < .15f && toggle.color <= 2,
                  "Toggle animation speed changes with frame rate");
            Check(slider.pixels < .15f && slider.color <= 2,
                  "Slider animation speed changes with frame rate");
        }
        io.DeltaTime = savedDelta;
        enabled = savedEnabled; amount = savedAmount;
        widgetFixture = false;
        Frame(); Frame();
        std::cout << "PASS: toggle and slider animate, settle and match at equal elapsed time under 30/60/144Hz\n";
    }

    ImGuiWindow *Popup(bool combo) {
        for (auto *window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active || !(window->Flags & ImGuiWindowFlags_Popup)) continue;
            if ((std::strstr(window->Name, "##Combo_") != nullptr) == combo) return window;
        }
        return nullptr;
    }

    ImVec2 FindControl(ImGuiWindow *window, ImGuiID id, float x) {
        Check(window != nullptr, "Control window missing");
        const auto top = window->InnerClipRect.Min.y + 3.f;
        const auto bottom = window->InnerClipRect.Max.y - 3.f;
        for (float y = top; y <= bottom; y += 4.f) {
            const ImVec2 position{x, y};
            Frame(position, false, true, false);
            if (ImGui::GetCurrentContext()->HoveredId == id) return position;
        }
        throw std::runtime_error("Could not find the expected production control by its ImGui ID");
    }

    void BoxOptions(const std::filesystem::path &output, Layout layout = Layout::Normal) {
        const auto saved = visuals::g_settings;
        Resize(layout);
        g_menuPage = 6;
        visuals::g_settings.boxes = visuals::box_style::corners;
        // Disabled ESP controls must still allow appearance to be configured.
        visuals::g_settings.player_box = false;
        Frame(); Frame();
        auto *controls = Child("/Controls_");
        Check(controls != nullptr, "Player ESP controls missing");
        ImGui::SetScrollY(controls, 0);
        Frame(); Frame();
        const auto rowId = ImHashStr("Box", 0, controls->ID);
        const auto optionsId = ImHashStr("##options", 0, rowId);
        const auto dots = FindControl(controls, optionsId, controls->WorkRect.Max.x - 16.f);
        Click(dots);
        Check(!visuals::g_settings.player_box, "Three-dot button toggled the Box checkbox");
        auto *options = Popup(false);
        Check(options != nullptr, "Box three-dot button did not open its options");
        const auto style = FindControl(options, ImHashStr("Style", 0, options->ID), options->WorkRect.Min.x + 70.f);
        Click(style);
        auto *choices = Popup(true);
        Check(choices != nullptr, "Box style combo did not open");
        SavePng(device.Get(), context.Get(), view.Get(), output.c_str());
        std::cout << "Captured " << output.filename().string() << '\n';
        const auto index = reinterpret_cast<void *>(static_cast<intptr_t>(2));
        const auto choiceSeed = ImHashData(&index, sizeof(index), choices->ID);
        const auto rounded = FindControl(choices, ImHashStr("Rounded", 0, choiceSeed), choices->WorkRect.Min.x + 55.f);
        Click(rounded);
        Check(visuals::g_settings.boxes == visuals::box_style::rounded,
              "Selecting Rounded in the Box popup did not change the renderer setting");
        Check(!visuals::g_settings.player_box, "Selecting a Box style toggled the Box checkbox");
        // Dismiss the parent popup through the same outside-click path a user takes.
        Click({4.f, 4.f});
        Check(Popup(false) == nullptr && Popup(true) == nullptr, "Options popup did not dismiss");
        visuals::g_settings = saved;
        Frame();
        std::cout << "PASS: Box three-dot popup changes style without toggling disabled ESP\n";
    }

    void CrimsonSky(const std::filesystem::path &output) {
        struct RestoreConfiguration {
            custom::configuration value = custom::get_configuration();
            ~RestoreConfiguration() { custom::set_configuration(value); }
        } restore;
        const auto entries = custom::world::skyboxes();
        constexpr int crimson = 17;
        Check(entries.size() > crimson && entries[crimson].label == "Crimson Dawn",
              "Crimson Dawn sky preset is missing or reordered existing choices");
        Check(entries[crimson].resource_path == "materials/custom/skybox/crimson_dawn_sky.vtex",
              "Crimson Dawn sky preset points to the wrong resource");

        auto initial = restore.value;
        initial.enabled = true;
        initial.skybox = true;
        initial.skybox_index = 0;
        initial.tint_sky = true;
        initial.match_sky_lighting = true;
        initial.lighting = false;
        initial.rainbow_lighting = true;
        initial.lighting_color = {.78f, .19f, .75f};
        initial.lighting_intensity = 3.88f;
        initial.fog = true;
        initial.fog_color = {.10f, .065f, .18f};
        initial.sky_texture.fill(0);
        constexpr char manualTexture[] = "materials/custom/skybox/manual_test.vtex";
        std::copy(std::begin(manualTexture), std::end(manualTexture), initial.sky_texture.begin());
        custom::set_configuration(initial);
        Resize(Layout::Normal);
        g_menuPage = 12;
        Frame(); Frame();
        auto *controls = Child("/Controls_");
        Check(controls != nullptr, "Sky controls missing");
        ImGui::SetScrollY(controls, 0); Frame(); Frame();

        const auto selectSky = [&](int index) {
            const auto comboId = ImHashStr("Sky image", 0, controls->ID);
            Click(FindControl(controls, comboId, controls->WorkRect.Min.x + 70.f));
            auto *choices = Popup(true);
            Check(choices != nullptr, "Sky image combo did not open");
            const auto choiceId = ImHashStr(entries[index].label.data(), 0, choices->ID);
            bool scrolled = false;
            // Find visible entries by their real hovered IDs. Scroll with the
            // mouse wheel so the last preset is tested through the user path.
            for (int attempt = 0; attempt < 12; ++attempt) {
                const float x = choices->WorkRect.Min.x + 70.f;
                for (float y = choices->InnerClipRect.Min.y + 3.f;
                     y < choices->InnerClipRect.Max.y - 3.f; y += 4.f) {
                    const ImVec2 position{x, y};
                    Frame(position, false, true, false);
                    if (ImGui::GetCurrentContext()->HoveredId != choiceId) continue;
                    Click(position);
                    Check(Popup(true) == nullptr, "Sky choice did not dismiss the combo");
                    Check(custom::get_configuration().skybox_index == index,
                          "Clicking a sky preset selected the wrong entry");
                    return scrolled;
                }
                ImGui::GetIO().MouseWheel = index > 8 ? -3.f : 3.f;
                Frame(choices->InnerClipRect.GetCenter());
                Frame(choices->InnerClipRect.GetCenter());
                scrolled = true;
            }
            throw std::runtime_error("Sky preset is unreachable by scrolling the dropdown");
        };

        Check(selectSky(crimson), "Crimson Dawn test did not exercise dropdown scrolling");
        auto selected = custom::get_configuration();
        Check(selected.skybox && selected.sky_texture[0] == '\0',
              "Selecting Crimson Dawn did not clear the manual sky texture override");
        Check(!selected.tint_sky, "Crimson Dawn did not start with neutral sky tint");
        Check(custom::world::sky_lighting_active(selected), "Crimson Dawn did not link lighting by default");
        auto effective = custom::world::effective_configuration(selected);
        Check(effective.lighting && !effective.rainbow_lighting && effective.lighting_intensity < 1.5f,
              "Crimson Dawn retained manual purple/rainbow lighting intensity");
        Check(selected.lighting_color == initial.lighting_color && selected.lighting_intensity == initial.lighting_intensity
              && selected.fog_color == initial.fog_color && !selected.lighting && selected.rainbow_lighting,
              "Sky selection destroyed saved manual lighting settings");
        Capture(12, output);

        controls = Child("/Controls_");
        auto matchPosition = FindControl(controls, ImHashStr("Match sky lighting", 0, controls->ID),
                                         controls->WorkRect.Min.x + 10.f);
        Click(matchPosition);
        selected = custom::get_configuration();
        effective = custom::world::effective_configuration(selected);
        Check(!selected.match_sky_lighting && !custom::world::sky_lighting_active(selected)
              && effective.lighting_color == initial.lighting_color && effective.fog_color == initial.fog_color,
              "Sky lighting toggle did not restore manual light and fog colors");
        Click(matchPosition);
        Check(custom::world::sky_lighting_active(custom::get_configuration()), "Sky lighting toggle did not reactivate");
        Capture(13, output.parent_path() / L"velora_crimson_lighting_menu.png");
        controls = Child("/Controls_");
        matchPosition = FindControl(controls, ImHashStr("Match sky lighting", 0, controls->ID),
                                    controls->WorkRect.Min.x + 10.f);
        Click(matchPosition);
        Check(!custom::get_configuration().match_sky_lighting, "Lighting page did not update the shared sky matching toggle");
        Click(matchPosition);
        Check(custom::world::sky_lighting_active(custom::get_configuration()), "Lighting page could not reactivate sky matching");
        g_menuPage = 12; Frame(); Frame();
        controls = Child("/Controls_");
        Click(FindControl(controls, ImHashStr("Sky tint", 0, controls->ID),
                          controls->WorkRect.Min.x + 10.f));
        for (int frame = 0; frame < 4; ++frame) Frame();
        Check(custom::get_configuration().tint_sky,
              "Manually enabling Crimson Dawn tint was reset during subsequent frames");
        selectSky(1);
        Check(custom::get_configuration().tint_sky,
              "Selecting a standard sky preset unexpectedly disabled manual tint");
        effective = custom::world::effective_configuration(custom::get_configuration());
        Check(!custom::world::sky_lighting_active(custom::get_configuration())
              && effective.lighting_color == initial.lighting_color && effective.lighting_intensity == initial.lighting_intensity,
              "Switching skies failed to restore manual lighting");
        Click(FindControl(controls, ImHashStr("Sky tint", 0, controls->ID),
                          controls->WorkRect.Min.x + 10.f));
        selectSky(2);
        Check(!custom::get_configuration().tint_sky,
              "Selecting a standard sky preset unexpectedly enabled manual tint");
        custom::set_configuration(restore.value);
        Frame();
        std::cout << "PASS: Crimson Dawn selection, resource mapping, neutral tint, matched light/fog, shared toggles and manual-setting restoration\n";
    }

    ImGuiID NavigationId(const char *label, int index) {
        auto *main = ImGui::FindWindowByName(windowName);
        Check(main != nullptr, "Main menu window missing");
        return ImHashStr(label, 0, ImHashData(&index, sizeof(index), main->ID));
    }

    std::map<ImGuiID, ImVec2> NavigationControls(bool groups) {
        auto *main = ImGui::FindWindowByName(windowName);
        auto *controls = Child("/Controls_");
        Check(main && controls, "Production menu or controls missing");
        std::map<ImGuiID, ImVec2> found;
        std::vector<ImGuiID> expected;
        if (groups) {
            for (int group = 0; group < std::size(menu_shell::groups); ++group)
                expected.push_back(NavigationId("group", group));
        } else {
            for (int page : menu_shell::navigationPages)
                if (menu_shell::pages[page].group == menu_shell::pages[g_menuPage].group)
                    expected.push_back(NavigationId("page", page));
        }
        // Discover real interactive bounds through ImGui's hovered IDs. This also
        // fails if a tab is drawn but clipped or covered by another control.
        const ImRect bounds = groups
            ? ImRect(main->Pos, {controls->Pos.x - 1.f, main->Pos.y + main->Size.y})
            : ImRect({controls->Pos.x, main->Pos.y}, {main->Pos.x + main->Size.x, controls->Pos.y - 1.f});
        for (float y = bounds.Min.y + 4.f; y < bounds.Max.y; y += 12.f) {
            for (float x = bounds.Min.x + 4.f; x < bounds.Max.x; x += 16.f) {
                const ImVec2 position{x, y};
                Frame(position, false, true, false);
                const auto hovered = ImGui::GetCurrentContext()->HoveredId;
                if (std::find(expected.begin(), expected.end(), hovered) != expected.end())
                    found.try_emplace(hovered, position);
                if (found.size() == expected.size()) return found;
            }
        }
        throw std::runtime_error(groups ? "A primary navigation item is unreachable" : "A subtab is unreachable or clipped");
    }

    void Navigation(Layout layout = Layout::Normal) {
        Resize(layout);
        g_menuPage = 23;
        Frame(); Frame();
        const auto groupControls = NavigationControls(true);
        std::array<int, std::size(menu_shell::groups)> remembered{};
        int reached = 0;
        for (int group = 0; group < std::size(menu_shell::groups); ++group) {
            Click(groupControls.at(NavigationId("group", group)));
            Check(menu_shell::pages[g_menuPage].group == group, "Primary navigation selected the wrong group");
            const auto pageControls = NavigationControls(false);
            for (int page : menu_shell::navigationPages) {
                if (menu_shell::pages[page].group != group) continue;
                Click(pageControls.at(NavigationId("page", page)));
                Check(g_menuPage == page, "Subtab selected the wrong page");
                auto *controls = Child("/Controls_");
                Check(controls != nullptr, "Selected page has no controls child");
                Check(Child("/Sections_") == nullptr, "Old section sidebar is still present");
                const auto lastRow = std::max_element(pageControls.begin(), pageControls.end(),
                    [](const auto &a, const auto &b) { return a.second.y < b.second.y; });
                Check(controls->Pos.y - lastRow->second.y < 70.f,
                      "A duplicate page-title strip still separates tabs from controls");
                remembered[group] = page;
                ++reached;
            }
        }
        Check(reached == std::size(menu_shell::navigationPages), "Not all production pages were reached");
        for (int group = static_cast<int>(std::size(menu_shell::groups)) - 1; group >= 0; --group) {
            Click(groupControls.at(NavigationId("group", group)));
            Check(g_menuPage == remembered[group], "Switching groups forgot the last selected subtab");
        }
        std::cout << "PASS: all " << std::size(menu_shell::navigationPages) << " pages reachable and each group remembers its subtab (layout "
                  << static_cast<int>(layout) << ")\n";
    }

    void Drag() {
        Resize(Layout::Normal);
        g_menuPage = 23;
        Frame(); Frame();
        auto *main = ImGui::FindWindowByName(windowName);
        const auto before = main->Pos;
        const ImVec2 start{before.x+75.f,before.y+68.f};
        const ImVec2 moved{start.x+18.f,start.y+10.f};
        Frame(start,false,false);Frame(start,true,false);
        Frame(moved,true,false);Frame(moved,false,false);
        const auto after = main->Pos;
        Check(std::abs(after.x-before.x-18.f)<.1f && std::abs(after.y-before.y-10.f)<.1f,
              "Dragging empty header space did not move the production menu");
        Check(g_menuPage==23,"Dragging the menu changed its active page");
        Frame();
        std::cout << "PASS: native header dragging\n";
    }

    void WrappedTabs(Layout layout, bool requireWrap = true) {
        Resize(layout);
        g_menuPage = 12;
        Frame(); Frame();
        const auto tabs = NavigationControls(false);
        const auto rows = std::minmax_element(tabs.begin(), tabs.end(),
            [](const auto &a, const auto &b) { return a.second.y < b.second.y; });
        Check(!requireWrap || rows.second->second.y - rows.first->second.y > 25.f,
              "World subtabs did not wrap into accessible rows");
        auto *controls = Child("/Controls_");
        Check(controls && controls->Size.y > 200.f, "Wrapped tabs leave too little room for page controls");
        for (int page : menu_shell::navigationPages) {
            if (menu_shell::pages[page].group != 4) continue;
            Click(tabs.at(NavigationId("page", page)));
            Check(g_menuPage == page, "A wrapped World subtab does not activate");
        }
        Check(Popup(false) == nullptr && Popup(true) == nullptr, "Tab navigation unexpectedly opened a popup");
        std::cout << "PASS: World subtabs remain clickable with sufficient controls space (layout " << static_cast<int>(layout) << ")\n";
    }
};
}

int wmain(int argc, wchar_t **argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Check(SUCCEEDED(com), "COM initialization");
        const std::filesystem::path output = argc > 1 ? argv[1] : L"tests/artifacts";
        std::filesystem::create_directories(output);
        {
            menu_preview::Renderer renderer;
            renderer.Capture(23, output / L"velora_hvh.png");
            renderer.Capture(24, output / L"velora_anti_aim.png");
            renderer.Capture(25, output / L"velora_movement.png");
            renderer.Capture(26, output / L"velora_surfaces.png");
            renderer.Capture(9, output / L"velora_chams.png");
            renderer.Capture(5, output / L"velora_camera.png");
            renderer.Capture(19, output / L"velora_viewmodel.png");
            renderer.Capture(6, output / L"velora_player_esp.png");
            renderer.Capture(7, output / L"velora_item_esp.png");
            renderer.Capture(8, output / L"velora_hud.png");
            renderer.Capture(0, output / L"velora_catalog.png");
            renderer.Capture(23, output / L"velora_compact_hvh.png", menu_preview::Layout::Compact);
            renderer.Capture(25, output / L"velora_compact_movement.png", menu_preview::Layout::Compact);
            renderer.Capture(19, output / L"velora_compact_viewmodel.png", menu_preview::Layout::Compact);
            renderer.Capture(26, output / L"velora_short_surfaces.png", menu_preview::Layout::Short);
            renderer.Capture(23, output / L"velora_minimum_hvh.png", menu_preview::Layout::Minimum);
            renderer.Capture(23, output / L"velora_narrow_hvh.png", menu_preview::Layout::Narrow);
            renderer.Capture(25, output / L"velora_narrow_movement.png", menu_preview::Layout::Narrow);
            renderer.Capture(19, output / L"velora_narrow_viewmodel.png", menu_preview::Layout::Narrow);
            renderer.Capture(26, output / L"velora_narrow_surfaces.png", menu_preview::Layout::Narrow);
            renderer.Capture(6, output / L"velora_compact_esp.png", menu_preview::Layout::Compact);
            renderer.Capture(6, output / L"velora_narrow_esp.png", menu_preview::Layout::Narrow);
            renderer.BoxOptions(output / L"velora_box_options.png");
            renderer.BoxOptions(output / L"velora_narrow_box_options.png", menu_preview::Layout::Narrow);
            renderer.CrimsonSky(output / L"velora_crimson_sky_menu.png");
            renderer.ProductionToggle();
            renderer.NativeControls(output / L"velora_controls.png");
            renderer.ControlAnimation(output);
            renderer.Navigation();
            renderer.Drag();
            renderer.Navigation(menu_preview::Layout::Narrow);
            renderer.Navigation(menu_preview::Layout::Short);
            renderer.Navigation(menu_preview::Layout::Minimum);
            renderer.WrappedTabs(menu_preview::Layout::Narrow);
            renderer.WrappedTabs(menu_preview::Layout::Short, false);
        }
        CoUninitialize();
        std::cout << "PASS: production menu captures, Crimson Dawn sky selection, custom-control showcase and interactions, all-page navigation through minimum height, group memory, wrapped tabs and ESP options\n";
        return 0;
    } catch (const std::exception &error) {
        if (SUCCEEDED(com)) CoUninitialize();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
