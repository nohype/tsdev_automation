#include "tsdevlib/args.h"
#include "tsdevlib/busy_guard.h"
#include "tsdevlib/dialog_check.h"
#include "tsdevlib/doc_read.h"
#include "tsdevlib/elevation.h"
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
#include <string>
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
std::optional<Failure> send_and_wait(const Options& opts, const Session& session,
                                     const std::set<HWND>& dialogs_before, const std::wstring& needle) {
    const std::wstring payload = L"\"" + opts.name + L"\", " + std::to_wstring(opts.type_number);
    std::wstring error;
    if (!pipe_send(payload, opts.timeouts_disabled, opts.timeout_ms, error))
        return Failure{Exit::PipeFailed, L"pipe_failed", error};
    if (!wait_for_title(session.pid, needle, opts.timeouts_disabled, opts.timeout_ms)) {
        const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
        if (!dialog_text.empty()) return Failure{Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text};
        if (crash_report_running()) return Failure{Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected"};
        if (!process_alive(session.pid))
            return Failure{Exit::Crashed, L"tsdev_died", L"TSDev exited during the operation"};
        return Failure{Exit::Timeout, L"timeout", L"item did not open within the timeout"};
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
    std::optional<Failure> failure;
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
        const std::string utf8 = to_utf8(outcome.editor_text);
        std::cout << "getsrc: ok (" << utf8.size() << " bytes -> " << to_utf8(opts.output_path) << ")\n";
        std::cout.flush();
    }

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
    return run_open(opts);
}
