#include "inject.h"

#include <windows.h>
#include <fstream>
#include <cstring>
#include <vector>

bool inject_dll(unsigned long pid, const std::wstring& dll_path,
                unsigned long& remote_base, std::wstring& error) {
    remote_base = 0;
    const HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!proc) {
        error = L"OpenProcess failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    const SIZE_T path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(proc, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        error = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L")";
        CloseHandle(proc);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote_path, dll_path.c_str(), path_bytes, &written) || written != path_bytes) {
        error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC load_library = GetProcAddress(kernel32, "LoadLibraryW");
    if (!load_library) {
        error = L"GetProcAddress(LoadLibraryW) failed";
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    const HANDLE thread = CreateRemoteThread(
        proc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
        remote_path, 0, nullptr);
    if (!thread) {
        error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // Wait for the remote thread to complete — DllMain finishes before
    // LoadLibraryW returns, so the hook is installed when this returns.
    WaitForSingleObject(thread, 30000);

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    remote_base = static_cast<unsigned long>(exit_code);

    CloseHandle(thread);
    VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exit_code == 0) {
        error = L"LoadLibraryW returned 0 in the remote process (DLL load failed)";
        return false;
    }
    return true;
}

bool call_remote_function(unsigned long pid, void* remote_fn, std::wstring& error) {
    const HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!proc) {
        error = L"OpenProcess failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    const HANDLE thread = CreateRemoteThread(
        proc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_fn),
        nullptr, 0, nullptr);
    if (!thread) {
        error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 30000);

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);

    CloseHandle(thread);
    CloseHandle(proc);

    if (exit_code == 0) {
        error = L"remote function returned 0 (ReArmHook failed)";
        return false;
    }
    return true;
}

namespace {

struct Section {
    unsigned long virtual_address;
    unsigned long virtual_size;
    unsigned long pointer_to_raw_data;
};

// Maps an RVA to a file offset using the section table.
unsigned long rva_to_file_offset(unsigned long rva, const std::vector<Section>& sections) {
    for (const Section& s : sections) {
        if (rva >= s.virtual_address && rva < s.virtual_address + s.virtual_size) {
            return rva - s.virtual_address + s.pointer_to_raw_data;
        }
    }
    return 0;
}

} // namespace

unsigned long get_export_rva(const std::wstring& dll_path, const char* export_name) {
    std::ifstream f(dll_path, std::ios::binary);
    if (!f) return 0;

    // DOS header: e_lfanew at offset 0x3C points to the PE signature.
    unsigned char dos[64];
    f.read(reinterpret_cast<char*>(dos), 64);
    if (!f) return 0;
    const unsigned long e_lfanew = *reinterpret_cast<unsigned long*>(dos + 0x3C);

    // PE signature (4 bytes) + COFF header (20 bytes).
    unsigned char pe_sig[4];
    f.seekg(e_lfanew);
    f.read(reinterpret_cast<char*>(pe_sig), 4);
    if (!f || std::memcmp(pe_sig, "PE\0\0", 4) != 0) return 0;

    unsigned char coff[20];
    f.read(reinterpret_cast<char*>(coff), 20);
    if (!f) return 0;
    const unsigned short num_sections = *reinterpret_cast<unsigned short*>(coff + 2);
    const unsigned short size_opt_header = *reinterpret_cast<unsigned short*>(coff + 16);

    // Optional header. Only PE32 (magic 0x10b) is supported; the DLL is always Win32.
    std::vector<unsigned char> opt(size_opt_header);
    f.read(reinterpret_cast<char*>(opt.data()), size_opt_header);
    if (!f || opt.size() < 96) return 0;
    const unsigned short opt_magic = *reinterpret_cast<unsigned short*>(opt.data());
    if (opt_magic != 0x10b) return 0;
    // Export directory is the first data directory, at offset 96 in PE32.
    const unsigned long export_rva = *reinterpret_cast<unsigned long*>(opt.data() + 96);
    const unsigned long export_size = *reinterpret_cast<unsigned long*>(opt.data() + 100);
    if (export_rva == 0 || export_size == 0) return 0;

    // Section headers (40 bytes each), immediately after the optional header.
    std::vector<Section> sections(num_sections);
    for (int i = 0; i < num_sections; ++i) {
        unsigned char sh[40];
        f.read(reinterpret_cast<char*>(sh), 40);
        if (!f) return 0;
        sections[i].virtual_address = *reinterpret_cast<unsigned long*>(sh + 12);
        sections[i].virtual_size = *reinterpret_cast<unsigned long*>(sh + 8);
        sections[i].pointer_to_raw_data = *reinterpret_cast<unsigned long*>(sh + 20);
    }

    // Export Directory: NumberOfNames (+0x18), AddressOfFunctions (+0x1C),
    // AddressOfNames (+0x20), AddressOfNameOrdinals (+0x24).
    const unsigned long exp_off = rva_to_file_offset(export_rva, sections);
    if (!exp_off) return 0;
    unsigned char edir[40];
    f.seekg(exp_off);
    f.read(reinterpret_cast<char*>(edir), 40);
    if (!f) return 0;
    const unsigned long num_names = *reinterpret_cast<unsigned long*>(edir + 0x18);
    const unsigned long addr_functions = *reinterpret_cast<unsigned long*>(edir + 0x1C);
    const unsigned long addr_names = *reinterpret_cast<unsigned long*>(edir + 0x20);
    const unsigned long addr_ordinals = *reinterpret_cast<unsigned long*>(edir + 0x24);
    if (num_names == 0) return 0;

    const unsigned long names_off = rva_to_file_offset(addr_names, sections);
    const unsigned long ordinals_off = rva_to_file_offset(addr_ordinals, sections);
    const unsigned long functions_off = rva_to_file_offset(addr_functions, sections);
    if (!names_off || !ordinals_off || !functions_off) return 0;

    // Iterate the name array; on match, use the ordinal to index the function array.
    for (unsigned long i = 0; i < num_names; ++i) {
        unsigned long name_rva = 0;
        f.seekg(names_off + i * 4);
        f.read(reinterpret_cast<char*>(&name_rva), 4);
        if (!f) return 0;
        const unsigned long name_off = rva_to_file_offset(name_rva, sections);
        if (!name_off) return 0;
        f.seekg(name_off);
        std::string nm;
        char c = 0;
        while (f.get(c) && c != '\0') nm.push_back(c);
        if (!f) return 0;
        if (nm == export_name) {
            unsigned short ordinal = 0;
            f.seekg(ordinals_off + i * 2);
            f.read(reinterpret_cast<char*>(&ordinal), 2);
            if (!f) return 0;
            unsigned long func_rva = 0;
            f.seekg(functions_off + ordinal * 4);
            f.read(reinterpret_cast<char*>(&func_rva), 4);
            if (!f) return 0;
            return func_rva;
        }
    }
    return 0;
}
