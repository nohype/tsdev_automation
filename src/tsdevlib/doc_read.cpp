#include "doc_read.h"
#include "window_watch.h"

#include <windows.h>
#include <vector>

namespace {

bool class_starts_with(HWND hwnd, const wchar_t* prefix) {
    wchar_t cls[64]{};
    return GetClassNameW(hwnd, cls, 64) > 0 && std::wstring(cls).rfind(prefix, 0) == 0;
}

struct DocCollector {
    HWND client = nullptr;
    std::vector<DocWindow>* out = nullptr;
};

BOOL CALLBACK collect_doc(HWND hwnd, LPARAM param) {
    auto* collector = reinterpret_cast<DocCollector*>(param);
    if (GetParent(hwnd) == collector->client && class_starts_with(hwnd, L"Afx:")) {
        wchar_t title[1024]{};
        const int copied = GetWindowTextW(hwnd, title, 1024);
        collector->out->push_back({hwnd, std::wstring(title, static_cast<size_t>(copied))});
    }
    return TRUE;
}

struct EditFinder {
    HWND edit = nullptr;
};

BOOL CALLBACK find_edit(HWND hwnd, LPARAM param) {
    auto* finder = reinterpret_cast<EditFinder*>(param);
    if (class_starts_with(hwnd, L"BCGPEditCtrl")) {
        finder->edit = hwnd;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

std::vector<DocWindow> list_doc_windows(unsigned long pid) {
    std::vector<DocWindow> docs;
    const HWND frame = static_cast<HWND>(find_main_frame(pid));
    if (!frame) return docs;
    const HWND client = FindWindowExW(frame, nullptr, L"MDIClient", nullptr);
    if (!client) return docs;
    DocCollector collector{client, &docs};
    EnumChildWindows(client, collect_doc, reinterpret_cast<LPARAM>(&collector));
    return docs;
}

bool read_doc_text(void* doc, std::wstring& text) {
    if (!doc) return false;
    EditFinder finder;
    EnumChildWindows(static_cast<HWND>(doc), find_edit, reinterpret_cast<LPARAM>(&finder));
    if (!finder.edit) return false;
    const int length = static_cast<int>(SendMessageW(finder.edit, WM_GETTEXTLENGTH, 0, 0));
    if (length < 0) return false;
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    const int copied = static_cast<int>(SendMessageW(finder.edit, WM_GETTEXT, length + 1,
                                                     reinterpret_cast<LPARAM>(buffer.data())));
    if (copied < 0) return false;
    text.assign(buffer, 0, static_cast<size_t>(copied));
    return true;
}

bool is_phantom_error_text(const std::wstring& text) {
    return text.find(L"Error loading document") != std::wstring::npos &&
           text.find(L"no longer exists in your EasyLanguage workarea") != std::wstring::npos;
}
