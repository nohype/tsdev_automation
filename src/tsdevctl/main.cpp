#include "tsdevlib/args.h"
#include "tsdevlib/busy_guard.h"
#include "tsdevlib/dialog_check.h"
#include "tsdevlib/doc_read.h"
#include "tsdevlib/elevation.h"
#include "tsdevlib/error_list.h"
#include "tsdevlib/exit_codes.h"
#include "tsdevlib/json_writer.h"
#include "tsdevlib/pipe_client.h"
#include "tsdevlib/tsdev_process.h"
#include "tsdevlib/window_watch.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iostream>
#include <optional>
#include <set>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring human_line(const OpResult& r) {
    std::wstring line = r.op + L": ";
    if (r.reports_exists && r.ok) {
        line += r.exists_value ? L"yes" : L"no";
        if (r.exists_value) line += L" (" + r.name + L" : " + r.type + L")";
    } else {
        line += r.ok ? L"ok" : L"failed";
    }
    if (r.has_pid && !(r.reports_exists && r.ok)) {
        line += L" (pid " + std::to_wstring(r.pid) + L", " + std::to_wstring(r.elapsed_ms) + L" ms)";
    }
    if (r.error_code != 0) line += L" [" + std::to_wstring(r.error_code) + L" " + r.error_name + L"] " + r.error_message;
    return line;
}

void emit(const Options& opts, const OpResult& result) {
    const std::string line = to_utf8(opts.json ? make_result_json(result) : human_line(result));
    std::cout << line << "\n";
    std::cout.flush();
}

// TSDev lifecycle state shared by all verbs (R5/R7).
struct Session {
    unsigned long pid = 0;
    void* process = nullptr;
    bool launched = false;
};

// TSDev re-shows its frame while it finishes its own startup, so the hidden
// state is (re-)enforced at every exit rather than only once.
void hide_if_launched(const Options& opts, const Session& session) {
    if (session.launched && !opts.nohide) {
        const auto frame = static_cast<HWND>(find_main_frame(session.pid));
        if (frame) ShowWindow(frame, SW_HIDE);
    }
}

// --- Quiet-UX frame management (R7: no visual flicker, no focus stealing) ---
// The frame must be WS_VISIBLE for the New dialog's name propagation and the
// verify "*" removal (verified live: WS_VISIBLE off-screen works; SW_HIDE
// breaks both). SW_SHOWNA shows without stealing focus (verified live with
// another application foreground: focus was NOT stolen). Moving the frame
// off-screen eliminates visual flicker while keeping WS_VISIBLE. The New
// dialog creation repositions the frame back on-screen (verified live), so
// the frame must be re-moved off-screen after the dialog appears.
constexpr int kOffscreenX = -32000;
constexpr int kOffscreenY = -32000;

void move_offscreen(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, kOffscreenX, kOffscreenY, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

struct FrameState {
    bool shown = false;      // we called SW_SHOWNA; need SW_HIDE at end
    bool moved = false;      // we moved the frame off-screen; need to restore
    RECT saved_rect = {};    // original frame position before we moved it
    DWORD fg_thread = 0;     // foreground window's thread (for focus restore)
    HWND fg_window = nullptr; // user's foreground window before we showed the frame
};

// Shows the frame quietly: SW_SHOWNA (no focus steal) if hidden, then moves
// off-screen (WS_VISIBLE kept — required for the New dialog's name propagation
// and the verify "*" removal, KB). Saves the original position for restoration.
// The foreground window must be saved earlier (at the start of the operation,
// before any TSDev interaction) via save_foreground().
// When nohide is true, shows on-screen with SW_SHOW (the user explicitly wants
// to watch; accepts flicker and focus steal).
void show_frame_quiet(HWND frame, bool nohide, FrameState& state) {
    if (!frame) return;
    if (!IsWindowVisible(frame)) {
        state.shown = true;
        ShowWindow(frame, nohide ? SW_SHOW : SW_SHOWNA);
    }
    if (!nohide) {
        GetWindowRect(frame, &state.saved_rect);
        move_offscreen(frame);
        state.moved = true;
    }
}

// Saves the user's foreground window at the start of the operation, before
// any TSDev interaction (launch, pipe-open, etc.) can steal focus. The focus
// is restored at exit via restore_frame_quiet.
void save_foreground(FrameState& state) {
    state.fg_window = GetForegroundWindow();
    if (state.fg_window)
        state.fg_thread = GetWindowThreadProcessId(state.fg_window, nullptr);
}

// Restores focus to the user's foreground window using AttachThreadInput,
// which allows SetForegroundWindow to work even if our process doesn't have
// the input focus (Windows' foreground management would otherwise block it).
void restore_foreground(HWND fg_window, DWORD fg_thread) {
    if (!fg_window || !fg_thread || !IsWindow(fg_window)) return;
    const DWORD our_thread = GetCurrentThreadId();
    AttachThreadInput(our_thread, fg_thread, TRUE);
    SetForegroundWindow(fg_window);
    AttachThreadInput(our_thread, fg_thread, FALSE);
}

// Restores the frame to its original state: hidden first (if we showed it),
// then position restored, then focus restored to the user's foreground window.
// Hiding before position restore eliminates a brief on-screen flash and a race
// with FrameGuard (which would move the visible frame back off-screen).
void restore_frame_quiet(HWND frame, const FrameState& state, bool nohide) {
    if (!frame || !IsWindow(frame)) return;
    if (state.shown && !nohide) {
        ShowWindow(frame, SW_HIDE);
    }
    if (state.moved) {
        SetWindowPos(frame, nullptr, state.saved_rect.left, state.saved_rect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!nohide) {
        restore_foreground(state.fg_window, state.fg_thread);
    }
}

// Frame guard: a background thread that continuously keeps the frame off-screen
// while the New dialog creation repositions it on-screen. TSDev's New dialog
// creation calls SetWindowPos on the frame to move it back on-screen (verified
// live), which can steal focus from the user's foreground application. The
// guard thread polls the frame position every 10ms and immediately moves it
// back off-screen, minimizing the on-screen time to ~10ms (one poll interval).
// This eliminates both visual flicker and focus stealing.
class FrameGuard {
    std::thread thread_;
    std::atomic<bool> running_{false};
    HWND frame_ = nullptr;
public:
    FrameGuard() = default;
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;

    void start(HWND frame) {
        if (!frame || running_.load()) return;
        frame_ = frame;
        running_.store(true);
        thread_ = std::thread([this]() {
            while (running_.load()) {
                if (IsWindow(frame_) && IsWindowVisible(frame_)) {
                    RECT r{};
                    GetWindowRect(frame_, &r);
                    if (r.left > kOffscreenX + 1000)
                        move_offscreen(frame_);
                }
                Sleep(10);
            }
        });
    }
    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }
    ~FrameGuard() { stop(); }
};

// Attaches to a running TSDev or launches a hidden one (R5/R7). Records the
// failure in `result` and returns the exit code to propagate (Exit::Ok when
// ready).
Exit ensure_tsdev_ready(const Options& opts, OpResult& result, Session& session) {
    const std::wstring exe = locate_tsdev_exe(opts.ts_path);
    if (exe.empty()) {
        result.error_code = static_cast<int>(Exit::LaunchFailed);
        result.error_name = L"tsdev_not_found";
        result.error_message = L"TSDev.exe not located (use --ts-path)";
        return Exit::LaunchFailed;
    }

    if (find_running_tsdev(session.pid)) {
        result.has_pid = true;
        result.pid = session.pid;
        if (!opts.nohide) {
            const auto frame = static_cast<HWND>(find_main_frame(session.pid));
            if (frame) ShowWindow(frame, SW_HIDE);
        }
        return Exit::Ok;
    }

    session.process = launch_tsdev(exe, !opts.nohide, session.pid);
    if (!session.process) {
        result.error_code = static_cast<int>(Exit::LaunchFailed);
        result.error_name = L"launch_failed";
        result.error_message = L"CreateProcess failed for " + exe;
        return Exit::LaunchFailed;
    }
    session.launched = true;
    result.has_pid = true;
    result.pid = session.pid;
    const auto deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    for (;;) {
        if (!process_alive(session.pid)) {
            result.error_code = static_cast<int>(Exit::Crashed);
            result.error_name = L"tsdev_died";
            result.error_message = L"TSDev exited during startup";
            return Exit::Crashed;
        }
        if (find_main_frame(session.pid)) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            hide_if_launched(opts, session);
            result.error_code = static_cast<int>(Exit::LaunchFailed);
            result.error_name = L"launch_timeout";
            result.error_message = L"TSDev main window did not appear in time";
            return Exit::LaunchFailed;
        }
        Sleep(100);
    }
    if (!opts.nohide) ShowWindow(static_cast<HWND>(find_main_frame(session.pid)), SW_HIDE);
    return Exit::Ok;
}

struct Failure {
    Exit code;
    const wchar_t* name;
    std::wstring message;
};

// Sends the pipe payload and waits for the frame title. Returns nullopt on
// success, else the failure (dialog/crash/timeout checked on every path).
// TSDev can drop the open command while its UI thread is still restoring the
// previous session's documents; the command is idempotent (re-opening an open
// document just activates it), so it is re-sent periodically while waiting.
std::optional<Failure> send_and_wait(const Options& opts, const Session& session,
                                     const std::set<HWND>& dialogs_before, const std::wstring& needle) {
    const std::wstring payload = L"\"" + opts.name + L"\", " + std::to_wstring(opts.type_number);
    std::wstring error;
    if (!pipe_send(payload, opts.timeouts_disabled, opts.timeout_ms, error))
        return Failure{Exit::PipeFailed, L"pipe_failed", error};
    const auto deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    auto last_send = std::chrono::steady_clock::now();
    for (;;) {
        if (wait_for_title(session.pid, needle, opts.timeouts_disabled, 1000)) break;
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
        if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
        if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};
        if (!process_alive(session.pid))
            return Failure{Exit::Crashed, L"tsdev_died", L"TSDev exited during the operation"};
        const auto now = std::chrono::steady_clock::now();
        if (!opts.timeouts_disabled && now >= deadline)
            return Failure{Exit::Timeout, L"timeout", L"item did not open within the timeout"};
        if (opts.timeouts_disabled || now - last_send >= std::chrono::milliseconds(3000)) {
            if (pipe_send(payload, opts.timeouts_disabled, opts.timeout_ms, error))
                last_send = now;
        }
    }
    return std::nullopt;
}

int run_open(const Options& opts) {
    OpResult result;
    result.op = L"open";
    result.type = opts.type;
    result.name = opts.name;
    Session session;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count());
    };
    auto fail = [&](Exit code, const wchar_t* name, const std::wstring& message) {
        hide_if_launched(opts, session);
        if (session.process) {
            CloseHandle(session.process);
            session.process = nullptr;
        }
        result.ok = false;
        result.error_code = static_cast<int>(code);
        result.error_name = name;
        result.error_message = message;
        result.elapsed_ms = elapsed_ms();
        emit(opts, result);
        return static_cast<int>(code);
    };

    BusyGuard busy = BusyGuard::acquire();
    if (!busy) return fail(Exit::Busy, L"busy", L"another tsdevctl invocation is running");

    if (!is_elevated()) return fail(Exit::NotElevated, L"not_elevated", L"tsdevctl must run in an elevated process (R8)");

    const Exit ready = ensure_tsdev_ready(opts, result, session);
    if (ready != Exit::Ok) return fail(ready, result.error_name.c_str(), result.error_message);

    const std::set<HWND> dialogs_before = snapshot_tsdev_dialogs(session.pid);

    const std::wstring needle = L" - " + opts.name + L" : " + opts.type_label;
    const std::optional<Failure> failure = send_and_wait(opts, session, dialogs_before, needle);
    if (failure) {
        // Unlike exists, open has no late-open re-scan: an opened document is
        // open's requested outcome, so a late execution is not residue. The
        // KB's "state unknown" guidance covers a timed-out open for callers.
        return fail(failure->code, failure->name, failure->message);
    }

    const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
    if (!dialog_text.empty()) return fail(Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text);
    if (crash_report_running()) return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected");

    hide_if_launched(opts, session);
    result.ok = true;
    result.elapsed_ms = elapsed_ms();
    emit(opts, result);
    if (session.process) CloseHandle(session.process);
    return static_cast<int>(Exit::Ok);
}

// The trailing '*' on a document title is a state-dependent UI marker, never
// part of the name (verified live; see docs/KNOWLEDGE_BASE.md).
std::wstring normalized_title(std::wstring title) {
    while (!title.empty() && title.back() == L'*') title.pop_back();
    return title;
}

struct DocMatch {
    const DocWindow* window = nullptr;
    bool opened_by_us = false;
};

// Locates the document window for the requested item: exact title match
// first (a fresh open titles the document with the requested spelling), then
// a case-insensitive pass (an already-open document keeps the spelling it
// was first opened with). Only windows absent before the pipe command are
// flagged as closable (R6: leave everything else exactly as found).
DocMatch find_doc_window(const std::vector<DocWindow>& docs_before,
                         const std::vector<DocWindow>& docs_after,
                         const std::wstring& expected) {
    const std::wstring expected_ci = lower(expected);
    DocMatch match;
    for (int pass = 0; pass < 2 && match.window == nullptr; ++pass) {
        for (const DocWindow& doc : docs_after) {
            const bool matches = pass == 0 ? normalized_title(doc.title) == expected
                                           : lower(doc.title).find(expected_ci) != std::wstring::npos;
            if (!matches) continue;
            match.window = &doc;
            match.opened_by_us = std::none_of(docs_before.begin(), docs_before.end(),
                [&](const DocWindow& before) { return before.window == doc.window; });
            break;
        }
    }
    return match;
}

// Closes a document window and waits (bounded, with dialog and crash checks)
// for it to disappear.
std::optional<Failure> close_doc_window(const Options& opts, const Session& session,
                                        const std::set<HWND>& dialogs_before, const DocWindow& doc) {
    PostMessageW(static_cast<HWND>(doc.window), WM_CLOSE, 0, 0);
    const auto deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    for (;;) {
        if (!IsWindow(static_cast<HWND>(doc.window))) return std::nullopt;
        if (!process_alive(session.pid)) return std::nullopt;
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
        if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
        if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};
        if (!opts.timeouts_disabled && std::chrono::steady_clock::now() >= deadline)
            return Failure{Exit::Timeout, L"timeout", L"opened document did not close within the timeout"};
        Sleep(50);
    }
}

// Closes a document that may be dirty: WM_CLOSE, then answers "No" to
// TSDev's "Save changes?" prompt (#32770) so the close completes without
// saving. Returns nullopt on success, else the failure.
std::optional<Failure> close_doc_discard(const Options& opts, const Session& session,
                                         const std::set<HWND>& dialogs_before, const DocWindow& doc) {
    PostMessageW(static_cast<HWND>(doc.window), WM_CLOSE, 0, 0);
    const auto deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    for (;;) {
        if (!IsWindow(static_cast<HWND>(doc.window))) return std::nullopt;
        if (!process_alive(session.pid)) return std::nullopt;
        // Answer "No" on a save-changes prompt: WM_COMMAND with IDNO (7).
        // The prompt is TSDev's own dialog; triggering its No button code path
        // is the same as the user clicking No (no synthetic input).
        const std::set<HWND> now = snapshot_tsdev_dialogs(session.pid);
        for (HWND dlg : now) {
            if (dialogs_before.count(dlg)) continue;
            PostMessageW(dlg, WM_COMMAND, IDNO, 0);
        }
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
        if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
        if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};
        if (!opts.timeouts_disabled && std::chrono::steady_clock::now() >= deadline)
            return Failure{Exit::Timeout, L"timeout", L"dirty document did not close within the timeout"};
        Sleep(50);
    }
}

// TSDev can execute a pipe command after the caller stopped waiting for it
// (verified live: a command ran after the timeout expired, leaving an open
// document behind). On a failed wait, re-scan for such a late open and close
// it so the timeout leaves no open-document residue. The scan also checks
// dialogs and crash reports: a late-executed command can raise an error
// dialog instead of (or while) opening the document, and that failure must
// be surfaced rather than left lingering in TSDev.
std::optional<Failure> close_late_open(const Options& opts, const Session& session,
                                       const std::vector<DocWindow>& docs_before,
                                       const std::set<HWND>& dialogs_before, const std::wstring& expected) {
    const auto deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    for (;;) {
        if (!process_alive(session.pid)) return std::nullopt;
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
        if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
        if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};
        const std::vector<DocWindow> docs_now = list_doc_windows(session.pid);
        const DocMatch match = find_doc_window(docs_before, docs_now, expected);
        if (match.window != nullptr && match.opened_by_us) {
            return close_doc_window(opts, session, dialogs_before, *match.window);
        }
        if (!opts.timeouts_disabled && std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        Sleep(100);
    }
}

// Outcome of the shared open-read-close sequence (exists + getsrc).
struct ReadOutcome {
    bool exists = false;
    std::wstring editor_text;
};

// Opens the item via pipe, waits for the doc, polls the editor text until
// stable (real item — its source) or phantom error page (does not exist),
// then closes the doc iff the caller opened it. On success returns nullopt
// and fills `out`: out.exists is the existence answer, out.editor_text is the
// rendered editor content (the source for a real item; the phantom error page
// for a nonexistent name — callers must not emit the latter as source). On
// failure returns the Failure; `out` is not meaningful.
std::optional<Failure> open_read_and_close(const Options& opts, const Session& session,
                                           const std::set<HWND>& dialogs_before,
                                           const std::vector<DocWindow>& docs_before,
                                           ReadOutcome& out) {
    const std::wstring expected = opts.name + L" : " + opts.type_label;
    const std::wstring needle = L" - " + expected;
    const std::optional<Failure> failure = send_and_wait(opts, session, dialogs_before, needle);
    if (failure) {
        // A late-executed open leaves a document behind; re-scan and close it.
        // A dialog or crash raised by the late execution is the more severe
        // signal and replaces the original wait failure.
        const std::optional<Failure> late_failure =
            close_late_open(opts, session, docs_before, dialogs_before, expected);
        if (late_failure) return late_failure;
        return failure;
    }

    const std::vector<DocWindow> docs_after = list_doc_windows(session.pid);
    const DocMatch match = find_doc_window(docs_before, docs_after, expected);
    if (match.window == nullptr)
        return Failure{Exit::Timeout, L"timeout", L"frame title changed but no matching document window appeared"};

    // TSDev renders the editor content asynchronously after the document
    // window appears, and a fresh phantom's editor transiently holds a single
    // space before the error page replaces it (verified live). Neither empty
    // nor non-empty is decidable on its own: poll until the phantom error
    // page appears (item does not exist) or the text is unchanged across two
    // consecutive 500 ms reads (item exists — its rendered source).
    std::wstring previous_text;
    int stable_reads = 0;
    const auto read_deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    for (;;) {
        if (!IsWindow(static_cast<HWND>(match.window->window)))
            return Failure{Exit::Crashed, L"tsdev_died", L"document window disappeared before its content rendered"};
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
        if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
        if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};
        if (!process_alive(session.pid)) return Failure{Exit::Crashed, L"tsdev_died", L"TSDev exited during the operation"};
        std::wstring text;
        if (read_doc_text(match.window->window, text) && !text.empty()) {
            if (is_phantom_error_text(text)) {
                out.exists = false;
                out.editor_text = text;
                break;
            }
            if (text == previous_text) {
                ++stable_reads;
                if (stable_reads >= 2) {
                    out.exists = true;
                    out.editor_text = text;
                    break;
                }
            } else {
                stable_reads = 0;
            }
            previous_text = text;
        } else {
            stable_reads = 0;
            previous_text.clear();
        }
        if (!opts.timeouts_disabled && std::chrono::steady_clock::now() >= read_deadline)
            return Failure{Exit::Timeout, L"timeout", L"editor content did not render within the timeout"};
        Sleep(500);
    }

    if (match.opened_by_us) {
        const std::optional<Failure> close_failure = close_doc_window(opts, session, dialogs_before, *match.window);
        if (close_failure) return close_failure;
    }

    const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
    if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
    if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};

    return std::nullopt;
}

// Writes `text` to `path` as UTF-8 with no BOM, line endings preserved as-is.
// On failure removes any partially-written file and sets `error`.
bool write_source_file(const std::wstring& path, const std::wstring& text, std::wstring& error) {
    const std::string utf8 = to_utf8(text);
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"cannot open '" + path + L"' for writing (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
                    written == static_cast<DWORD>(utf8.size());
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(path.c_str());  // remove partial file (R10: no partial artifacts)
        error = L"write failed for '" + path + L"'";
        return false;
    }
    return true;
}

// Reads `path` as UTF-8 (stripping an optional UTF-8 BOM) and converts to a
// wstring for WM_SETTEXT. Returns false and fills `error` on failure.
bool read_source_file(const std::wstring& path, std::wstring& text, std::wstring& error) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"cannot open '" + path + L"' for reading (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > static_cast<LONGLONG>(64u * 1024u * 1024u)) {
        CloseHandle(h);
        error = L"source file '" + path + L"' is too large or its size is unavailable";
        return false;
    }
    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read_total = 0;
    while (read_total < raw.size()) {
        DWORD chunk = 0;
        const DWORD to_read = static_cast<DWORD>(raw.size() - read_total);
        if (!ReadFile(h, raw.data() + read_total, to_read, &chunk, nullptr) || chunk == 0) {
            CloseHandle(h);
            error = L"read failed for '" + path + L"'";
            return false;
        }
        read_total += chunk;
    }
    CloseHandle(h);
    // Strip a UTF-8 BOM (EF BB BF) if present.
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    text = from_utf8(raw);
    return true;
}

int run_exists(const Options& opts) {
    OpResult result;
    result.op = L"exists";
    result.type = opts.type;
    result.name = opts.name;
    Session session;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count());
    };
    auto fail = [&](Exit code, const wchar_t* name, const std::wstring& message) {
        hide_if_launched(opts, session);
        if (session.process) {
            CloseHandle(session.process);
            session.process = nullptr;
        }
        result.ok = false;
        result.error_code = static_cast<int>(code);
        result.error_name = name;
        result.error_message = message;
        result.elapsed_ms = elapsed_ms();
        emit(opts, result);
        return static_cast<int>(code);
    };

    BusyGuard busy = BusyGuard::acquire();
    if (!busy) return fail(Exit::Busy, L"busy", L"another tsdevctl invocation is running");

    if (!is_elevated()) return fail(Exit::NotElevated, L"not_elevated", L"tsdevctl must run in an elevated process (R8)");

    const Exit ready = ensure_tsdev_ready(opts, result, session);
    if (ready != Exit::Ok) return fail(ready, result.error_name.c_str(), result.error_message);

    const std::set<HWND> dialogs_before = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before = list_doc_windows(session.pid);

    ReadOutcome outcome;
    const std::optional<Failure> failure = open_read_and_close(opts, session, dialogs_before, docs_before, outcome);
    if (failure) return fail(failure->code, failure->name, failure->message);

    hide_if_launched(opts, session);
    result.ok = true;
    result.reports_exists = true;
    result.exists_value = outcome.exists;
    result.elapsed_ms = elapsed_ms();
    emit(opts, result);
    if (session.process) CloseHandle(session.process);
    return static_cast<int>(outcome.exists ? Exit::Ok : Exit::NotFound);
}

int run_getsrc(const Options& opts) {
    OpResult result;
    result.op = L"getsrc";
    result.type = opts.type;
    result.name = opts.name;
    Session session;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count());
    };
    auto fail = [&](Exit code, const wchar_t* name, const std::wstring& message) {
        hide_if_launched(opts, session);
        if (session.process) {
            CloseHandle(session.process);
            session.process = nullptr;
        }
        result.ok = false;
        result.error_code = static_cast<int>(code);
        result.error_name = name;
        result.error_message = message;
        result.elapsed_ms = elapsed_ms();
        emit(opts, result);
        return static_cast<int>(code);
    };

    BusyGuard busy = BusyGuard::acquire();
    if (!busy) return fail(Exit::Busy, L"busy", L"another tsdevctl invocation is running");

    if (!is_elevated()) return fail(Exit::NotElevated, L"not_elevated", L"tsdevctl must run in an elevated process (R8)");

    const Exit ready = ensure_tsdev_ready(opts, result, session);
    if (ready != Exit::Ok) return fail(ready, result.error_name.c_str(), result.error_message);

    const std::set<HWND> dialogs_before = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before = list_doc_windows(session.pid);

    ReadOutcome outcome;
    const std::optional<Failure> failure = open_read_and_close(opts, session, dialogs_before, docs_before, outcome);
    if (failure) return fail(failure->code, failure->name, failure->message);

    hide_if_launched(opts, session);
    result.ok = true;
    result.reports_exists = true;
    result.exists_value = outcome.exists;
    result.reports_source = true;
    result.elapsed_ms = elapsed_ms();

    if (!outcome.exists) {
        // NotFound: no source, no file written.
        if (opts.json) {
            std::cout << to_utf8(make_result_json(result)) << "\n";
            std::cout.flush();
        } else {
            std::cout << "getsrc: not found\n";
            std::cout.flush();
        }
        if (session.process) CloseHandle(session.process);
        return static_cast<int>(Exit::NotFound);
    }

    // Real item: the editor text IS the source.
    result.source = outcome.editor_text;

    // Write to --output file if requested (before emitting, so a write failure
    // is reported as an error with no source on stdout).
    if (!opts.output_path.empty()) {
        std::wstring write_err;
        if (!write_source_file(opts.output_path, outcome.editor_text, write_err))
            return fail(Exit::Unexpected, L"output_write_failed", write_err);
        result.output_path = opts.output_path;
    }

    if (opts.json) {
        std::cout << to_utf8(make_result_json(result)) << "\n";
        std::cout.flush();
    } else if (opts.output_path.empty()) {
        // Raw source to stdout (UTF-8, no trailing newline added).
        std::cout << to_utf8(outcome.editor_text);
        std::cout.flush();
    } else {
        // Human summary line: getsrc: ok (N bytes -> <file>).
        // ASCII "->" (not Unicode arrow) for console encoding safety on Windows.
        const std::string utf8 = to_utf8(outcome.editor_text);
        std::cout << "getsrc: ok (" << utf8.size() << " bytes -> " << to_utf8(opts.output_path) << ")\n";
        std::cout.flush();
    }

    if (session.process) CloseHandle(session.process);
    return static_cast<int>(Exit::Ok);
}

int run_insert(const Options& opts) {
    OpResult result;
    result.op = L"insert";
    result.type = opts.type;
    result.name = opts.name;
    Session session;
    // Quiet-UX frame state: the frame is shown via SW_SHOWNA (no focus steal)
    // and moved off-screen (WS_VISIBLE kept — required for the New dialog's
    // name propagation and the verify "*" removal, KB). The original position
    // and hidden state are restored on every exit path (R7).
    FrameState frame_state;
    // FrameGuard declared before the fail lambda so the lambda can stop it.
    // The guard is started later (before the New dialog command) and stopped
    // on every exit path via the fail lambda or the success path.
    FrameGuard frame_guard;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count());
    };
    auto fail = [&](Exit code, const wchar_t* name, const std::wstring& message) {
        frame_guard.stop();
        hide_if_launched(opts, session);
        restore_frame_quiet(static_cast<HWND>(find_main_frame(session.pid)), frame_state, opts.nohide);
        if (session.process) {
            CloseHandle(session.process);
            session.process = nullptr;
        }
        result.ok = false;
        result.error_code = static_cast<int>(code);
        result.error_name = name;
        result.error_message = message;
        result.elapsed_ms = elapsed_ms();
        emit(opts, result);
        return static_cast<int>(code);
    };

    BusyGuard busy = BusyGuard::acquire();
    if (!busy) return fail(Exit::Busy, L"busy", L"another tsdevctl invocation is running");

    if (!is_elevated()) return fail(Exit::NotElevated, L"not_elevated", L"tsdevctl must run in an elevated process (R8)");

    // Save the user's foreground window before any TSDev interaction (launch,
    // pipe-open, etc.) can steal focus. Restored at exit via restore_frame_quiet.
    if (!opts.nohide) save_foreground(frame_state);

    const Exit ready = ensure_tsdev_ready(opts, result, session);
    if (ready != Exit::Ok) return fail(ready, result.error_name.c_str(), result.error_message);

    // Read the source file before any TSDev interaction so a missing/unreadable
    // file fails fast without touching TSDev state.
    std::wstring source_text;
    std::wstring source_error;
    if (!read_source_file(opts.source_path, source_text, source_error))
        return fail(Exit::Unexpected, L"source_read_failed", source_error);

    // Proactive exists check: open the name through TSDev's own channel and
    // read the editor. A real item means the name is taken (avoid collision
    // popups); a phantom error page means the name is free to insert.
    const std::set<HWND> dialogs_before = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before = list_doc_windows(session.pid);
    ReadOutcome exists_outcome;
    {
        const std::optional<Failure> failure =
            open_read_and_close(opts, session, dialogs_before, docs_before, exists_outcome);
        if (failure) return fail(failure->code, failure->name, failure->message);
    }
    if (exists_outcome.exists)
        return fail(Exit::AlreadyExists, L"already_exists", L"item already exists in the TSDev library");

    // Open the phantom doc for insert: send the same pipe payload (open-or-
    // create on a nonexistent name creates a phantom empty doc).
    const std::set<HWND> dialogs_before2 = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before2 = list_doc_windows(session.pid);
    const std::wstring expected = opts.name + L" : " + opts.type_label;
    const std::wstring needle = L" - " + expected;
    const std::optional<Failure> open_failure = send_and_wait(opts, session, dialogs_before2, needle);
    if (open_failure) {
        const std::optional<Failure> late_failure =
            close_late_open(opts, session, docs_before2, dialogs_before2, expected);
        if (late_failure) return fail(late_failure->code, late_failure->name, late_failure->message);
        return fail(open_failure->code, open_failure->name, open_failure->message);
    }

    const std::vector<DocWindow> docs_after = list_doc_windows(session.pid);
    const DocMatch match = find_doc_window(docs_before2, docs_after, expected);
    if (match.window == nullptr)
        return fail(Exit::Timeout, L"timeout", L"frame title changed but no matching document window appeared");

    const HWND doc_hwnd = static_cast<HWND>(match.window->window);

    // Activate the doc window (MDI activation) so MFC command routing targets
    // this document when the verify command is sent to the frame.
    const HWND frame = static_cast<HWND>(find_main_frame(session.pid));
    const HWND mdiclient = frame ? FindWindowExW(frame, nullptr, L"MDIClient", nullptr) : nullptr;
    if (mdiclient) {
        DWORD_PTR activate_result = 0;
        SendMessageTimeoutW(mdiclient, WM_MDIACTIVATE, reinterpret_cast<WPARAM>(doc_hwnd), 0,
                            SMTO_ABORTIFHUNG, 2000, &activate_result);
    }

    // Trigger TSDev's own "New <Type>" command on the phantom's MDI child
    // (app-class commands are only routable via an MDI child window; the frame
    // suppresses them). This opens TSDev's "New <Type>" dialog.
    // The frame MUST be WS_VISIBLE when the dialog's OK is dispatched — with a
    // hidden frame TSDev creates the document with an empty name (KB). Show
    // it quietly (SW_SHOWNA + off-screen) and restore at exit (R7).
    show_frame_quiet(frame, opts.nohide, frame_state);
    const std::set<HWND> wins_before_new = snapshot_tsdev_windows(session.pid);
    // Start the frame guard: a background thread that continuously keeps the
    // frame off-screen while the New dialog creation repositions it on-screen.
    // This eliminates visual flicker and focus stealing (the dialog creation's
    // SetWindowPos can steal focus from the user's foreground application).
    // The guard was declared before the fail lambda so the lambda can stop it.
    if (!opts.nohide) frame_guard.start(frame);
    PostMessageW(doc_hwnd, WM_COMMAND, static_cast<WPARAM>(opts.type_new_cmd_id), 0);
    // The create-dialog class is type-specific (TS_IDD_COMMONCREATE for
    // Strategy, TS_IDD_FUNCCREATE for Function, ... — KB); match the common
    // TS_IDD_ prefix of TSDev's own dialogs.
    const HWND create_dlg = wait_for_tsdev_dialog(session.pid, L"TS_IDD_",
                                                  wins_before_new, opts.timeouts_disabled,
                                                  opts.timeout_ms);
    // Stop the frame guard: the dialog has appeared, the frame is now stable.
    frame_guard.stop();
    if (!create_dlg) {
        std::wstring close_note;
        if (match.opened_by_us) {
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            const auto cf = close_doc_window(opts, session, d_before, *match.window);
            if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
        }
        return fail(Exit::Timeout, L"timeout",
                    L"New " + opts.type + L" dialog did not appear" + close_note);
    }

    // The New dialog creation repositions the frame back on-screen (verified
    // live). Re-move the frame off-screen to eliminate visual flicker while
    // keeping WS_VISIBLE (required for name propagation and verify, KB).
    // The dialog itself is a small window that doesn't steal focus (verified
    // live: with the frame off-screen, the dialog appearing on-screen did NOT
    // steal focus from another application).
    if (!opts.nohide && frame) {
        move_offscreen(frame);
    }

    // Fill the name (first Edit child) and confirm with IDOK — the same code
    // paths the UI dialog triggers, no synthetic clicks or keystrokes.
    HWND name_edit = nullptr;
    EnumChildWindows(create_dlg, [](HWND child, LPARAM param) -> BOOL {
        wchar_t cls[32]{};
        if (GetClassNameW(child, cls, 32) == 0) return TRUE;
        if (std::wstring(cls) != L"Edit") return TRUE;
        *reinterpret_cast<HWND*>(param) = child;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&name_edit));
    if (!name_edit) {
        PostMessageW(create_dlg, WM_COMMAND, IDCANCEL, 0);
        std::wstring close_note;
        if (match.opened_by_us) {
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            const auto cf = close_doc_window(opts, session, d_before, *match.window);
            if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
        }
        return fail(Exit::Unexpected, L"create_dialog",
                    L"New " + opts.type + L" dialog has no name field" + close_note);
    }
    DWORD_PTR set_res = 0;
    const LRESULT set_ok = SendMessageTimeoutW(name_edit, WM_SETTEXT, 0,
                                               reinterpret_cast<LPARAM>(opts.name.c_str()),
                                               SMTO_ABORTIFHUNG, 2000, &set_res);
    if (!set_ok) {
        // A hung/aborted name set would leave the dialog with an empty name —
        // TSDev would then create an empty-named document (KB). Fail fast.
        PostMessageW(create_dlg, WM_COMMAND, IDCANCEL, 0);
        std::wstring close_note;
        if (match.opened_by_us) {
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            const auto cf = close_doc_window(opts, session, d_before, *match.window);
            if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
        }
        return fail(Exit::Unexpected, L"create_dialog",
                    L"could not set the name in the New " + opts.type + L" dialog" + close_note);
    }
    PostMessageW(create_dlg, WM_COMMAND, IDOK, 0);

    // Wait for the real document window (the New dialog created it; the
    // phantom remains open behind it).
    HWND new_doc = nullptr;
    const auto new_doc_deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    // Dialog snapshot taken after the New dialog was dismissed (IDOK) so the
    // create dialog itself is not misread as a new error dialog.
    const std::set<HWND> dialogs_after_new = snapshot_tsdev_dialogs(session.pid);
    // A doc created by the New dialog is necessarily a NEW window. Pre-existing
    // windows whose titles merely CONTAIN the name (a superset-named item, the
    // same name of another type, a session-restored duplicate) must never be
    // selected: the source would be written into the user's document (R6).
    std::set<HWND> pre_existing;
    for (const DocWindow& dw : docs_before2)
        pre_existing.insert(static_cast<HWND>(dw.window));
    for (;;) {
        if (!process_alive(session.pid))
            return fail(Exit::Crashed, L"tsdev_died", L"TSDev exited while creating the document");
        if (crash_report_running())
            return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected while creating the document");
        const std::wstring dlg_text = check_new_dialogs(session.pid, dialogs_after_new);
        if (!dlg_text.empty()) {
            // TSDev popped an error dialog (e.g. invalid name) — report it.
            // Dismiss the create dialog if still up, and close the phantom
            // before failing.
            PostMessageW(create_dlg, WM_CLOSE, 0, 0);
            if (match.opened_by_us) {
                const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
                close_doc_window(opts, session, d_before, *match.window);
            }
            return fail(Exit::ErrorDialog, L"tsdev_dialog", dlg_text);
        }
        for (const DocWindow& dw : list_doc_windows(session.pid)) {
            const HWND h = static_cast<HWND>(dw.window);
            if (h == doc_hwnd) continue;
            if (pre_existing.count(h)) continue;
            std::wstring t = dw.title;
            std::transform(t.begin(), t.end(), t.begin(), towlower);
            std::wstring want = opts.name;
            std::transform(want.begin(), want.end(), want.begin(), towlower);
            if (t.find(want) != std::wstring::npos) { new_doc = h; break; }
        }
        if (new_doc) break;
        if (!opts.timeouts_disabled && std::chrono::steady_clock::now() >= new_doc_deadline) {
            std::wstring close_note;
            if (match.opened_by_us) {
                const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
                const auto cf = close_doc_window(opts, session, d_before, *match.window);
                if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
            }
            return fail(Exit::Timeout, L"timeout",
                        L"new document window did not appear after the New dialog" + close_note);
        }
        Sleep(100);
    }

    // Set the source into the new document's editor via WM_SETTEXT (no
    // clipboard, no UI automation).
    if (!set_doc_text(new_doc, source_text)) {
        // Close BOTH the created document and the phantom before failing.
        std::wstring close_note;
        const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
        const auto cf_new = close_doc_window(opts, session, d_before, DocWindow{new_doc, L""});
        if (cf_new) close_note = L" [also: failed to close new document: " + cf_new->message + L"]";
        if (match.opened_by_us) {
            const auto cf = close_doc_window(opts, session, d_before, *match.window);
            if (cf) close_note += L" [also: failed to close document: " + cf->message + L"]";
        }
        return fail(Exit::Unexpected, L"set_text_failed",
                    L"could not set editor text via WM_SETTEXT" + close_note);
    }

    // Activate the new document so the verify command routes to it.
    if (mdiclient) {
        DWORD_PTR activate_result = 0;
        SendMessageTimeoutW(mdiclient, WM_MDIACTIVATE, reinterpret_cast<WPARAM>(new_doc), 0,
                            SMTO_ABORTIFHUNG, 2000, &activate_result);
    }

    // The frame stays WS_VISIBLE during the verify: with it hidden the "*"
    // dirty marker is never removed (neither success nor failure updates the
    // title, KB), which would break the completion poll below. The frame is
    // off-screen (quiet-UX) and restored on every exit path via frame_state.

    // Trigger the verify (compile + save) code path via WM_COMMAND 0x296a,
    // the same command ID the menu sends. PostMessageW (not SendMessageW)
    // because the failure path shows a modal MessageBox that would block.
    const std::set<HWND> dialogs_before_verify = snapshot_tsdev_dialogs(session.pid);
    PostMessageW(frame, WM_COMMAND, 0x296a, 0);

    // Poll for compile completion. With the frame visible, compile failures
    // are SILENT: TSDev routes the errors into its Output error list and
    // removes the "*" quickly for BOTH success and failure (proven live,
    // 2026-09-03), so the "*" cycle alone cannot classify the result — the
    // error-list summary read after the poll is authoritative. A dialog
    // observed during the poll (detected + dismissed by check_new_dialogs)
    // is kept as a safety net. On the success path the "*" dirty marker is
    // removed when the compile finishes; the post-verify exists check remains
    // the definitive success confirmation.
    std::wstring compile_error;
    bool compile_error_seen = false;
    const auto verify_deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    // Two-phase poll: wait for the "*" dirty marker to appear (compile
    // started), then for it to disappear (compile finished). WM_SETTEXT may
    // not set the document's modified flag, so the "*" may never appear —
    // fall back to the post-verify exists check after a sub-deadline.
    bool dirty_seen = false;
    const auto first_check_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    const auto no_dirty_deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    for (;;) {
        if (!process_alive(session.pid))
            return fail(Exit::Crashed, L"tsdev_died", L"TSDev exited during verify");
        if (crash_report_running())
            return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected during verify");
        if (!IsWindow(new_doc)) {
            std::wstring close_note;
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            const auto cf_new = close_doc_window(opts, session, d_before, DocWindow{new_doc, L""});
            if (cf_new) close_note = L" [also: failed to close new document: " + cf_new->message + L"]";
            if (match.opened_by_us) {
                const auto cf = close_doc_window(opts, session, d_before, *match.window);
                if (cf) close_note += L" [also: failed to close document: " + cf->message + L"]";
            }
            return fail(Exit::Crashed, L"tsdev_died",
                        L"document window disappeared during verify" + close_note);
        }
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before_verify);
        if (!dialog_text.empty()) {
            compile_error = dialog_text;
            compile_error_seen = true;
            Sleep(500);  // let TSDev finish its internal failure cleanup (error markers etc.)
            break;  // dialog during verify = compile failed; do not wait for the "*" cycle
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < first_check_time) { Sleep(100); continue; }  // minimum wait before first title check
        // Read the raw title via a bounded cross-process read to check for
        // the "*" dirty marker. GetWindowTextW can block indefinitely if the
        // target's UI thread is wedged; SendMessageTimeoutW aborts on hang.
        wchar_t title_buf[256]{};
        DWORD_PTR copied = 0;
        std::wstring title;
        if (SendMessageTimeoutW(new_doc, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(title_buf),
                                SMTO_ABORTIFHUNG, 2000, &copied)) {
            title = title_buf;
        }
        if (!title.empty() && title.back() == L'*') {
            dirty_seen = true;
        } else if (dirty_seen) {
            break;  // "*" appeared then disappeared = compile finished
        } else if (now >= no_dirty_deadline) {
            break;  // "*" never appeared — fall back to post-verify exists check
        }
        if (!opts.timeouts_disabled && now >= verify_deadline) {
            std::wstring close_note;
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            // The doc may be dirty (the source was set; the compile outcome is
            // unknown) — answer No to any save prompt so the close completes.
            const auto cf_new = close_doc_discard(opts, session, d_before, DocWindow{new_doc, L""});
            if (cf_new) close_note = L" [also: failed to close new document: " + cf_new->message + L"]";
            if (match.opened_by_us) {
                const auto cf = close_doc_discard(opts, session, d_before, *match.window);
                if (cf) close_note += L" [also: failed to close document: " + cf->message + L"]";
            }
            return fail(Exit::Timeout, L"timeout",
                        L"verify did not complete within the timeout" + close_note);
        }
        Sleep(100);
    }

    // Failure classification (both sources, before any doc closing). The
    // frame is visible during the verify, so compile failures are routed to
    // TSDev's Output error list silently (no dialog — KB). The list's summary
    // row ("N error(s), M warning(s)") is authoritative for the compile
    // result; the dialog path is a safety net (not expected with the frame
    // visible).
    bool compile_failed = false;
    std::wstring failure_text;
    if (compile_error_seen) {
        compile_failed = true;
        failure_text = compile_error;
    } else {
        std::wstring error_rows;
        int error_count = 0;
        if (read_compile_error_summary(session.pid, error_count, error_rows) && error_count > 0) {
            compile_failed = true;
            failure_text = !error_rows.empty() ? error_rows : std::to_wstring(error_count) + L" error(s)";
        }
    }

    // A failed compile leaves the document dirty ("*" already removed): close
    // BOTH documents answering No to TSDev's save prompt (the item remains in
    // the library saved-uncompiled) before reporting.
    if (compile_failed) {
        std::wstring close_note;
        const auto cf_new = close_doc_discard(opts, session, dialogs_before_verify, DocWindow{new_doc, L""});
        if (cf_new) close_note = L" [also: " + cf_new->message + L"]";
        if (match.opened_by_us) {
            const auto cf = close_doc_discard(opts, session, dialogs_before_verify, *match.window);
            if (cf) close_note += L" [also: " + cf->message + L"]";
        }
        // R10: the item was saved before the compile ran, so it remains in the
        // library with uncompiled source — state that in the error itself.
        return fail(Exit::CompileFailed, L"compile_error",
                    failure_text +
                    L" [item '" + opts.name + L"' was created and saved uncompiled]" +
                    close_note);
    }

    // Close the created document and the phantom (if we opened it) before the
    // post-verify exists check. Neither is dirty on the success path, so no
    // save prompt is raised.
    {
        const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
        if (match.opened_by_us) {
            const auto cf = close_doc_window(opts, session, d_before, *match.window);
            if (cf) return fail(cf->code, cf->name, cf->message);
        }
        const auto cf_new = close_doc_window(opts, session, d_before, DocWindow{new_doc, L""});
        if (cf_new) return fail(cf_new->code, cf_new->name, cf_new->message);
    }

    // Post-verify exists check: confirms the item was saved (covers the
    // no-dialog silent-failure case).
    const std::set<HWND> dialogs_before3 = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before3 = list_doc_windows(session.pid);
    ReadOutcome verify_outcome;
    {
        const std::optional<Failure> failure =
            open_read_and_close(opts, session, dialogs_before3, docs_before3, verify_outcome);
        if (failure) return fail(failure->code, failure->name, failure->message);
    }
    if (!verify_outcome.exists)
        return fail(Exit::CompileFailed, L"compile_failed",
                    L"compile/verify did not save the item (no error dialog, but item not found after verify)");

    hide_if_launched(opts, session);
    restore_frame_quiet(static_cast<HWND>(find_main_frame(session.pid)), frame_state, opts.nohide);
    result.ok = true;
    result.elapsed_ms = elapsed_ms();
    emit(opts, result);
    if (session.process) CloseHandle(session.process);
    return static_cast<int>(Exit::Ok);
}

// Strips trailing whitespace (spaces, tabs, CR, LF) from `s`. Used by modify's
// post-verify source confirmation: read_doc_text already strips trailing NULs,
// and trailing-newline differences between the set and re-read source must not
// cause a false mismatch.
std::wstring rtrim_ws(std::wstring s) {
    while (!s.empty()) {
        const wchar_t c = s.back();
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') s.pop_back();
        else break;
    }
    return s;
}

// Normalizes line endings for source comparison: removes all \r characters,
// so \r\n (Windows) and \n (Unix/TSDev editor) compare equal. The source file
// may have \r\n (Windows text mode), while TSDev's editor normalizes to \n.
// Without this, modify's post-verify source confirmation would falsely report
// "source_not_updated" for Windows-line-ending source files.
std::wstring normalize_eols(std::wstring s) {
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    return s;
}

int run_modify(const Options& opts) {
    OpResult result;
    result.op = L"modify";
    result.type = opts.type;
    result.name = opts.name;
    Session session;
    // Quiet-UX frame state: same as insert — SW_SHOWNA + off-screen.
    FrameState frame_state;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count());
    };
    auto fail = [&](Exit code, const wchar_t* name, const std::wstring& message) {
        hide_if_launched(opts, session);
        restore_frame_quiet(static_cast<HWND>(find_main_frame(session.pid)), frame_state, opts.nohide);
        if (session.process) {
            CloseHandle(session.process);
            session.process = nullptr;
        }
        result.ok = false;
        result.error_code = static_cast<int>(code);
        result.error_name = name;
        result.error_message = message;
        result.elapsed_ms = elapsed_ms();
        emit(opts, result);
        return static_cast<int>(code);
    };

    BusyGuard busy = BusyGuard::acquire();
    if (!busy) return fail(Exit::Busy, L"busy", L"another tsdevctl invocation is running");

    if (!is_elevated()) return fail(Exit::NotElevated, L"not_elevated", L"tsdevctl must run in an elevated process (R8)");

    // Save the user's foreground window before any TSDev interaction (launch,
    // pipe-open, etc.) can steal focus. Restored at exit via restore_frame_quiet.
    if (!opts.nohide) save_foreground(frame_state);

    const Exit ready = ensure_tsdev_ready(opts, result, session);
    if (ready != Exit::Ok) return fail(ready, result.error_name.c_str(), result.error_message);

    // Read the source file before any TSDev interaction so a missing/unreadable
    // file fails fast without touching TSDev state.
    std::wstring source_text;
    std::wstring source_error;
    if (!read_source_file(opts.source_path, source_text, source_error))
        return fail(Exit::Unexpected, L"source_read_failed", source_error);

    // Proactive exists check: open the name through TSDev's own channel and
    // read the editor. A phantom error page means the item does not exist
    // (R2: notify if absent); a real item means it is safe to modify. The doc
    // opened for the check is closed by open_read_and_close.
    const std::set<HWND> dialogs_before = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before = list_doc_windows(session.pid);
    ReadOutcome exists_outcome;
    {
        const std::optional<Failure> failure =
            open_read_and_close(opts, session, dialogs_before, docs_before, exists_outcome);
        if (failure) return fail(failure->code, failure->name, failure->message);
    }
    if (!exists_outcome.exists)
        return fail(Exit::NotFound, L"not_found", L"item does not exist in the TSDev library");

    // Open the real item for modify: send the same pipe payload (open-or-
    // create on an existing name opens the real document).
    const std::set<HWND> dialogs_before2 = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before2 = list_doc_windows(session.pid);
    const std::wstring expected = opts.name + L" : " + opts.type_label;
    const std::wstring needle = L" - " + expected;
    const std::optional<Failure> open_failure = send_and_wait(opts, session, dialogs_before2, needle);
    if (open_failure) {
        const std::optional<Failure> late_failure =
            close_late_open(opts, session, docs_before2, dialogs_before2, expected);
        if (late_failure) return fail(late_failure->code, late_failure->name, late_failure->message);
        return fail(open_failure->code, open_failure->name, open_failure->message);
    }

    const std::vector<DocWindow> docs_after = list_doc_windows(session.pid);
    const DocMatch match = find_doc_window(docs_before2, docs_after, expected);
    if (match.window == nullptr)
        return fail(Exit::Timeout, L"timeout", L"frame title changed but no matching document window appeared");

    const HWND doc_hwnd = static_cast<HWND>(match.window->window);

    // Activate the doc window (MDI activation) so MFC command routing targets
    // this document when the verify command is sent to the frame.
    const HWND frame = static_cast<HWND>(find_main_frame(session.pid));
    const HWND mdiclient = frame ? FindWindowExW(frame, nullptr, L"MDIClient", nullptr) : nullptr;
    if (mdiclient) {
        DWORD_PTR activate_result = 0;
        SendMessageTimeoutW(mdiclient, WM_MDIACTIVATE, reinterpret_cast<WPARAM>(doc_hwnd), 0,
                            SMTO_ABORTIFHUNG, 2000, &activate_result);
    }

    // The frame MUST be WS_VISIBLE during the verify: with it hidden the "*"
    // dirty marker is never removed (KB), which would break the completion
    // poll below. Show it quietly (SW_SHOWNA + off-screen) and restore at
    // exit (R7).
    show_frame_quiet(frame, opts.nohide, frame_state);

    // Set the new source into the editor via WM_SETTEXT (no clipboard, no UI
    // automation). If the set fails, close the doc and fail.
    if (!set_doc_text(doc_hwnd, source_text)) {
        std::wstring close_note;
        const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
        if (match.opened_by_us) {
            const auto cf = close_doc_window(opts, session, d_before, *match.window);
            if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
        }
        return fail(Exit::Unexpected, L"set_text_failed",
                    L"could not set editor text via WM_SETTEXT" + close_note);
    }

    // Activate the document again so the verify command routes to it.
    if (mdiclient) {
        DWORD_PTR activate_result = 0;
        SendMessageTimeoutW(mdiclient, WM_MDIACTIVATE, reinterpret_cast<WPARAM>(doc_hwnd), 0,
                            SMTO_ABORTIFHUNG, 2000, &activate_result);
    }

    // Trigger the verify (compile + save) code path via WM_COMMAND 0x296a,
    // the same command ID the menu sends. PostMessageW (not SendMessageW)
    // because the failure path shows a modal MessageBox that would block.
    const std::set<HWND> dialogs_before_verify = snapshot_tsdev_dialogs(session.pid);
    PostMessageW(frame, WM_COMMAND, 0x296a, 0);

    // Poll for compile completion. With the frame visible, compile failures
    // are SILENT: TSDev routes the errors into its Output error list and
    // removes the "*" quickly for BOTH success and failure (proven live,
    // 2026-09-03), so the "*" cycle alone cannot classify the result — the
    // error-list summary read after the poll is authoritative. A dialog
    // observed during the poll (detected + dismissed by check_new_dialogs)
    // is kept as a safety net. On the success path the "*" dirty marker is
    // removed when the compile finishes; the post-verify source confirmation
    // remains the definitive success check.
    std::wstring compile_error;
    bool compile_error_seen = false;
    const auto verify_deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    // Two-phase poll: wait for the "*" dirty marker to appear (compile
    // started), then for it to disappear (compile finished). WM_SETTEXT may
    // not set the document's modified flag, so the "*" may never appear —
    // fall back to the post-verify source confirmation after a sub-deadline.
    bool dirty_seen = false;
    const auto first_check_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    const auto no_dirty_deadline = opts.timeouts_disabled
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    for (;;) {
        if (!process_alive(session.pid))
            return fail(Exit::Crashed, L"tsdev_died", L"TSDev exited during verify");
        if (crash_report_running())
            return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected during verify");
        if (!IsWindow(doc_hwnd)) {
            std::wstring close_note;
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            if (match.opened_by_us) {
                const auto cf = close_doc_window(opts, session, d_before, *match.window);
                if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
            }
            return fail(Exit::Crashed, L"tsdev_died",
                        L"document window disappeared during verify" + close_note);
        }
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before_verify);
        if (!dialog_text.empty()) {
            compile_error = dialog_text;
            compile_error_seen = true;
            Sleep(500);  // let TSDev finish its internal failure cleanup (error markers etc.)
            break;  // dialog during verify = compile failed; do not wait for the "*" cycle
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < first_check_time) { Sleep(100); continue; }  // minimum wait before first title check
        // Read the raw title via a bounded cross-process read to check for
        // the "*" dirty marker. GetWindowTextW can block indefinitely if the
        // target's UI thread is wedged; SendMessageTimeoutW aborts on hang.
        wchar_t title_buf[256]{};
        DWORD_PTR copied = 0;
        std::wstring title;
        if (SendMessageTimeoutW(doc_hwnd, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(title_buf),
                                SMTO_ABORTIFHUNG, 2000, &copied)) {
            title = title_buf;
        }
        if (!title.empty() && title.back() == L'*') {
            dirty_seen = true;
        } else if (dirty_seen) {
            break;  // "*" appeared then disappeared = compile finished
        } else if (now >= no_dirty_deadline) {
            break;  // "*" never appeared — fall back to post-verify source confirmation
        }
        if (!opts.timeouts_disabled && now >= verify_deadline) {
            std::wstring close_note;
            const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
            // The doc may be dirty (the source was set; the compile outcome is
            // unknown) — answer No to any save prompt so the close completes.
            if (match.opened_by_us) {
                const auto cf = close_doc_discard(opts, session, d_before, *match.window);
                if (cf) close_note = L" [also: failed to close document: " + cf->message + L"]";
            }
            return fail(Exit::Timeout, L"timeout",
                        L"verify did not complete within the timeout" + close_note);
        }
        Sleep(100);
    }

    // Failure classification (both sources, before any doc closing). The
    // frame is visible during the verify, so compile failures are routed to
    // TSDev's Output error list silently (no dialog — KB). The list's summary
    // row ("N error(s), M warning(s)") is authoritative for the compile
    // result; the dialog path is a safety net (not expected with the frame
    // visible).
    bool compile_failed = false;
    std::wstring failure_text;
    if (compile_error_seen) {
        compile_failed = true;
        failure_text = compile_error;
    } else {
        std::wstring error_rows;
        int error_count = 0;
        if (read_compile_error_summary(session.pid, error_count, error_rows) && error_count > 0) {
            compile_failed = true;
            failure_text = !error_rows.empty() ? error_rows : std::to_wstring(error_count) + L" error(s)";
        }
    }

    // A failed compile leaves the document dirty ("*" already removed): close
    // it answering No to TSDev's save prompt. The verify path saves before
    // compiling, so the item remains in the library with uncompiled NEW
    // source (R10) — state that in the error itself.
    if (compile_failed) {
        std::wstring close_note;
        if (match.opened_by_us) {
            const auto cf = close_doc_discard(opts, session, dialogs_before_verify, *match.window);
            if (cf) close_note = L" [also: " + cf->message + L"]";
        }
        return fail(Exit::CompileFailed, L"compile_error",
                    failure_text +
                    L" [item '" + opts.name + L"' was saved with uncompiled new source]" +
                    close_note);
    }

    // Close the document (not dirty on the success path, so no save prompt).
    if (match.opened_by_us) {
        const std::set<HWND> d_before = snapshot_tsdev_dialogs(session.pid);
        const auto cf = close_doc_window(opts, session, d_before, *match.window);
        if (cf) return fail(cf->code, cf->name, cf->message);
    }

    // Post-verify SOURCE CONFIRMATION (key difference from insert): the item
    // already existed, so existence alone is NOT confirmation — the source
    // must have been updated. Re-read the item's source and compare it with
    // the source that was set (trailing whitespace stripped on both sides).
    const std::set<HWND> dialogs_before3 = snapshot_tsdev_dialogs(session.pid);
    const std::vector<DocWindow> docs_before3 = list_doc_windows(session.pid);
    ReadOutcome verify_outcome;
    {
        const std::optional<Failure> failure =
            open_read_and_close(opts, session, dialogs_before3, docs_before3, verify_outcome);
        if (failure) return fail(failure->code, failure->name, failure->message);
    }
    if (!verify_outcome.exists)
        return fail(Exit::Unexpected, L"not_found",
                    L"verify completed but the item was not found after verify");
    if (rtrim_ws(normalize_eols(verify_outcome.editor_text)) != rtrim_ws(normalize_eols(source_text)))
        return fail(Exit::Unexpected, L"source_not_updated",
                    L"verify completed but the source was not updated (the item may still have its old source)");

    hide_if_launched(opts, session);
    restore_frame_quiet(static_cast<HWND>(find_main_frame(session.pid)), frame_state, opts.nohide);
    result.ok = true;
    result.elapsed_ms = elapsed_ms();
    emit(opts, result);
    if (session.process) CloseHandle(session.process);
    return static_cast<int>(Exit::Ok);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options opts;
    const ParseOutcome parsed = parse_args(argc, argv, opts);
    if (parsed.help) {
        std::cout << to_utf8(usage_text());
        return static_cast<int>(Exit::Ok);
    }
    if (!parsed.ok) {
        std::cerr << "tsdevctl: " << to_utf8(parsed.error) << "\n";
        return static_cast<int>(Exit::Usage);
    }
    if (opts.verb == L"exists") return run_exists(opts);
    if (opts.verb == L"getsrc") return run_getsrc(opts);
    if (opts.verb == L"insert") return run_insert(opts);
    if (opts.verb == L"modify") return run_modify(opts);
    return run_open(opts);
}
