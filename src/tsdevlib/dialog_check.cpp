#include "dialog_check.h"
#include "tsdev_process.h"

#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

namespace {
constexpr wchar_t kDialogClass[] = L"#32770";
constexpr wchar_t kCrashReporter[] = L"TSCrashReport.exe";

struct Collect {
    unsigned long pid;
    std::set<HWND> dialogs;
};

BOOL CALLBACK collect_dialogs(HWND hwnd, LPARAM param) {
    auto* collect = reinterpret_cast<Collect*>(param);
    wchar_t cls[16]{};
    if (GetClassNameW(hwnd, cls, 16) == 0 || cls != std::wstring(kDialogClass)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == collect->pid) collect->dialogs.insert(hwnd);
    return TRUE;
}

struct CollectWindows {
    unsigned long pid;
    std::set<HWND> windows;
};

// Same enumeration as collect_dialogs but without the class filter: any
// visible top-level window of the pid.
BOOL CALLBACK collect_windows(HWND hwnd, LPARAM param) {
    auto* collect = reinterpret_cast<CollectWindows*>(param);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == collect->pid) collect->windows.insert(hwnd);
    return TRUE;
}

struct Match {
    unsigned long pid;
    const std::wstring* class_substr;  // lowercase needle
    const std::set<HWND>* before;
    HWND window;
};

BOOL CALLBACK find_new_window(HWND hwnd, LPARAM param) {
    auto* match = reinterpret_cast<Match*>(param);
    if (match->before->count(hwnd)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != match->pid) return TRUE;
    wchar_t cls[256]{};
    if (GetClassNameW(hwnd, cls, 256) == 0) return TRUE;
    std::wstring cls_ci = cls;
    std::transform(cls_ci.begin(), cls_ci.end(), cls_ci.begin(), towlower);
    if (cls_ci.find(*match->class_substr) == std::wstring::npos) return TRUE;
    match->window = hwnd;
    return FALSE;
}

std::wstring window_text(HWND hwnd) {
    wchar_t text[1024]{};
    const int len = GetWindowTextW(hwnd, text, 1024);
    return len > 0 ? std::wstring(text) : std::wstring();
}
}

std::set<HWND> snapshot_tsdev_dialogs(unsigned long pid) {
    Collect collect{pid, {}};
    EnumWindows(collect_dialogs, reinterpret_cast<LPARAM>(&collect));
    return collect.dialogs;
}

bool crash_report_running() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kCrashReporter) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::wstring check_new_dialogs(unsigned long pid, const std::set<HWND>& before) {
    const std::set<HWND> now = snapshot_tsdev_dialogs(pid);
    std::wstring text;
    for (HWND hwnd : now) {
        if (before.count(hwnd)) continue;
        if (!text.empty()) text += L" | ";
        text += window_text(hwnd);
        EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
            wchar_t cls[32]{};
            if (GetClassNameW(child, cls, 32) == 0) return TRUE;
            const std::wstring child_class = cls;
            if (child_class != L"Static" && child_class != L"Edit" && child_class != L"SysLink") return TRUE;
            const std::wstring line = window_text(child);
            if (!line.empty()) {
                auto* out = reinterpret_cast<std::wstring*>(param);
                *out += L" | " + line;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&text));
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return text;
}

std::set<HWND> snapshot_tsdev_windows(unsigned long pid) {
    CollectWindows collect{pid, {}};
    EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&collect));
    return collect.windows;
}

HWND wait_for_tsdev_dialog(unsigned long pid, const std::wstring& class_substr,
                           const std::set<HWND>& before, bool timeouts_disabled,
                           unsigned long timeout_ms) {
    const auto deadline = timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    // Case-insensitive class-substring match (the MFC class name carries
    // runtime prefixes; only the resource symbol substring is stable).
    std::wstring needle = class_substr;
    std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
    for (;;) {
        if (!process_alive(pid)) return nullptr;
        Match match{pid, &needle, &before, nullptr};
        EnumWindows(find_new_window, reinterpret_cast<LPARAM>(&match));
        if (match.window) return match.window;
        if (std::chrono::steady_clock::now() >= deadline) return nullptr;
        Sleep(100);
    }
}
