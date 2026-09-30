// ts_install.h
//
// TradeStation install-root resolution, extracted verbatim from the former
// anonymous-namespace block in src/tsdev_lib.cpp (the "~122-1444" block):
// precedence is (1) explicit --ts-path override, (2) registry
// Installing\RootDirectory, (3) registry Versions\9.05.00\RootDirectory,
// (4) conventional default from %ProgramFiles(x86)%. Validation requires
// <root>\Program\orlib20.dll to exist.
//
// Pure Win32 (kernel32 + advapi32 registry reads) — NO orlib20 / warehouse /
// filesystem-mutation dependency. resolveTsInstallRoot() never throws and
// always returns a candidate path; validateTsInstall() is the caller's gate.

#ifndef TSDEV_TS_INSTALL_H
#define TSDEV_TS_INSTALL_H

#include <windows.h>

#include <string>

namespace tsdev {

// Strip a single trailing backslash (if present) from a wide path.
// Does NOT strip the backslash from a drive root like "C:\".
std::wstring normalizeTrailingBackslash(std::wstring path);

// Read a REG_SZ value from the registry (KEY_WOW64_32KEY view). Returns the
// value, or an empty string on failure.
std::wstring readRegistryRootDir(HKEY hive, const wchar_t* subkey,
                                 const wchar_t* valueName);

// Conventional default install root: %ProgramFiles(x86)% + "\TradeStation 9.5".
std::wstring getConventionalDefaultRoot();

// True when <root>\Program\orlib20.dll exists.
bool validateTsInstall(const std::wstring& root);

// Resolve the TS install root. Priority: override → registry → default.
// Logs the mechanism used at Info level.
std::wstring resolveTsInstallRoot(const wchar_t* overridePath);

// Convenience: resolved root + "\Program".
std::wstring resolveTsProgramDir(const wchar_t* overridePath);

// Convenience: resolved root + "\Program\Cache".
std::wstring resolveTsCacheDir(const wchar_t* overridePath);

} // namespace tsdev

#endif // TSDEV_TS_INSTALL_H
