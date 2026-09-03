#pragma once

enum class Exit : int {
    Ok = 0,
    NotFound = 1,
    Usage = 2,
    NotElevated = 3,
    LaunchFailed = 4,
    PipeFailed = 5,
    Timeout = 6,
    ErrorDialog = 7,
    Crashed = 8,
    Busy = 9,
    Unexpected = 10,
};
