#include "window_watch.h"
#include "tsdev_process.h"

#include <windows.h>
#include <chrono>

namespace {
constexpr wchar_t kFrameClass[] = L"TSDEV.EXE TRADESTATION";

struct Match {
    unsigned long pid;
    HWND frame;
};

BOOL CALLBACK enum_frame(HWND hwnd, LPARAM param) {
    auto* match = reinterpret_cast<Match*>(param);
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) == 0 || cls != std::wstring(kFrameClass)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == match->pid) {
        match->frame = hwnd;
        return FALSE;
    }
    return TRUE;
}
}

void* find_main_frame(unsigned long pid) {
    Match match{pid, nullptr};
    EnumWindows(enum_frame, reinterpret_cast<LPARAM>(&match));
    return match.frame;
}

bool wait_for_title(unsigned long pid, const std::wstring& needle, bool timeouts_disabled, unsigned long timeout_ms) {
    const auto deadline = timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (!process_alive(pid)) return false;
        const auto frame = static_cast<HWND>(find_main_frame(pid));
        if (frame) {
            wchar_t title[1024]{};
            if (GetWindowTextW(frame, title, 1024) > 0 &&
                std::wstring(title).find(needle) != std::wstring::npos) {
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(100);
    }
}
