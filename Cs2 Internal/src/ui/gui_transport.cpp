#include "gui_transport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace gui_transport {
namespace {
constexpr std::size_t maximum_message_bytes = 8u * 1024u * 1024u;
constexpr DWORD io_timeout_ms = 3000;

struct handle_closer {
    void operator()(void* value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
using unique_handle = std::unique_ptr<void, handle_closer>;
struct local_closer {
    void operator()(void* value) const noexcept { if (value) LocalFree(value); }
};
using unique_local = std::unique_ptr<void, local_closer>;

std::mutex lifecycle_mutex;
std::thread worker;
unique_handle stop_event;

unique_local user_security_descriptor() {
    HANDLE raw_token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return {};
    unique_handle token(raw_token);
    DWORD size{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size) return {};
    std::vector<BYTE> storage(size);
    if (!GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size)) return {};
    const auto user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR raw_sid{};
    if (!ConvertSidToStringSidW(user->User.Sid, &raw_sid)) return {};
    unique_local sid(raw_sid);
    // Protected DACL: only the identity running this game may connect. Local
    // remote connections are separately prohibited by PIPE_REJECT_REMOTE_CLIENTS.
    const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(raw_sid) + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return {};
    return unique_local(descriptor);
}

DWORD remaining_ms(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    if (now >= deadline) return 0;
    return static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, MAXDWORD - 1));
}

bool finish_pending(HANDLE pipe, OVERLAPPED& operation, HANDLE stop,
                    DWORD timeout, DWORD& transferred) {
    const HANDLE events[]{stop, operation.hEvent};
    const auto result = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (result == WAIT_OBJECT_0 + 1) {
        return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    }
    // The OVERLAPPED storage cannot leave scope before cancellation completes.
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

bool connect_client(HANDLE pipe, HANDLE stop, HANDLE io_event) {
    ResetEvent(io_event);
    OVERLAPPED operation{};
    operation.hEvent = io_event;
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    DWORD transferred{};
    return finish_pending(pipe, operation, stop, INFINITE, transferred);
}

bool read_bytes(HANDLE pipe, HANDLE stop, HANDLE io_event, char* data,
                DWORD capacity, ULONGLONG deadline, DWORD& transferred) {
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
    ResetEvent(io_event);
    OVERLAPPED operation{};
    operation.hEvent = io_event;
    transferred = 0;
    if (ReadFile(pipe, data, capacity, &transferred, &operation)) return transferred != 0;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    return finish_pending(pipe, operation, stop, remaining_ms(deadline), transferred) && transferred != 0;
}

bool read_request(HANDLE pipe, HANDLE stop, HANDLE io_event, std::string& request) {
    const auto deadline = GetTickCount64() + io_timeout_ms;
    std::array<char, 16384> buffer{};
    while (request.size() < maximum_message_bytes && remaining_ms(deadline)) {
        DWORD transferred{};
        const auto available = std::min<std::size_t>(buffer.size(), maximum_message_bytes - request.size());
        if (!read_bytes(pipe, stop, io_event, buffer.data(), static_cast<DWORD>(available), deadline, transferred)) return false;
        const auto newline = std::find(buffer.begin(), buffer.begin() + transferred, '\n');
        if (newline != buffer.begin() + transferred) {
            request.append(buffer.data(), static_cast<std::size_t>(newline - buffer.begin()));
            if (!request.empty() && request.back() == '\r') request.pop_back();
            return !request.empty();
        }
        request.append(buffer.data(), transferred);
    }
    return false;
}

bool write_response(HANDLE pipe, HANDLE stop, HANDLE io_event, const std::string& response) {
    const auto deadline = GetTickCount64() + io_timeout_ms;
    std::size_t offset{};
    while (offset < response.size() && remaining_ms(deadline)) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
        ResetEvent(io_event);
        OVERLAPPED operation{};
        operation.hEvent = io_event;
        DWORD transferred{};
        const auto count = static_cast<DWORD>(std::min<std::size_t>(65536, response.size() - offset));
        if (!WriteFile(pipe, response.data() + offset, count, &transferred, &operation)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                !finish_pending(pipe, operation, stop, remaining_ms(deadline), transferred)) return false;
        }
        if (!transferred) return false;
        offset += transferred;
    }
    return offset == response.size();
}

void serve(unique_handle pipe, HANDLE stop, Handler handler) {
    unique_handle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!io_event) return;
    while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
        if (!connect_client(pipe.get(), stop, io_event.get())) break;
        try {
            std::string request;
            if (read_request(pipe.get(), stop, io_event.get(), request)) {
                std::string response;
                try { response = handler(request); }
                catch (...) { response = R"({"ok":false,"error":"The requested change could not be applied."})"; }
                if (response.empty() || response.size() >= maximum_message_bytes ||
                    response.find('\n') != std::string::npos) {
                    response = R"({"ok":false,"error":"The response exceeds the bridge format or size limit."})";
                }
                response.push_back('\n');
                if (write_response(pipe.get(), stop, io_event.get(), response)) {
                    // DisconnectNamedPipe discards unread buffers. The client closes
                    // after reading its JSON line; wait briefly for that close instead
                    // of using an unbounded synchronous FlushFileBuffers call.
                    char unused{};
                    DWORD transferred{};
                    read_bytes(pipe.get(), stop, io_event.get(), &unused, 1,
                               GetTickCount64() + 500, transferred);
                }
            }
        }
        catch (...) {
            // An allocation failure or malformed request must not unwind into the game.
        }
        DisconnectNamedPipe(pipe.get());
    }
    CancelIoEx(pipe.get(), nullptr);
    DisconnectNamedPipe(pipe.get());
}
}

bool Start(Handler handler) {
    std::lock_guard lock(lifecycle_mutex);
    if (worker.joinable()) return true;
    if (!handler) return false;
    try {
        auto descriptor = user_security_descriptor();
        if (!descriptor) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
        const auto name = L"\\\\.\\pipe\\velora_gui_v1_" + std::to_wstring(GetCurrentProcessId());
        unique_handle pipe(CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, io_timeout_ms, &attributes));
        if (pipe.get() == INVALID_HANDLE_VALUE) return false;
        unique_handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop) return false;
        const auto stop_handle = stop.get();
        worker = std::thread([pipe = std::move(pipe), stop_handle, handler = std::move(handler)]() mutable {
            serve(std::move(pipe), stop_handle, std::move(handler));
        });
        stop_event = std::move(stop);
        return true;
    }
    catch (...) { return false; }
}

void Stop() {
    std::lock_guard lock(lifecycle_mutex);
    if (stop_event) SetEvent(stop_event.get());
    if (worker.joinable()) worker.join();
    stop_event.reset();
}
}
