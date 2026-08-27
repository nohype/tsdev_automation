// test_close_warehouse.cpp
//
// Pure unit test for the explicit warehouse-close status seam. The production
// close export has a void return type, so TsDevLib reports whether the
// SEH-protected call completed. This test injects a successful function and an
// SEH-raising function; it does not construct TsDevLib, load orlib20.dll, or
// touch a TradeStation installation.

#include "../src/tsdev_lib_test_hooks.h"

#include <cstdio>
#include <windows.h>

namespace {

int g_lastParam = -1;

void __stdcall closeSucceeds(int param) {
    g_lastParam = param;
}

void __stdcall closeRaises(int) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE,
                   0, nullptr);
}

int fail(const char* message, int code) {
    std::fprintf(stderr, "[FAIL] %s (code=%d)\n", message, code);
    return code;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_close_warehouse: explicit close status (unit) ===\n");

    g_lastParam = -1;
    if (!tsdev::invokeCloseWarehouseForTest(&closeSucceeds)) {
        return fail("completed close call should return true", 10);
    }
    if (g_lastParam != 0) {
        return fail("close call should receive production parameter 0", 11);
    }
    std::fprintf(stdout,
                 "  [1] PASS: completed close call reports true and receives 0\n");

    if (tsdev::invokeCloseWarehouseForTest(&closeRaises)) {
        return fail("SEH-raising close call should return false", 20);
    }
    std::fprintf(stdout,
                 "  [2] PASS: SEH-raising close call reports false\n");

    std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    return 0;
}
