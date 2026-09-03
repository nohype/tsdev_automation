#include "elevation.h"
#include <windows.h>

bool is_elevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
            elevated = elevation.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}
