#pragma once
#include <string>

// After TSDev finishes a compile, its Output error list (a SysListView32
// control) holds one summary row " N error(s), M warning(s) " plus one row
// per error. Reads the summary count from the TSDev process's list views.
// Returns true when a summary row was found; sets errors to its count.
// error_rows (optional out) receives the non-summary rows' first-column text
// joined with " | " (the compile error messages).
bool read_compile_error_summary(unsigned long pid, int& errors, std::wstring& error_rows);
