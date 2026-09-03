#include "pipe_client.h"

#include <windows.h>
#include <chrono>

namespace {
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TSDevPipe";
}

bool pipe_send(const std::wstring& payload, bool timeouts_disabled, unsigned long timeout_ms, std::wstring& error) {
    const std::wstring bytes = payload + L"\0";
    const DWORD size = static_cast<DWORD>(bytes.size() * sizeof(wchar_t));
    const auto deadline = timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const bool ok = WriteFile(pipe, bytes.c_str(), size, &written, nullptr) && written == size;
            const DWORD write_error = GetLastError();
            CloseHandle(pipe);
            if (ok) return true;
            error = L"pipe write failed (error " + std::to_wstring(write_error) + L")";
            return false;
        }
        const DWORD open_error = GetLastError();
        if (open_error != ERROR_PIPE_BUSY && open_error != ERROR_FILE_NOT_FOUND) {
            error = L"pipe connect failed (error " + std::to_wstring(open_error) + L")";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = L"pipe connect timed out";
            return false;
        }
        // WaitNamedPipeW returns immediately when the server has not created
        // the pipe yet, so a plain sleep avoids a tight retry loop.
        Sleep(100);
    }
}
