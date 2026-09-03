#include "error_list.h"
#include "window_watch.h"

#include <windows.h>
#include <cstdint>
#include <vector>

namespace {

// Standard list-view message numbers (stable Win32 values; defined here to
// avoid pulling in commctrl.h for three constants).
constexpr unsigned int kLvmGetItemCount = 0x1004;    // LVM_GETITEMCOUNT
constexpr unsigned int kLvmGetItemTextW = 0x1073;    // LVM_GETITEMTEXTW
constexpr unsigned int kLvifText = 0x1;              // LVIF_TEXT

// Cross-process sends are bounded like every other window read (see
// doc_read.cpp): a wedged TSDev UI thread must not hang the caller.
constexpr UINT kHungTimeoutMs = 2000;

// A pathological item count must not drive the loop.
constexpr int kMaxItems = 10000;

// The target process is 32-bit: a packed 32-bit LVITEMW mirrors its layout.
#pragma pack(push, 4)
struct LvItemW32 {
    unsigned int mask;
    int iItem;
    int iSubItem;
    unsigned int state;
    unsigned int stateMask;
    unsigned int pszText;   // 32-bit pointer
    int cchTextMax;
    int iImage;
    int iIndent;
    int groupId;
    unsigned int cColumns;
    unsigned int puColumns;
    unsigned int piColFmt;
    int iGroup;
};
#pragma pack(pop)

struct ListCollector {
    std::vector<HWND>* lists;
};

BOOL CALLBACK collect_list(HWND hwnd, LPARAM param) {
    auto* collector = reinterpret_cast<ListCollector*>(param);
    wchar_t cls[32]{};
    if (GetClassNameW(hwnd, cls, 32) > 0 && std::wstring(cls) == L"SysListView32")
        collector->lists->push_back(hwnd);
    return TRUE;
}

// Reads every item's column-0 text from one list view. Returns false when the
// list is inaccessible (stale handle, allocation or memory failure) — the
// caller skips it silently.
bool read_list_texts(HWND lv, unsigned long pid, std::vector<std::wstring>& texts) {
    DWORD_PTR count_res = 0;
    if (!SendMessageTimeoutW(lv, kLvmGetItemCount, 0, 0, SMTO_ABORTIFHUNG, kHungTimeoutMs, &count_res))
        return false;
    const int count = static_cast<int>(count_res);
    if (count <= 0) return true;
    HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!proc) return false;
    const SIZE_T alloc_size = sizeof(LvItemW32) + 1024 * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(proc);
        return false;
    }
    for (int i = 0; i < count && i < kMaxItems; ++i) {
        LvItemW32 item{};
        item.mask = kLvifText;
        item.iItem = i;
        item.iSubItem = 0;
        // The remote buffer lives in the 32-bit target's address space (below
        // 4 GB even when this tool runs 64-bit), so the truncation to the
        // struct's 32-bit pointer field is exact.
        item.pszText = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(remote) + sizeof(LvItemW32));
        item.cchTextMax = 512;
        SIZE_T written = 0;
        if (!WriteProcessMemory(proc, remote, &item, sizeof(item), &written)) break;
        DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(lv, kLvmGetItemTextW, static_cast<WPARAM>(i),
                                 reinterpret_cast<LPARAM>(remote), SMTO_ABORTIFHUNG, kHungTimeoutMs, &copied))
            break;
        wchar_t buf[513]{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(proc, static_cast<char*>(remote) + sizeof(LvItemW32),
                               buf, 512 * sizeof(wchar_t), &read))
            break;
        texts.push_back(std::wstring(buf));
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return true;
}

// Parses the number immediately before position `pos` (skip spaces, then
// digits backwards), e.g. " 1 error(s), 0 warning(s) " -> 1.
bool parse_count_before(const std::wstring& text, size_t pos, int& out) {
    size_t end = pos;
    while (end > 0 && text[end - 1] == L' ') --end;
    size_t begin = end;
    while (begin > 0 && text[begin - 1] >= L'0' && text[begin - 1] <= L'9') --begin;
    if (begin == end) return false;
    long value = 0;
    for (size_t i = begin; i < end; ++i) value = value * 10 + (text[i] - L'0');
    out = static_cast<int>(value);
    return true;
}

}  // namespace

bool read_compile_error_summary(unsigned long pid, int& errors, std::wstring& error_rows) {
    errors = 0;
    error_rows.clear();
    const HWND frame = static_cast<HWND>(find_main_frame(pid));
    if (!frame) return false;
    std::vector<HWND> lists;
    ListCollector collector{&lists};
    EnumChildWindows(frame, collect_list, reinterpret_cast<LPARAM>(&collector));
    for (const HWND lv : lists) {
        std::vector<std::wstring> texts;
        if (!read_list_texts(lv, pid, texts)) continue;  // stale/inaccessible list: not an error
        int parsed = 0;
        bool have_summary = false;
        std::wstring rows;
        for (const std::wstring& text : texts) {
            const size_t pos = text.find(L"error(s)");
            if (!have_summary && pos != std::wstring::npos && parse_count_before(text, pos, parsed)) {
                have_summary = true;  // the FIRST row with the pattern is the summary
                continue;
            }
            if (!rows.empty()) rows += L" | ";
            rows += text;
        }
        if (have_summary) {
            errors = parsed;
            error_rows = rows;
            return true;
        }
    }
    return false;
}
