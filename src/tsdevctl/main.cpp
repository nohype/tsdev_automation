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
    if (failure) return fail(failure->code, failure->name, failure->message);

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

    const std::wstring needle = L" - " + opts.name + L" : " + opts.type_label;
    const std::optional<Failure> failure = send_and_wait(opts, session, dialogs_before, needle);
    if (failure) return fail(failure->code, failure->name, failure->message);

    // Locate the document window the pipe command produced. The frame title
    // echoes the requested name (verified live), so the exact match is the
    // requested spelling; the case-insensitive pass covers a TSDev that
    // normalizes to the stored spelling. Only windows this invocation created
    // are closed again (R6: leave everything else exactly as found).
    const std::wstring expected = opts.name + L" : " + opts.type_label;
    const std::vector<DocWindow> docs_after = list_doc_windows(session.pid);
    const DocWindow* target = nullptr;
    bool opened_by_us = false;
    for (const DocWindow& doc : docs_after) {
        std::wstring title = doc.title;
        while (!title.empty() && title.back() == L'*') title.pop_back();
        if (title == expected) {
            target = &doc;
            opened_by_us = std::none_of(docs_before.begin(), docs_before.end(), [&](const DocWindow& before) {
                return before.window == doc.window;
            });
            break;
        }
    }
    if (!target) {
        for (const DocWindow& doc : docs_after) {
            if (lower(doc.title).find(lower(expected)) != std::wstring::npos) {
                target = &doc;
                opened_by_us = std::none_of(docs_before.begin(), docs_before.end(), [&](const DocWindow& before) {
                    return before.window == doc.window;
                });
                break;
            }
        }
    }
    if (!target) return fail(Exit::Timeout, L"timeout", L"frame title changed but no matching document window appeared");

    std::wstring editor_text;
    if (!read_doc_text(target->window, editor_text))
        return fail(Exit::Unexpected, L"editor_missing", L"no editor control found in the opened document window");
    const bool exists = !is_phantom_error_text(editor_text);

    if (opened_by_us) {
        // Close what this invocation opened; a left-behind phantom document
        // would be restored by TSDev's session handling on the next start.
        PostMessageW(static_cast<HWND>(target->window), WM_CLOSE, 0, 0);
        const auto close_deadline = opts.timeouts_disabled
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
        for (;;) {
            if (!IsWindow(static_cast<HWND>(target->window))) break;
            const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
            if (!dialog_text.empty()) return fail(Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text);
            if (crash_report_running()) return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected");
            if (!process_alive(session.pid)) return fail(Exit::Crashed, L"tsdev_died", L"TSDev exited during the operation");
            if (std::chrono::steady_clock::now() >= close_deadline)
                return fail(Exit::Timeout, L"timeout", L"opened document did not close within the timeout");
            Sleep(50);
        }
    }

    const std::wstring dialog_text = check_new_dialogs(session.pid, dialogs_before);
    if (!dialog_text.empty()) return fail(Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text);
    if (crash_report_running()) return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected");

    hide_if_launched(opts, session);
    result.ok = true;
    result.reports_exists = true;
    result.exists_value = exists;
    result.elapsed_ms = elapsed_ms();
    emit(opts, result);
    if (session.process) CloseHandle(session.process);
    return static_cast<int>(exists ? Exit::Ok : Exit::NotFound);
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
    return opts.verb == L"exists" ? run_exists(opts) : run_open(opts);
}
