#pragma once
#include <string>

// Locates the TSDev.exe path: explicit root override, else registry hint,
// else the default installation root. Returns an empty string when not found.
std::wstring locate_tsdev_exe(const std::wstring& ts_root_override);

bool find_running_tsdev(unsigned long& pid);
bool process_alive(unsigned long pid);

// Returns a process handle (caller closes) or nullptr on failure.
// When hide is true, the STARTUPINFO requests a hidden initial window.
void* launch_tsdev(const std::wstring& exe_path, bool hide, unsigned long& pid);
