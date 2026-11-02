// test_rc01_com_balance.cpp
//
// RC-01 regression test: the two WHServer bind sites must perform ZERO COM
// init/uninit calls of their own.
//
// Governing rule (src/com_apartment_guard.cpp:24-28): each successful
// CoInitializeEx INCLUDING S_FALSE must be balanced by exactly one
// CoUninitialize on the same thread. Both bind sites run on the TsDevLib
// guard-owned STA, so the old "confirm" CoInitializeEx (which returns
// S_FALSE and was never balanced) leaked one COM balance per bind. The fix
// removes both confirm calls outright: with zero inits, zero uninits are
// owed and the rule holds vacuously; the guard remains the sole owner.
//
// Checks:
//   1-2. Mock-ComApi balance rule: a ComApartmentGuard built on a mock
//        ComApi returning S_OK (check 1) or S_FALSE (check 2) records
//        exactly one init call and exactly one uninit call. These pin the
//        governing rule (green before and after the fix).
//   3-6. Source scan: the WhComNameSource::bind body in
//        src/whserver_names.cpp (checks 3-4) and the
//        ComRecycleAttach::attachAndForceShutDown body in
//        src/whserver_recycle.cpp (checks 5-6) contain zero
//        CoInitializeEx( and zero CoUninitialize( calls (line comments
//        stripped first, so prose references do not trip the scan). These
//        FAIL before the RC-01 fix (RED) and PASS after it (GREEN).
//
// The test locates src/ relative to the executable's location
// (build\Release\ -> repo root, i.e. ..\..\src\<file>). An optional
// argv[1] overrides the src directory.
//
// Pure unit test: no orlib20, no TradeStation, no real COM calls (the mock
// ComApi never touches the test process's apartment). Runs under -NoTs.
//
// Exit codes:
//   0  = PASS -- rule holds and both bind bodies are COM-init-free
//   1x = FAIL -- a balance/scan check failed (10 + check number)
//   2  = ERROR -- a source file could not be opened/read/parsed

#include "../src/com_apartment_guard.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// --- Mock COM API (never touches the real apartment) -----------------------

struct MockState {
    HRESULT nextInitResult = S_OK;
    int     initCalls      = 0;
    int     uninitCalls    = 0;
};

MockState g_mock;

HRESULT WINAPI mockCoInitializeEx(LPVOID /*pvReserved*/, DWORD /*dwCoInit*/) {
    ++g_mock.initCalls;
    return g_mock.nextInitResult;
}

void WINAPI mockCoUninitialize() {
    ++g_mock.uninitCalls;
}

ComApi mockApi() {
    return ComApi{&mockCoInitializeEx, &mockCoUninitialize};
}

void resetMock(HRESULT hr) {
    g_mock = MockState{};
    g_mock.nextInitResult = hr;
}

// Check 1: S_OK success balances exactly once.
int checkGuardBalanceSOk() {
    std::fprintf(stdout, "  [check 1] guard balance S_OK: 1 init / 1 uninit\n");
    resetMock(S_OK);
    {
        ComApartmentGuard guard(mockApi());
        if (!guard.initialized()) {
            return fail("S_OK: guard should be initialized", 11);
        }
    }
    if (g_mock.initCalls != 1 || g_mock.uninitCalls != 1) {
        std::fprintf(stderr, "  initCalls=%d uninitCalls=%d (expected 1/1)\n",
                      g_mock.initCalls, g_mock.uninitCalls);
        return fail("S_OK: expected exactly 1 init + 1 uninit", 11);
    }
    std::fprintf(stdout, "    PASS\n");
    return 0;
}

// Check 2: S_FALSE success balances exactly once (S_FALSE still owes one).
int checkGuardBalanceSFalse() {
    std::fprintf(stdout,
                 "  [check 2] guard balance S_FALSE: 1 init / 1 uninit\n");
    resetMock(S_FALSE);
    {
        ComApartmentGuard guard(mockApi());
        if (!guard.initialized()) {
            return fail("S_FALSE: guard should be initialized", 12);
        }
    }
    if (g_mock.initCalls != 1 || g_mock.uninitCalls != 1) {
        std::fprintf(stderr, "  initCalls=%d uninitCalls=%d (expected 1/1)\n",
                      g_mock.initCalls, g_mock.uninitCalls);
        return fail("S_FALSE: expected exactly 1 init + 1 uninit", 12);
    }
    std::fprintf(stdout, "    PASS\n");
    return 0;
}

// --- Source scan helpers ----------------------------------------------------

std::string resolveSrcDir(int argc, char** argv) {
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        return argv[1];
    }
    wchar_t exePathW[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return "src";
    }
    char exePathA[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, exePathW, -1, exePathA, MAX_PATH,
                        nullptr, nullptr);
    std::string dir(exePathA);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
    }
    // build\Release\ -> repo root -> src\.
    return dir + "\\..\\..\\src";
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return !out.empty();
}

// Extract the function body: lines after the first line containing
// startMarker, up to (excluding) the first subsequent line that is exactly
// "}". Returns false when the shape is not found.
bool extractBody(const std::string& content, const std::string& startMarker,
                 std::vector<std::string>& bodyLines) {
    bodyLines.clear();
    std::istringstream iss(content);
    std::string line;
    bool inBody = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!inBody) {
            if (line.find(startMarker) != std::string::npos) {
                inBody = true;
            }
            continue;
        }
        if (line == "}") {
            return true;
        }
        bodyLines.push_back(line);
    }
    bodyLines.clear();
    return false;
}

// True when the (comment-stripped) line calls token: token followed by
// optional spaces/tabs then '('.
bool lineCalls(const std::string& line, const std::string& token) {
    size_t pos = line.find(token);
    while (pos != std::string::npos) {
        size_t i = pos + token.size();
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i < line.size() && line[i] == '(') {
            return true;
        }
        pos = line.find(token, pos + 1);
    }
    return false;
}

// Assert a bind body contains zero calls to token. Returns 0 on success.
int checkBodyHasNoCall(const std::vector<std::string>& bodyLines,
                       const std::string& what, const std::string& token,
                       int checkNo) {
    std::fprintf(stdout, "  [check %d] %s: zero %s( calls\n", checkNo,
                 what.c_str(), token.c_str());
    for (size_t n = 0; n < bodyLines.size(); ++n) {
        std::string line = bodyLines[n];
        const size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (lineCalls(line, token)) {
            std::fprintf(stderr, "  body line %u calls %s: %s\n",
                         static_cast<unsigned>(n + 1), token.c_str(),
                         bodyLines[n].c_str());
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "%s body still calls %s (unbalanced RC-01)",
                          what.c_str(), token.c_str());
            return fail(msg, 10 + checkNo);
        }
    }
    std::fprintf(stdout, "    PASS\n");
    return 0;
}

int runScanChecks(const std::string& srcDir) {
    std::string namesPath = srcDir + "\\whserver_names.cpp";
    std::string recyclePath = srcDir + "\\whserver_recycle.cpp";
    std::string names, recycle;
    if (!readFile(namesPath, names)) {
        std::fprintf(stderr, "ERROR: could not read %s\n", namesPath.c_str());
        return 2;
    }
    if (!readFile(recyclePath, recycle)) {
        std::fprintf(stderr, "ERROR: could not read %s\n",
                     recyclePath.c_str());
        return 2;
    }

    std::vector<std::string> bindBody, attachBody;
    if (!extractBody(names, "WhComNameSource::bind", bindBody)) {
        std::fprintf(stderr, "ERROR: WhComNameSource::bind body not found\n");
        return 2;
    }
    if (!extractBody(recycle, "ComRecycleAttach::attachAndForceShutDown",
                     attachBody)) {
        std::fprintf(stderr,
                     "ERROR: attachAndForceShutDown body not found\n");
        return 2;
    }

    int ec = 0;
    if ((ec = checkBodyHasNoCall(bindBody, "WhComNameSource::bind",
                                 "CoInitializeEx", 3)) != 0) {
        return ec;
    }
    if ((ec = checkBodyHasNoCall(bindBody, "WhComNameSource::bind",
                                 "CoUninitialize", 4)) != 0) {
        return ec;
    }
    if ((ec = checkBodyHasNoCall(attachBody,
                                 "ComRecycleAttach::attachAndForceShutDown",
                                 "CoInitializeEx", 5)) != 0) {
        return ec;
    }
    if ((ec = checkBodyHasNoCall(attachBody,
                                 "ComRecycleAttach::attachAndForceShutDown",
                                 "CoUninitialize", 6)) != 0) {
        return ec;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_rc01_com_balance: RC-01 bind-site COM balance ===\n");

    int ec = 0;
    if ((ec = checkGuardBalanceSOk()) != 0) {
        return ec;
    }
    if ((ec = checkGuardBalanceSFalse()) != 0) {
        return ec;
    }
    if ((ec = runScanChecks(resolveSrcDir(argc, argv))) != 0) {
        return ec;
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}
