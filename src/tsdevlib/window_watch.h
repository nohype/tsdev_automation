#pragma once
#include <string>

// Returns the TSDev main frame window (class TSDEV.EXE TRADESTATION) for the
// given pid, or nullptr.
void* find_main_frame(unsigned long pid);

// Polls the main frame title until it contains `needle`. timeout_ms == 0
// means wait forever. Returns false on timeout or if the process dies.
bool wait_for_title(unsigned long pid, const std::wstring& needle, bool timeouts_disabled, unsigned long timeout_ms);
