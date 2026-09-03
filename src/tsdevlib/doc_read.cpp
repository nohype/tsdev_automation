#include "doc_read.h"
#include "window_watch.h"

#include <windows.h>
#include <vector>

namespace {

// Cross-process window reads block indefinitely when the target's UI thread
// wedges (a state TSDev.exe is observed to enter; see docs/KNOWLEDGE_BASE.md).
// SendMessageTimeoutW with SMTO_ABORTIFHUNG bounds each call so the caller's
// operation deadline stays enforceable; a hung call reads as "not readable
// now" and the caller's poll loop handles it.
constexpr UINT kHungTimeoutMs = 2000;

bool read_text_timeout(HWND hwnd, std::wstring& text) {
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, kHungTimeoutMs, &length))
        return false;
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, static_cast<WPARAM>(length + 1),
                             reinterpret_cast<LPARAM>(buffer.data()), SMTO_ABORTIFHUNG, kHungTimeoutMs, &copied))
        return false;
    text.assign(buffer, 0, static_cast<size_t>(copied));
    return true;
}

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
        std::wstring title;
        if (!read_text_timeout(hwnd, title)) return TRUE;
        collector->out->push_back({hwnd, title});
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
    return read_text_timeout(finder.edit, text);
}

bool is_phantom_error_text(const std::wstring& text) {
    return text.find(L"Error loading document") != std::wstring::npos &&
           text.find(L"no longer exists in your EasyLanguage workarea") != std::wstring::npos;
}
