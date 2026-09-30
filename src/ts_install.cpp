// ts_install.cpp
//
// TradeStation install-root resolution, extracted VERBATIM from the former
// anonymous-namespace block in src/tsdev_lib.cpp (TVS-B.7). Semantics and
// precedence are unchanged: --ts-path override → registry (Installing →
// Versions\9.05.00) → conventional default; validation requires
// <root>\Program\orlib20.dll.
//
// Pure Win32 (kernel32 + advapi32) — no orlib20, no warehouse.

#include "ts_install.h"

#include "tsdev_lib.h"  // tsdev_log_message / TsDevLogLevel

namespace tsdev {

// Strip a single trailing backslash (if present) from a wide path.
// Does NOT strip the backslash from a drive root like "C:\" (checks that the
// char before the trailing '\' is not ':'). This prevents double-backslashes
// in derived paths (the registry value has a trailing '\').
std::wstring normalizeTrailingBackslash(std::wstring path) {
    if (path.size() > 1 && path.back() == L'\\' &&
        path[path.size() - 2] != L':') {
        path.pop_back();
    }
    return path;
}

// Read a REG_SZ value from the registry. Returns the value as a std::wstring,
// or an empty string on failure. Uses KEY_WOW64_32KEY to access the 32-bit
// registry view (the subkey path must NOT include WOW6432Node — the flag
// handles the redirection; including both causes double-redirection).
std::wstring readRegistryRootDir(HKEY hive, const wchar_t* subkey,
                                 const wchar_t* valueName) {
    HKEY hKey = nullptr;
    LONG rc = RegOpenKeyExW(hive, subkey, 0,
                            KEY_READ | KEY_WOW64_32KEY, &hKey);
    if (rc != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buf[MAX_PATH] = {0};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    rc = RegQueryValueExW(hKey, valueName, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf), &bufSize);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        return std::wstring();
    }
    // bufSize is in bytes; compute the string length (excluding null terminator).
    DWORD charCount = bufSize / sizeof(wchar_t);
    if (charCount > 0 && buf[charCount - 1] == L'\0') {
        charCount--;  // exclude the null terminator
    }
    return std::wstring(buf, charCount);
}

// Construct the conventional default TS install root from the
// %ProgramFiles(x86)% environment variable + "\TradeStation 9.5". If the
// env var is not set (very unlikely on modern Windows), falls back to
// "C:\Program Files (x86)" (a Windows convention, not a TS-specific
// hardcoding — contains no "TradeStation 9.5" string).
std::wstring getConventionalDefaultRoot() {
    wchar_t pfBuf[MAX_PATH] = {0};
    DWORD pfLen = GetEnvironmentVariableW(L"ProgramFiles(x86)", pfBuf, MAX_PATH);
    std::wstring pfBase;
    if (pfLen == 0 || pfLen >= MAX_PATH) {
        pfBase = L"C:\\Program Files (x86)";
    } else {
        pfBase = std::wstring(pfBuf, pfLen);
    }
    // Strip a trailing backslash from the env var value if present.
    if (pfBase.size() > 1 && pfBase.back() == L'\\' &&
        pfBase[pfBase.size() - 2] != L':') {
        pfBase.pop_back();
    }
    return pfBase + L"\\TradeStation 9.5";
}

// Validates that <root>\Program\orlib20.dll exists. Returns true if found.
bool validateTsInstall(const std::wstring& root) {
    std::wstring orlibPath = root + L"\\Program\\orlib20.dll";
    DWORD attr = GetFileAttributesW(orlibPath.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
}

// Resolves the TS install root path. Priority: override → registry → default.
// Reports which mechanism was used via an Info-level log message (visible at
// -v and above; suppressed at the default Warning level).
std::wstring resolveTsInstallRoot(const wchar_t* overridePath) {
    // (1) Explicit override
    if (overridePath != nullptr && overridePath[0] != L'\0') {
        std::wstring root = normalizeTrailingBackslash(std::wstring(overridePath));
        tsdev_log_message(TsDevLogLevel::Info,
                     "tsdev: TS install path resolved via --ts-path override: %ls\n",
                     root.c_str());
        return root;
    }

    // (2) Primary registry key: ...\TradeStation\Installing\RootDirectory
    std::wstring root = readRegistryRootDir(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\TradeStation Technologies\\TradeStation\\Installing",
        L"RootDirectory");
    if (!root.empty()) {
        root = normalizeTrailingBackslash(root);
        tsdev_log_message(TsDevLogLevel::Info,
                     "tsdev: TS install path resolved via registry (Installing): %ls\n",
                     root.c_str());
        return root;
    }

    // (3) Fallback registry key: ...\TradeStation\Versions\9.05.00\RootDirectory
    root = readRegistryRootDir(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\TradeStation Technologies\\TradeStation\\Versions\\9.05.00",
        L"RootDirectory");
    if (!root.empty()) {
        root = normalizeTrailingBackslash(root);
        tsdev_log_message(TsDevLogLevel::Info,
                     "tsdev: TS install path resolved via registry (Versions\\9.05.00): %ls\n",
                     root.c_str());
        return root;
    }

    // (4) Conventional default (last resort) — constructed from %ProgramFiles(x86)%
    root = getConventionalDefaultRoot();
    tsdev_log_message(TsDevLogLevel::Info,
                 "tsdev: TS install path resolved via conventional default: %ls\n",
                 root.c_str());
    return root;
}

std::wstring resolveTsProgramDir(const wchar_t* overridePath) {
    return resolveTsInstallRoot(overridePath) + L"\\Program";
}

std::wstring resolveTsCacheDir(const wchar_t* overridePath) {
    return resolveTsProgramDir(overridePath) + L"\\Cache";
}

} // namespace tsdev
