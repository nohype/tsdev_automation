// test_preop_hash_classification.cpp
//
// Focused unit test for the pre-operation hash classification bug fix in
// doModify(): sha256FileCli() must distinguish a genuinely absent artifact
// (Sha256FileStatus::Absent) from an existing-but-unreadable one
// (Sha256FileStatus::Error). The original bug returned an empty string for
// ANY open/read/hash error, and doModify() treated empty as "no prior
// artifact" — silently converting an unreadable existing file into a new-file
// sentinel.
//
// This is a PURE UNIT TEST. It does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, does NOT construct TsDevLib, and does NOT need a
// 64 MB stack. It exercises the implemented sha256FileCli() helper (and the
// Sha256FileResult / Sha256FileStatus types) declared inside an anonymous
// namespace in src/tsdev_cli.cpp.
//
// Linkage mechanism (same as test_json_serialize.cpp / test_serve_framing.cpp):
//   sha256FileCli() lives inside an anonymous namespace in src/tsdev_cli.cpp
//   (internal linkage), so it is NOT accessible from a separate translation
//   unit. The only way to reach it is to #include the .cpp directly so the
//   test is part of the SAME translation unit. This pulls in tsdev_cli.cpp's
//   int main(); the test #defines TSDEV_CLI_TEST before the include so main()
//   is excluded by the TSDEV_CLI_TEST guard in src/tsdev_cli.cpp.
//
// Test scenarios (all use temp files in %TEMP%; no TradeStation paths):
//
//   (1) Absent file: a path that does not exist.
//       Expected: Sha256FileStatus::Absent, empty hash.
//
//   (2) Readable file: a regular file with known content.
//       Expected: Sha256FileStatus::Hashed, non-empty uppercase-hex hash
//       matching an independently computed SHA-256.
//
//   (3) Directory (not a regular file): an existing directory path.
//       Expected: Sha256FileStatus::Error, empty hash.
//       This is the "existing-but-unreadable" case: the path exists but
//       cannot be hashed as a regular file. doModify() must fail closed,
//       NOT treat it as absent.
//
//   (4) Unreadable file: a regular file whose ACL denies GENERIC_READ to the
//       current user. This exercises the CreateFileA failure path after
//       GetFileAttributesA succeeds (the path exists but cannot be opened).
//       Expected: Sha256FileStatus::Error, empty hash.
//       NOTE: This scenario is skipped (not failed) when the test process is
//       elevated (administrator), because an elevated process bypasses DACL
//       checks and can open the file regardless. The skip is reported on
//       stdout so the test remains honest. When non-elevated (the normal CI
//       -NoTs case), the scenario is exercised.
//
// Exit code: 0 only if ALL non-skipped checks pass; distinct nonzero codes
// per failure.
//
// Runs on a worker thread with a 64 MB stack only to match the project's test
// harness convention; the stack size is harmless for a unit test.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (sha256FileCli, Sha256FileResult, Sha256FileStatus, noPriorArtifactHash,
// etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// ---------------------------------------------------------------------------
// Independent SHA-256 helper (bcrypt) for cross-checking the Hashed result.
// Mirrors the project's bcrypt-everywhere style. Returns uppercase hex.
// ---------------------------------------------------------------------------
std::string independentSha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (status != 0) throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    struct AlgGuard {
        BCRYPT_ALG_HANDLE h;
        ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    } algGuard{hAlg};

    DWORD hashObjLen = 0, cbResult = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&hashObjLen),
                      sizeof(DWORD), &cbResult, 0);
    DWORD hashLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                      reinterpret_cast<PUCHAR>(&hashLen),
                      sizeof(DWORD), &cbResult, 0);

    std::vector<uint8_t> hashObj(hashObjLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjLen, nullptr, 0, 0);
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) BCryptDestroyHash(h); }
    } hashGuard{hHash};

    if (!data.empty()) {
        BCryptHashData(hHash, const_cast<PUCHAR>(data.data()),
                       static_cast<ULONG>(data.size()), 0);
    }

    std::vector<uint8_t> hash(hashLen);
    BCryptFinishHash(hHash, hash.data(), hashLen, 0);

    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(hash.size() * 2);
    for (uint8_t b : hash) {
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Temp directory RAII (same pattern as test_persistence_integrity.cpp).
// ---------------------------------------------------------------------------
struct TempDir {
    std::string path;
    bool ok = false;
    TempDir() {
        char tmp[MAX_PATH];
        DWORD len = GetTempPathA(MAX_PATH, tmp);
        if (len == 0 || len >= MAX_PATH) return;
        std::string base(tmp, len);
        char uniq[MAX_PATH];
        if (GetTempFileNameA(base.c_str(), "ph", 0, uniq) == 0) return;
        DeleteFileA(uniq);
        if (!CreateDirectoryA(uniq, nullptr)) return;
        path = uniq;
        ok = true;
    }
    ~TempDir() {
        if (!ok || path.empty()) return;
        std::string cmd = "cmd /c rmdir /s /q \"" + path + "\"";
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); }
        if (pi.hThread)  { CloseHandle(pi.hThread); }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string join(const std::string& dir, const std::string& leaf) {
    if (!dir.empty() && dir.back() != '\\') return dir + "\\" + leaf;
    return dir + leaf;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct G { HANDLE h; ~G() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } g{h};
    DWORD written = 0;
    if (!WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &written, nullptr)) return false;
    return written == bytes.size();
}

// Check whether the current process is elevated (administrator). When
// elevated, the unreadable-file scenario is skipped because an elevated
// process bypasses DACL read denials.
bool isProcessElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    TOKEN_ELEVATION elev = {};
    DWORD ret = 0;
    bool ok = GetTokenInformation(hToken, TokenElevation, &elev,
                                  sizeof(elev), &ret);
    CloseHandle(hToken);
    return ok && elev.TokenIsElevated != 0;
}

// Build a DACL that denies GENERIC_READ to the current user, then apply it
// to the file. Returns true on success.
bool denyReadToCurrentUser(const std::string& path) {
    // Get the current user's SID.
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    struct TokenGuard {
        HANDLE h;
        ~TokenGuard() { if (h) CloseHandle(h); }
    } tg{hToken};

    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return false;
    std::vector<uint8_t> tuBuf(needed);
    if (!GetTokenInformation(hToken, TokenUser, tuBuf.data(), needed, &needed))
        return false;
    TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(tuBuf.data());
    if (!tu->User.Sid) return false;

    // Build a security descriptor with a deny-ACE for GENERIC_READ.
    // SDDL form: D:(D;;GR;;;S-1-5-21-...) — but we need the SID string.
    // Use ConvertSidToStringSidA + BuildExplicitAccessWithName approach.
    LPSTR sidStr = nullptr;
    if (!ConvertSidToStringSidA(tu->User.Sid, &sidStr)) return false;
    struct SidStrGuard {
        LPSTR s;
        ~SidStrGuard() { if (s) LocalFree(s); }
    } ssg{sidStr};

    // SDDL: D = DACL; (D;;GR;;;SID) = deny GENERIC_READ to SID.
    std::string sddl = "D:(D;;GR;;;" + std::string(sidStr) + ")";

    PSECURITY_DESCRIPTOR pSD = nullptr;
    ULONG sdLen = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            sddl.c_str(), SDDL_REVISION_1, &pSD, &sdLen)) {
        return false;
    }
    struct SDGuard {
        PSECURITY_DESCRIPTOR p;
        ~SDGuard() { if (p) LocalFree(p); }
    } sdg{pSD};

    BOOL daclPresent = FALSE, daclDefaulted = FALSE;
    PACL pDacl = nullptr;
    if (!GetSecurityDescriptorDacl(pSD, &daclPresent, &pDacl, &daclDefaulted))
        return false;
    if (!daclPresent || !pDacl) return false;

    DWORD rc = SetNamedSecurityInfoA(
        const_cast<LPSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, pDacl, nullptr);
    return rc == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Test worker (runs on a 64 MB-stack thread per project convention).
// ---------------------------------------------------------------------------
DWORD WINAPI runTest(LPVOID param) {
    int& ec = *static_cast<int*>(param);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_preop_hash_classification: sha256FileCli() ===\n");

    TempDir tmp;
    if (!tmp.ok) {
        ec = fail("could not create temp dir", 1);
        return 0;
    }
    std::fprintf(stdout, "  temp dir: %s\n", tmp.path.c_str());

    // -------------------------------------------------------------------
    // (1) Absent file: path does not exist.
    //     Expected: Sha256FileStatus::Absent, empty hash.
    // -------------------------------------------------------------------
    {
        std::string absentPath = join(tmp.path, "nonexistent.ell");
        Sha256FileResult result = sha256FileCli(absentPath);
        std::fprintf(stdout,
                     "  [1] Absent: status=%d hash=\"%s\"\n",
                     static_cast<int>(result.status), result.hash.c_str());
        if (result.status != Sha256FileStatus::Absent) {
            ec = fail("[1] expected Absent status", 10);
            return 0;
        }
        if (!result.hash.empty()) {
            ec = fail("[1] expected empty hash for absent file", 11);
            return 0;
        }
        std::fprintf(stdout, "    PASS: absent file -> Absent, empty hash\n");
    }

    // -------------------------------------------------------------------
    // (2) Readable file: regular file with known content.
    //     Expected: Sha256FileStatus::Hashed, non-empty uppercase-hex hash
    //     matching the independently computed SHA-256.
    // -------------------------------------------------------------------
    {
        std::vector<uint8_t> content;
        for (int i = 0; i < 256; ++i)
            content.push_back(static_cast<uint8_t>(i));
        std::string readablePath = join(tmp.path, "readable.ell");
        if (!writeFile(readablePath, content)) {
            ec = fail("[2] failed to write readable fixture", 20);
            return 0;
        }

        Sha256FileResult result = sha256FileCli(readablePath);
        std::string expected = independentSha256Hex(content);
        std::fprintf(stdout,
                     "  [2] Readable: status=%d hash=\"%s\" expected=\"%s\"\n",
                     static_cast<int>(result.status),
                     result.hash.c_str(), expected.c_str());
        if (result.status != Sha256FileStatus::Hashed) {
            ec = fail("[2] expected Hashed status", 21);
            return 0;
        }
        if (result.hash.empty()) {
            ec = fail("[2] expected non-empty hash for readable file", 22);
            return 0;
        }
        if (result.hash != expected) {
            ec = fail("[2] hash mismatch vs independent SHA-256", 23);
            return 0;
        }
        // Verify uppercase hex (all chars 0-9 or A-F).
        for (char c : result.hash) {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
                ec = fail("[2] hash contains non-uppercase-hex chars", 24);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: readable file -> Hashed, correct hash\n");
    }

    // -------------------------------------------------------------------
    // (3) Directory (not a regular file): existing directory path.
    //     Expected: Sha256FileStatus::Error, empty hash.
    //     This is the "existing-but-unreadable" case: the path exists but
    //     cannot be hashed as a regular file.
    // -------------------------------------------------------------------
    {
        std::string dirPath = join(tmp.path, "subdir");
        if (!CreateDirectoryA(dirPath.c_str(), nullptr)) {
            ec = fail("[3] failed to create subdirectory", 30);
            return 0;
        }

        Sha256FileResult result = sha256FileCli(dirPath);
        std::fprintf(stdout,
                     "  [3] Directory: status=%d hash=\"%s\"\n",
                     static_cast<int>(result.status), result.hash.c_str());
        if (result.status != Sha256FileStatus::Error) {
            ec = fail("[3] expected Error status for directory", 31);
            return 0;
        }
        if (!result.hash.empty()) {
            ec = fail("[3] expected empty hash for directory", 32);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: directory -> Error (existing-but-unreadable)\n");
    }

    // -------------------------------------------------------------------
    // (4) Unreadable file: regular file whose DACL denies GENERIC_READ.
    //     Expected: Sha256FileStatus::Error, empty hash.
    //     SKIPPED when the process is elevated (admin bypasses DACL denials).
    // -------------------------------------------------------------------
    {
        if (isProcessElevated()) {
            std::fprintf(stdout,
                "  [4] Unreadable: SKIPPED (process is elevated; admin"
                " bypasses DACL read denials)\n");
        } else {
            std::vector<uint8_t> content;
            for (int i = 0; i < 64; ++i)
                content.push_back(static_cast<uint8_t>(i * 7));
            std::string unreadablePath = join(tmp.path, "unreadable.ell");
            if (!writeFile(unreadablePath, content)) {
                ec = fail("[4] failed to write unreadable fixture", 40);
                return 0;
            }
            if (!denyReadToCurrentUser(unreadablePath)) {
                std::fprintf(stdout,
                    "  [4] Unreadable: SKIPPED (could not apply deny-read ACL;"
                    " non-fatal -- directory case [3] already covers"
                    " existing-but-unreadable)\n");
            } else {
                Sha256FileResult result = sha256FileCli(unreadablePath);
                std::fprintf(stdout,
                             "  [4] Unreadable: status=%d hash=\"%s\"\n",
                             static_cast<int>(result.status),
                             result.hash.c_str());
                if (result.status != Sha256FileStatus::Error) {
                    ec = fail("[4] expected Error status for unreadable file",
                              41);
                    return 0;
                }
                if (!result.hash.empty()) {
                    ec = fail("[4] expected empty hash for unreadable file",
                              42);
                    return 0;
                }
                std::fprintf(stdout,
                    "    PASS: unreadable file -> Error"
                    " (existing-but-unreadable)\n");
            }
        }
    }

    // -------------------------------------------------------------------
    // (5) Cross-check: noPriorArtifactHash() returns a non-empty, non-empty-
    //     buffer SHA-256 sentinel (the value doModify uses for Absent). This
    //     confirms the absent-file path produces a real, non-empty sentinel
    //     that the validator does NOT map to InvalidInput (which requires an
    //     EMPTY pre-hash).
    // -------------------------------------------------------------------
    {
        const std::string& sentinel = noPriorArtifactHash();
        std::fprintf(stdout,
                     "  [5] noPriorArtifactHash: \"%s\"\n",
                     sentinel.c_str());
        if (sentinel.empty()) {
            ec = fail("[5] noPriorArtifactHash returned empty", 50);
            return 0;
        }
        // The well-known SHA-256 of zero bytes (empty buffer).
        const char* expectedEmptySha256 =
            "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
        if (sentinel != expectedEmptySha256) {
            // The fallback sentinel is acceptable (bcrypt failure); just log.
            std::fprintf(stdout,
                "    NOTE: noPriorArtifactHash != empty-buffer SHA-256"
                " (fallback sentinel in use; acceptable)\n");
        } else {
            std::fprintf(stdout,
                "    PASS: noPriorArtifactHash == empty-buffer SHA-256\n");
        }
    }

    if (ec == 0) {
        std::fprintf(stdout,
                     "\n=== ALL CHECKS PASSED ===\n"
                     "  - Absent file -> Absent, empty hash\n"
                     "  - Readable file -> Hashed, correct hash\n"
                     "  - Directory -> Error (existing-but-unreadable)\n"
                     "  - Unreadable file -> Error (when non-elevated)\n"
                     "  - noPriorArtifactHash non-empty sentinel\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
// ---------------------------------------------------------------------------
DWORD WINAPI threadProc(LPVOID param) {
    int* ec = static_cast<int*>(param);
    __try {
        runTest(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        *ec = 99;
    }
    return 0;
}

} // namespace

int main() {
    std::fprintf(stdout,
                 "=== test_preop_hash_classification: pre-op hash"
                 " absent vs unreadable/error ===\n");

    int ec = 0;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, &ec, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::fprintf(stdout,
                 "\n=== test_preop_hash_classification complete"
                 " (exit %d) ===\n", ec);
    if (ec == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return ec;
}
