#pragma once
#include <string>

// Connects to the TSDev control pipe and writes one UTF-16LE NUL-terminated
// payload. The server never replies. Returns false and fills `error` on
// failure. timeout_ms == 0 (or timeouts disabled) means: retry forever.
bool pipe_send(const std::wstring& payload, bool timeouts_disabled, unsigned long timeout_ms, std::wstring& error);
