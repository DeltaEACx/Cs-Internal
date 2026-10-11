#define NOMINMAX
#include <Windows.h>
#include "ui/gui_transport.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::wstring pipe_name() { return L"\\\\.\\pipe\\velora_gui_v1_" + std::to_wstring(GetCurrentProcessId()); }
HANDLE connect() {
    for (int retry = 0; retry != 100; ++retry) {
        const auto handle = CreateFileW(pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) return handle;
        Sleep(10);
    }
    throw std::runtime_error("The test client could not connect");
}
std::string exchange(const std::string& request, bool fragment = false) {
    const auto pipe = connect();
    DWORD count{};
    if (fragment) {
        require(WriteFile(pipe, request.data(), 3, &count, nullptr) && count == 3, "Fragment write failed");
        Sleep(5);
    }
    const auto offset = fragment ? 3 : 0;
    require(WriteFile(pipe, request.data() + offset, static_cast<DWORD>(request.size() - offset), &count, nullptr) && count == request.size() - offset, "Request write failed");
    std::string response;
    char bytes[8192];
    while (ReadFile(pipe, bytes, sizeof(bytes), &count, nullptr) && count) {
        response.append(bytes, count);
        if (response.back() == '\n') break;
    }
    CloseHandle(pipe);
    return response;
}
void stop_with_deadline() {
    const auto before = std::chrono::steady_clock::now();
    gui_transport::Stop();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    require(elapsed < std::chrono::milliseconds(500), "Stop exceeded its I/O cancellation deadline");
}
}

int main() {
    try {
        std::atomic<int> calls{};
        const auto handler = [&](const std::string& request) {
            ++calls;
            if (request == "throw") throw std::runtime_error("Intentional test failure");
            if (request == "big") return std::string(1024 * 1024, 'x');
            return request;
        };
        require(gui_transport::Start(handler), "Transport startup failed");
        const std::string large = "{\"value\":\"" + std::string(96 * 1024, 'x') + "\"}\n";
        require(exchange(large, true) == large, "Fragmented large roundtrip failed");
        require(exchange("{\"value\":\"\xE6\xA1\x9C\"}\n") == "{\"value\":\"\xE6\xA1\x9C\"}\n", "UTF-8 did not survive roundtrip");
        require(exchange("throw\n").find("\"ok\":false") != std::string::npos, "Handler exception escaped transport");
        require(calls == 3, "Unexpected request count");
        stop_with_deadline();

        require(gui_transport::Start(handler), "Transport restart failed");
        auto stalled = connect();
        DWORD count{};
        WriteFile(stalled, "{", 1, &count, nullptr);
        Sleep(15);
        stop_with_deadline();
        CloseHandle(stalled);

        require(gui_transport::Start(handler), "Transport second restart failed");
        stalled = connect();
        WriteFile(stalled, "big\n", 4, &count, nullptr);
        Sleep(15);
        stop_with_deadline();
        CloseHandle(stalled);

        const auto occupied = CreateNamedPipeW(pipe_name().c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 256, 256, 0, nullptr);
        require(occupied != INVALID_HANDLE_VALUE, "Could not prepare ownership test");
        require(!gui_transport::Start(handler), "Transport accepted a pipe already owned by another server");
        CloseHandle(occupied);
        std::cout << "PASS: fragmented 96KiB / UTF-8 / exception / restart / idle-read-write cancellation / first-instance ownership\n";
        return 0;
    } catch (const std::exception& error) {
        gui_transport::Stop();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
