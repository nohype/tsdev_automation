#pragma once
#include <windows.h>
#include <set>
#include <string>

std::set<HWND> snapshot_tsdev_dialogs(unsigned long pid);
bool crash_report_running();

// Dismisses (WM_CLOSE) dialogs that appeared since the snapshot and returns
// their combined text; empty means no new dialog.
std::wstring check_new_dialogs(unsigned long pid, const std::set<HWND>& before);
