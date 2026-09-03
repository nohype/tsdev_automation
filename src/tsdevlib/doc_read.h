#pragma once
#include <string>
#include <vector>

struct DocWindow {
    void* window;
    std::wstring title;
};

// Enumerates TSDev's MDI document windows (direct children of the MDI client,
// class prefix "Afx:") for the given pid.
std::vector<DocWindow> list_doc_windows(unsigned long pid);

// Reads the text of the document's editor control (class prefix BCGPEditCtrl).
// The WM_GETTEXT buffer is sized from WM_GETTEXTLENGTH first; a fixed buffer
// would silently truncate large sources.
bool read_doc_text(void* doc, std::wstring& text);

// Sets the text of the document's editor control (class prefix BCGPEditCtrl)
// via WM_SETTEXT, using SendMessageTimeoutW with SMTO_ABORTIFHUNG (same
// bounded-send pattern as read_doc_text). Returns false when the editor
// control is not found or the send times out / is aborted.
bool set_doc_text(void* doc, const std::wstring& text);

// True when the text is TSDev's own "document no longer exists" error page —
// the marker TSDev renders into the editor when an opened item is not in the
// library (the phantom document created by open-or-create).
bool is_phantom_error_text(const std::wstring& text);
