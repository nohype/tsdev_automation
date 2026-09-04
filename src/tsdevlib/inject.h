#pragma once
#include <string>

// Injects `dll_path` into the process `pid` using CreateRemoteThread +
// LoadLibraryW. Returns true on success. `error` is filled on failure.
// `remote_base` receives the HMODULE returned by LoadLibraryW in the remote
// process (the thread exit code). Waits for the remote thread to complete
// (DllMain finishes) before returning.
bool inject_dll(unsigned long pid, const std::wstring& dll_path,
                unsigned long& remote_base, std::wstring& error);

// Calls `remote_fn` in the process `pid` via CreateRemoteThread and waits
// for it to complete. Returns true on success (exit code != 0). `error` is
// filled on failure.
bool call_remote_function(unsigned long pid, void* remote_fn, std::wstring& error);

// Parses the PE export table of `dll_path` and returns the RVA of the named
// export, or 0 on failure. This avoids loading the DLL locally (which may
// load at a different base than in the remote process due to ASLR rebasing).
unsigned long get_export_rva(const std::wstring& dll_path, const char* export_name);
