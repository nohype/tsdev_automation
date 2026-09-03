#include "tsdevlib/args.h"
#include "tsdevlib/busy_guard.h"
#include "tsdevlib/dialog_check.h"
#include "tsdevlib/elevation.h"
#include "tsdevlib/exit_codes.h"
#include "tsdevlib/json_writer.h"
#include "tsdevlib/pipe_client.h"
#include "tsdevlib/tsdev_process.h"
#include "tsdevlib/window_watch.h"

#include <windows.h>
#include <chrono>
#include <iostream>
#include <set>

namespace {
std::wstring human_line(const OpResult& r) {
    std::wstring line = r.op + L": " + (r.ok ? L"ok" : L"failed");
    if (r.has_pid) line += L" (pid " + std::to_wstring(r.pid) + L", " + std::to_wstring(r.elapsed_ms) + L" ms)";
    if (r.error_code != 0) line += L" [" + std::to_wstring(r.error_code) + L" " + r.error_name + L"] " + r.error_message;
    return line;
}

void emit(const Options& opts, const OpResult& result) {
    const std::string line = to_utf8(opts.json ? make_result_json(result) : human_line(result));
    std::cout << line << "\n";
    std::cout.flush();
}

int run_open(const Options& opts) {
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };

    OpResult result;
    result.op = L"open";
    result.type = opts.type;
    result.name = opts.name;

    unsigned long pid = 0;
    void* process = nullptr;
    bool launched = false;
    std::set<HWND> dialogs_before;

    // TSDev re-shows its frame while it finishes its own startup, so the
    // hidden state is (re-)enforced at every exit rather than only once.
    const auto ensure_hidden = [&] {
        if (launched && !opts.nohide) {
            const auto frame = static_cast<HWND>(find_main_frame(pid));
            if (frame) ShowWindow(frame, SW_HIDE);
        }
    };
    const auto fail = [&](Exit code, const wchar_t* name, const std::wstring& message) {
        ensure_hidden();
        if (process) {
            CloseHandle(process);
            process = nullptr;
        }
        result.ok = false;
        result.error_code = static_cast<int>(code);
        result.error_name = name;
        result.error_message = message;
        result.elapsed_ms = elapsed();
        emit(opts, result);
        return static_cast<int>(code);
    };

    BusyGuard busy = BusyGuard::acquire();
    if (!busy) return fail(Exit::Busy, L"busy", L"another tsdevctl invocation is running");

    if (!is_elevated()) return fail(Exit::NotElevated, L"not_elevated", L"tsdevctl must run in an elevated process (R8)");

    const std::wstring exe = locate_tsdev_exe(opts.ts_path);
    if (exe.empty()) return fail(Exit::LaunchFailed, L"tsdev_not_found", L"TSDev.exe not located (use --ts-path)");

    if (find_running_tsdev(pid)) {
        result.has_pid = true;
        result.pid = pid;
        if (!opts.nohide) {
            const auto frame = static_cast<HWND>(find_main_frame(pid));
            if (frame) ShowWindow(frame, SW_HIDE);
        }
    } else {
        process = launch_tsdev(exe, !opts.nohide, pid);
        if (!process) return fail(Exit::LaunchFailed, L"launch_failed", L"CreateProcess failed for " + exe);
        launched = true;
        result.has_pid = true;
        result.pid = pid;
        const auto deadline = opts.timeouts_disabled
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
        for (;;) {
            if (!process_alive(pid)) return fail(Exit::Crashed, L"tsdev_died", L"TSDev exited during startup");
            if (find_main_frame(pid)) break;
            if (std::chrono::steady_clock::now() >= deadline)
                return fail(Exit::LaunchFailed, L"launch_timeout", L"TSDev main window did not appear in time");
            Sleep(100);
        }
        if (!opts.nohide) ShowWindow(static_cast<HWND>(find_main_frame(pid)), SW_HIDE);
    }

    dialogs_before = snapshot_tsdev_dialogs(pid);

    const std::wstring payload = L"\"" + opts.name + L"\", " + std::to_wstring(opts.type_number);
    if (!pipe_send(payload, opts.timeouts_disabled, opts.timeout_ms, result.error_message))
        return fail(Exit::PipeFailed, L"pipe_failed", result.error_message);

    const std::wstring needle = L" - " + opts.name + L" : " + opts.type_label;
    if (!wait_for_title(pid, needle, opts.timeouts_disabled, opts.timeout_ms)) {
        const std::wstring dialog_text = check_new_dialogs(pid, dialogs_before);
        if (!dialog_text.empty())
            return fail(Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text);
        if (crash_report_running()) return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected");
        if (!process_alive(pid)) return fail(Exit::Crashed, L"tsdev_died", L"TSDev exited during the operation");
        return fail(Exit::Timeout, L"timeout", L"item did not open within the timeout");
    }

    const std::wstring dialog_text = check_new_dialogs(pid, dialogs_before);
    if (!dialog_text.empty()) return fail(Exit::ErrorDialog, L"error_dialog", L"TSDev dialog: " + dialog_text);
    if (crash_report_running()) return fail(Exit::Crashed, L"crash_report", L"TSCrashReport.exe detected");

    ensure_hidden();
    result.ok = true;
    result.elapsed_ms = elapsed();
    emit(opts, result);
    if (process) CloseHandle(process);
    return static_cast<int>(Exit::Ok);
}
}

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
    return run_open(opts);
}
