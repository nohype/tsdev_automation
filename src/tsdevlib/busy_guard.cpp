#include "busy_guard.h"

BusyGuard BusyGuard::acquire() {
    BusyGuard guard;
    guard.handle_ = CreateMutexW(nullptr, TRUE, L"Local\\TSDevAutomation.Busy");
    if (guard.handle_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(guard.handle_);
        guard.handle_ = nullptr;
    }
    return guard;
}

BusyGuard::~BusyGuard() {
    if (handle_) {
        ReleaseMutex(handle_);
        CloseHandle(handle_);
    }
}
