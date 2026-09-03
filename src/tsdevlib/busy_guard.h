#pragma once
#include <windows.h>

class BusyGuard {
public:
    static BusyGuard acquire();
    ~BusyGuard();
    BusyGuard(BusyGuard&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    BusyGuard(const BusyGuard&) = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
    explicit operator bool() const { return handle_ != nullptr; }

private:
    BusyGuard() = default;
    HANDLE handle_ = nullptr;
};
