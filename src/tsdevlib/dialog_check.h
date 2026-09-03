#pragma once
#include <windows.h>
#include <set>
#include <string>

std::set<HWND> snapshot_tsdev_dialogs(unsigned long pid);
bool crash_report_running();

// Dismisses (WM_CLOSE) dialogs that appeared since the snapshot and returns
// their combined text; empty means no new dialog.
std::wstring check_new_dialogs(unsigned long pid, const std::set<HWND>& before);

// Snapshots ALL top-level windows of the process (any class).
std::set<HWND> snapshot_tsdev_windows(unsigned long pid);

// Polls for a new VISIBLE top-level window of `pid` whose class name contains
// `class_substr` (case-insensitive) and that is not in `before`. Returns the
// hwnd, or nullptr on timeout / process death. Bounded by timeout_ms unless
// timeouts_disabled.
HWND wait_for_tsdev_dialog(unsigned long pid, const std::wstring& class_substr,
                           const std::set<HWND>& before, bool timeouts_disabled,
                           unsigned long timeout_ms);
