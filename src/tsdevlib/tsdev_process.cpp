#include "tsdev_process.h"

#include <windows.h>
#include <tlhelp32.h>

namespace {
constexpr wchar_t kDefaultRoot[] = L"C:\\Program Files (x86)\\TradeStation 9.5";
constexpr wchar_t kExeName[] = L"TSDev.exe";
}

std::wstring locate_tsdev_exe(const std::wstring& ts_root_override) {
    std::wstring root = ts_root_override;
    if (root.empty()) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\TradeStation", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t value[MAX_PATH]{};
            DWORD size = sizeof(value);
            DWORD type = 0;
            if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS &&
                type == REG_SZ) {
                root = value;
            }
            RegCloseKey(key);
        }
    }
    if (root.empty()) root = kDefaultRoot;
    std::wstring exe = root + L"\\Program\\" + kExeName;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
    return exe;
}

bool find_running_tsdev(unsigned long& pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kExeName) == 0) {
                pid = entry.th32ProcessID;
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool process_alive(unsigned long pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

void* launch_tsdev(const std::wstring& exe_path, bool hide, unsigned long& pid) {
    const size_t slash = exe_path.find_last_of(L'\\');
    const std::wstring workdir = slash == std::wstring::npos ? L"." : exe_path.substr(0, slash);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (hide) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe_path + L"\"";
    if (!CreateProcessW(exe_path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workdir.c_str(), &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    pid = pi.dwProcessId;
    return pi.hProcess;
}
