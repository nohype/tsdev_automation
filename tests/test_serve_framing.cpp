// test_serve_framing.cpp
//
// Unit test for TVS-K.4: named pipe framing helpers.
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll, does NOT touch the
// TradeStation installation, and does NOT need a 64 MB stack for orlib20. It
// exercises the implemented framing I/O helpers in src/tsdev_cli.cpp:
//   - bool writeFramed(HANDLE pipe, const std::string& payload)
//   - bool readFramed(HANDLE pipe, std::string& out)
// and the TVS C stdin batch-control framed-manifest reader:
//   - bool readBatchControlManifest(FILE* input, std::string& manifest,
//         std::string& error)
//
// The framing format (per the protocol spec, docs/workstream_serve_plan.md
// §2 "Message framing") is:
//   [4 bytes: payload length N, little-endian uint32]
//   [N bytes: UTF-8 JSON payload]
// N must be in [0, 67108864] (64 MiB cap). Messages outside this range are
// rejected (readFramed returns false). A zero-length payload is accepted by
// the framing layer (the JSON parser would reject it; that is not the
// framing layer's concern).
//
// The helpers operate on Windows HANDLEs (from CreatePipe / CreateNamedPipeW),
// NOT on FILE* or std::istream. The test uses CreatePipe() to obtain a pair
// of anonymous pipe handles (one read end, one write end) and round-trips
// payloads through them. This is self-contained — no external pipe server is
// needed.
//
// It runs on a worker thread with a 64 MB stack only to match the project's
// test harness convention (all tests run on a large-stack worker thread); the
// stack size is harmless for a unit test.
//
// Linkage mechanism:
//   writeFramed() and readFramed() live inside an anonymous namespace in
//   src/tsdev_cli.cpp (internal linkage), so they are NOT accessible from a
//   separate translation unit. The only way to reach them is to #include the
//   .cpp directly so the test is part of the SAME translation unit. This
//   pulls in tsdev_cli.cpp's int main(); the test #defines TSDEV_CLI_TEST
//   before the include so main() is excluded by the TSDEV_CLI_TEST guard in
//   src/tsdev_cli.cpp.
//
// The checks below exercise the implemented framing helpers for normal,
// boundary, and broken-pipe cases.
//
// Checks:
//   1. Round-trip: writeFramed(write, "Hello World") then
//      readFramed(read, out) -> out == "Hello World".
//   2. Empty payload: writeFramed(write, "") then readFramed(read, out) ->
//      out == "" (framing layer accepts N=0).
//   3. JSON payload round-trip: writeFramed(write,
//      "{\"op\":\"insert\",\"type\":\"strategy\"}") then readFramed ->
//      out == the input JSON string.
//   4. Length prefix is little-endian: after writeFramed(write, "AB"),
//      manually read 4 bytes from the pipe and assert they are
//      [2, 0, 0, 0] (little-endian uint32 == 2).
//   5. Oversized payload rejected: manually write a bad length prefix
//      (0xFFFFFFFF, which is > 64 MiB) to the pipe, then call readFramed
//      and assert it returns false. (Avoids allocating 64 MB+.)
//   6. Broken pipe handling: close the write end of a pipe, then call
//      readFramed on the read end and assert it returns false (graceful
//      failure, not crash).
//   7. ConnectNamedPipe classification: a successful call and
//      ERROR_PIPE_CONNECTED remain normal, while an unexpected failure maps
//      to a nonzero infrastructure status.
//   8. (TVS C) readBatchControlManifest accepts the framed wire form
//      "TSDEV_BATCH_CONTROL/1 <decimal UTF-8 byte length>\n" + exactly
//      <length> manifest bytes — empty, nonempty UTF-8 manifests whose
//      source paths contain spaces, and a declared length of exactly the
//      64 MiB cap — and leaves the following decision lines unread on the
//      stream.
//   9. (TVS C) malformed headers rejected: EOF/truncated header, wrong
//      version, missing/empty/nondecimal/signed/overflow length, trailing
//      junk after the length, declared length > 64 MiB, and a short
//      payload all return false with a nonempty diagnostic.
//  10. (TVS C) framed manifest then decision lines on the SAME stream:
//      readBatchControlManifest consumes exactly the declared bytes, then
//      readBatchControlDecision reads "continue\n"/"cancel\n" decisions.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
// (main() is wrapped in the #ifndef TSDEV_CLI_TEST guard in tsdev_cli.cpp.)
#define TSDEV_CLI_TEST

// Include the CLI source directly so anonymous-namespace internals
// (writeFramed, readFramed, CliResult, ordered_json, etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// 64 MiB framing cap (per protocol spec).
constexpr uint32_t kFrameCap = 67108864u;

// Helper (TVS C checks): returns a binary-mode FILE* positioned at the start
// of an anonymous pipe pre-filled with `bytes`; once those bytes are consumed
// the reader sees EOF. This feeds the stdin-shaped control protocol exact
// byte sequences — including immediate EOF — without touching the real stdin
// or the filesystem. The FILE* owns the pipe read handle; fclose() releases
// it. Returns nullptr on setup failure.
FILE* fileFromBytes(const std::string& bytes) {
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return nullptr;
    if (!bytes.empty()) {
        DWORD wrote = 0;
        if (!WriteFile(writeEnd, bytes.data(),
                       static_cast<DWORD>(bytes.size()), &wrote, nullptr) ||
            wrote != bytes.size()) {
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return nullptr;
        }
    }
    // Closing the write end makes the reader see EOF once the buffered
    // bytes are drained.
    CloseHandle(writeEnd);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(readEnd),
                             _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(readEnd);
        return nullptr;
    }
    FILE* f = _fdopen(fd, "rb");
    if (!f) {
        _close(fd);  // _close releases the underlying OS handle
        return nullptr;
    }
    return f;
}

// Helper (TVS C checks): drains the remainder of `f` into a string — used to
// assert the byte-exact manifest/decision boundary on the input stream.
std::string drainFile(FILE* f) {
    std::string out;
    char buf[256];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    return out;
}

// Helper (TVS C checks): writes `bytes` to a unique temp file and returns a
// binary-mode read/write FILE* positioned at its start, with `path` (caller
// buffer of at least MAX_PATH) holding the file path for DeleteFileA
// cleanup. Needed for inputs too large for an anonymous pipe (a multi-MiB
// write with no concurrent reader would deadlock on the pipe buffer).
FILE* diskFileFromBytes(const std::string& bytes, char* path) {
    char dir[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, dir)) return nullptr;
    if (!GetTempFileNameA(dir, "tbc", 0, path)) return nullptr;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb+") != 0 || f == nullptr) {
        DeleteFileA(path);
        return nullptr;
    }
    if (!bytes.empty() &&
        std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fclose(f);
        DeleteFileA(path);
        return nullptr;
    }
    std::rewind(f);
    return f;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_serve_framing: writeFramed/readFramed (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: Round-trip of a plain string.
    //   writeFramed(write, "Hello World") then readFramed(read, out).
    // Expected: readFramed returns true, out == "Hello World".
    // ------------------------------------------------------------------
    {
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            ec = fail("check1: CreatePipe failed", 10);
            return 0;
        }

        if (!writeFramed(writeEnd, "Hello World")) {
            ec = fail("check1: writeFramed returned false", 11);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::string out;
        if (!readFramed(readEnd, out)) {
            ec = fail("check1: readFramed returned false", 12);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::fprintf(stdout, "  [1] round-trip: out=\"%s\"\n", out.c_str());
        if (out != "Hello World") {
            ec = fail("check1: out != \"Hello World\"", 13);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        std::fprintf(stdout, "    PASS: plain string round-trip OK\n");
    }

    // ------------------------------------------------------------------
    // Check 2: Empty payload (N=0).
    //   writeFramed(write, "") then readFramed(read, out).
    // Expected: readFramed returns true, out == "" (framing layer accepts
    //   zero-length payloads; JSON parsing is a separate concern).
    // ------------------------------------------------------------------
    {
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            ec = fail("check2: CreatePipe failed", 20);
            return 0;
        }

        if (!writeFramed(writeEnd, "")) {
            ec = fail("check2: writeFramed(\"\") returned false", 21);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::string out = "SENTINEL"; // ensure readFramed overwrites it
        if (!readFramed(readEnd, out)) {
            ec = fail("check2: readFramed returned false for empty payload",
                      22);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::fprintf(stdout, "  [2] empty payload: out=\"%s\" len=%zu\n",
                     out.c_str(), out.size());
        if (out != "") {
            ec = fail("check2: out != \"\" (empty payload)", 23);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        std::fprintf(stdout, "    PASS: empty payload round-trip OK\n");
    }

    // ------------------------------------------------------------------
    // Check 3: JSON payload round-trip.
    //   writeFramed(write, "{\"op\":\"insert\",\"type\":\"strategy\"}")
    //   then readFramed(read, out).
    // Expected: out == the input JSON string (byte-for-byte).
    // ------------------------------------------------------------------
    {
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            ec = fail("check3: CreatePipe failed", 30);
            return 0;
        }

        const std::string json = "{\"op\":\"insert\",\"type\":\"strategy\"}";
        if (!writeFramed(writeEnd, json)) {
            ec = fail("check3: writeFramed returned false", 31);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::string out;
        if (!readFramed(readEnd, out)) {
            ec = fail("check3: readFramed returned false", 32);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::fprintf(stdout, "  [3] json payload: out=\"%s\"\n", out.c_str());
        if (out != json) {
            ec = fail("check3: out != input JSON string", 33);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        std::fprintf(stdout, "    PASS: JSON payload round-trip OK\n");
    }

    // ------------------------------------------------------------------
    // Check 4: Length prefix is little-endian.
    //   After writeFramed(write, "AB"), manually ReadFile 4 bytes from the
    //   pipe and assert they are [2, 0, 0, 0] (little-endian uint32 == 2).
    //   Then read the 2 payload bytes and assert they are "AB" (sanity).
    // ------------------------------------------------------------------
    {
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            ec = fail("check4: CreatePipe failed", 40);
            return 0;
        }

        if (!writeFramed(writeEnd, "AB")) {
            ec = fail("check4: writeFramed(\"AB\") returned false", 41);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        unsigned char prefix[4] = {0, 0, 0, 0};
        DWORD got = 0;
        if (!ReadFile(readEnd, prefix, 4, &got, nullptr) || got != 4) {
            ec = fail("check4: ReadFile of 4-byte prefix failed", 42);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::fprintf(stdout,
                     "  [4] length prefix bytes: [%u, %u, %u, %u]\n",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
        if (prefix[0] != 2 || prefix[1] != 0 ||
            prefix[2] != 0 || prefix[3] != 0) {
            ec = fail("check4: prefix != [2,0,0,0] (little-endian uint32)",
                      43);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        // Sanity: payload bytes should be "AB".
        char payload[2] = {0, 0};
        got = 0;
        if (!ReadFile(readEnd, payload, 2, &got, nullptr) || got != 2) {
            ec = fail("check4: ReadFile of 2-byte payload failed", 44);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }
        if (payload[0] != 'A' || payload[1] != 'B') {
            ec = fail("check4: payload != \"AB\"", 45);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        std::fprintf(stdout,
                     "    PASS: length prefix is little-endian uint32\n");
    }

    // ------------------------------------------------------------------
    // Check 5: Oversized payload rejected by readFramed.
    //   Manually write a bad length prefix (0xFFFFFFFF, which is > 64 MiB)
    //   to the pipe, then call readFramed and assert it returns false.
    //   This exercises the rejection path without allocating 64 MB+.
    // ------------------------------------------------------------------
    {
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            ec = fail("check5: CreatePipe failed", 50);
            return 0;
        }

        // 0xFFFFFFFF is well above the 64 MiB cap (67108864).
        const uint32_t badLen = 0xFFFFFFFFu;
        DWORD wrote = 0;
        if (!WriteFile(writeEnd, &badLen, 4, &wrote, nullptr) || wrote != 4) {
            ec = fail("check5: WriteFile of bad length prefix failed", 51);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::string out;
        if (readFramed(readEnd, out)) {
            ec = fail("check5: readFramed should return false for >64MiB len",
                      52);
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return 0;
        }

        std::fprintf(stdout,
                     "  [5] oversized prefix 0x%X (>cap %u): rejected\n",
                     badLen, kFrameCap);
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        std::fprintf(stdout,
                     "    PASS: oversized payload rejected by readFramed\n");
    }

    // ------------------------------------------------------------------
    // Check 6: Broken pipe handling.
    //   Close the write end of a pipe, then call readFramed on the read
    //   end. Assert it returns false (graceful failure, not a crash).
    //   With no data written and the writer closed, ReadFile on the read
    //   end returns 0 bytes (EOF) -> readFramed must report failure.
    // ------------------------------------------------------------------
    {
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            ec = fail("check6: CreatePipe failed", 60);
            return 0;
        }

        // Close the write end without writing anything -> read end sees EOF.
        CloseHandle(writeEnd);
        writeEnd = nullptr;

        std::string out;
        if (readFramed(readEnd, out)) {
            ec = fail("check6: readFramed should return false on broken pipe",
                      61);
            CloseHandle(readEnd);
            return 0;
        }

        std::fprintf(stdout,
                     "  [6] broken pipe (writer closed, no data): rejected\n");
        CloseHandle(readEnd);
        std::fprintf(stdout,
                     "    PASS: broken pipe handled gracefully (no crash)\n");
    }

    // ------------------------------------------------------------------
    // Check 7: ConnectNamedPipe failure classification.
    //   A successful connect and the documented ERROR_PIPE_CONNECTED race
    //   are normal; an unexpected failed connect must produce a nonzero
    //   infrastructure status. The serve worker uses this same helper before
    //   it leaves its outer loop.
    // ------------------------------------------------------------------
    if (serveConnectExitCode(TRUE, ERROR_ACCESS_DENIED) != 0) {
        ec = fail("check7: successful ConnectNamedPipe must remain normal", 70);
        return 0;
    }
    if (serveConnectExitCode(FALSE, ERROR_PIPE_CONNECTED) != 0) {
        ec = fail("check7: ERROR_PIPE_CONNECTED must remain normal", 71);
        return 0;
    }
    if (serveConnectExitCode(FALSE, ERROR_ACCESS_DENIED) == 0) {
        ec = fail("check7: unexpected ConnectNamedPipe failure must be nonzero", 72);
        return 0;
    }
    std::fprintf(stdout,
                 "  [7] ConnectNamedPipe failure classification: unexpected failure is nonzero\n");
    std::fprintf(stdout,
                 "    PASS: normal connect/disconnect semantics preserved and unexpected failure fails closed\n");

    // ------------------------------------------------------------------
    // Check 8 (TVS C): readBatchControlManifest accepts the framed wire
    //   form. stdin bytes begin with the ASCII header
    //     TSDEV_BATCH_CONTROL/1 <decimal UTF-8 manifest byte length>\n
    //   followed by EXACTLY <length> manifest bytes; the stream then
    //   continues with decision lines (left unread by this reader).
    //   (a) "TSDEV_BATCH_CONTROL/1 0\ncontinue\n" -> true, manifest is
    //       empty, and the stream remainder is "continue\n".
    //   (b) a nonempty UTF-8 manifest whose <source_file> path contains
    //       spaces -> true, manifest byte-identical to the declared
    //       payload, remainder "continue\ncancel\n".
    // ------------------------------------------------------------------
    {
        FILE* f = fileFromBytes("TSDEV_BATCH_CONTROL/1 0\ncontinue\n");
        if (!f) {
            ec = fail("check8a: pipe setup failed", 80);
            return 0;
        }
        std::string manifest = "SENTINEL";
        std::string err;
        if (!readBatchControlManifest(f, manifest, err)) {
            std::fclose(f);
            ec = fail("check8a: valid empty manifest rejected", 81);
            return 0;
        }
        if (!manifest.empty()) {
            std::fclose(f);
            ec = fail("check8a: manifest != \"\" for declared length 0", 82);
            return 0;
        }
        std::string rest = drainFile(f);
        std::fclose(f);
        if (rest != "continue\n") {
            ec = fail("check8a: decision lines not left on the stream", 83);
            return 0;
        }
        std::fprintf(stdout,
                     "  [8a] framed empty manifest accepted; remainder "
                     "\"continue\\n\"\n");

        // Nonempty UTF-8 manifest (the \xC3\xB1 pair is 'ñ', a multibyte
        // UTF-8 sequence — the declared length is a BYTE count) whose
        // source path contains spaces.
        const std::string payload =
            "strategy MyStrat C:\\My Dir\\src file.el\n"
            "function Ni\xC3\xB1oFn C:\\src\\fn.el\n";
        const std::string wire = "TSDEV_BATCH_CONTROL/1 " +
            std::to_string(payload.size()) + "\n" + payload +
            "continue\ncancel\n";
        f = fileFromBytes(wire);
        if (!f) {
            ec = fail("check8b: pipe setup failed", 84);
            return 0;
        }
        manifest.clear();
        err.clear();
        if (!readBatchControlManifest(f, manifest, err)) {
            std::string msg = "check8b: valid UTF-8 manifest rejected: " + err;
            std::fclose(f);
            ec = fail(msg.c_str(), 85);
            return 0;
        }
        if (manifest != payload) {
            std::fclose(f);
            ec = fail("check8b: manifest not byte-identical to payload",
                      86);
            return 0;
        }
        rest = drainFile(f);
        std::fclose(f);
        if (rest != "continue\ncancel\n") {
            ec = fail("check8b: decision lines not left on the stream", 87);
            return 0;
        }
        std::fprintf(stdout,
                     "  [8b] framed UTF-8/path-space manifest (%zu bytes) "
                     "accepted verbatim\n", payload.size());

        // (c) cap boundary: a declared length of EXACTLY the 64 MiB cap is
        //     accepted (the cap rejects only lengths > cap, matching the
        //     readFramed `len > kServeFrameCap` convention). Fed via a temp
        //     file — a 64 MiB anonymous-pipe write would deadlock on the
        //     pipe buffer with no concurrent reader.
        {
            const std::string big(kFrameCap, 'x');
            const std::string wire = "TSDEV_BATCH_CONTROL/1 " +
                std::to_string(kFrameCap) + "\n" + big;
            char path[MAX_PATH];
            f = diskFileFromBytes(wire, path);
            if (!f) {
                ec = fail("check8c: temp-file setup failed", 88);
                return 0;
            }
            manifest.clear();
            err.clear();
            bool ok = readBatchControlManifest(f, manifest, err);
            std::fclose(f);
            DeleteFileA(path);
            std::fprintf(stdout,
                         "  [8c] declared length == 64 MiB cap -> %s\n",
                         ok ? "accepted" : "REJECTED");
            if (!ok || manifest.size() != kFrameCap) {
                ec = fail("check8c: at-cap manifest rejected or truncated",
                          89);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: framed manifest header accepted, boundary "
                     "is byte-exact\n");
    }

    // ------------------------------------------------------------------
    // Check 9 (TVS C): readBatchControlManifest rejects malformed or
    //   oversized input. Every case must return false AND leave a nonempty
    //   diagnostic in `error`.
    // ------------------------------------------------------------------
    {
        const char* badInputs[] = {
            "",                          // EOF header: no bytes at all
            "TSDEV_BATCH_CON",           // EOF mid-header (truncated magic)
            "XSDEV_BATCH_CONTROL/1 3\nabc", // wrong magic
            "TSDEV_BATCH_CONTROL/2 3\nabc", // wrong version
            "TSDEV_BATCH_CONTROL/1\n",   // missing length token
            "TSDEV_BATCH_CONTROL/1 \n",  // empty length token
            "TSDEV_BATCH_CONTROL/1 abc\nhello",  // nondecimal length
            "TSDEV_BATCH_CONTROL/1 -1\nx",       // signed length
            "TSDEV_BATCH_CONTROL/1 +5\nhello",   // signed length
            "TSDEV_BATCH_CONTROL/1 99999999999999999999999999\nx", // overflow
            "TSDEV_BATCH_CONTROL/1 5x\nhello",   // junk inside length
            "TSDEV_BATCH_CONTROL/1 5 6\nhello",  // junk after length
            "TSDEV_BATCH_CONTROL/1 67108865\n",  // > 64 MiB cap
            "TSDEV_BATCH_CONTROL/1 10\nshort",   // short payload (5 of 10)
            "TSDEV_BATCH_CONTROL/1 5\n"          // short payload (0 of 5)
        };
        for (size_t i = 0; i < sizeof(badInputs) / sizeof(badInputs[0]);
             ++i) {
            FILE* f = fileFromBytes(badInputs[i]);
            if (!f) {
                ec = fail("check9: pipe setup failed", 90);
                return 0;
            }
            std::string manifest;
            std::string err;
            bool ok = readBatchControlManifest(f, manifest, err);
            std::fclose(f);
            std::fprintf(stdout, "  [9.%u] rejected=%d error=\"%s\"\n",
                         static_cast<unsigned>(i), !ok, err.c_str());
            if (ok) {
                ec = fail("check9: malformed input accepted", 91);
                return 0;
            }
            if (err.empty()) {
                ec = fail("check9: rejection left error empty", 92);
                return 0;
            }
        }
        std::fprintf(stdout,
                     "    PASS: malformed/oversized/short framed input "
                     "rejected\n");
    }

    // ------------------------------------------------------------------
    // Check 10 (TVS C): framed manifest + decision lines on the SAME
    //   stream — the --batch-stdin + --batch-progress-control wire shape.
    //   readBatchControlManifest consumes exactly the declared bytes, then
    //   readBatchControlDecision reads one decision line per call.
    // ------------------------------------------------------------------
    {
        const std::string payload = "strategy A C:\\x.el\n";
        const std::string wire = "TSDEV_BATCH_CONTROL/1 " +
            std::to_string(payload.size()) + "\n" + payload +
            "continue\ncancel\n";
        FILE* f = fileFromBytes(wire);
        if (!f) {
            ec = fail("check10: pipe setup failed", 100);
            return 0;
        }
        std::string manifest;
        std::string err;
        if (!readBatchControlManifest(f, manifest, err) ||
            manifest != payload) {
            std::fclose(f);
            ec = fail("check10: manifest read failed on shared stream",
                      101);
            return 0;
        }
        BatchControlDecision d1 = readBatchControlDecision(f, err);
        BatchControlDecision d2 = readBatchControlDecision(f, err);
        std::fclose(f);
        if (d1 != BatchControlDecision::Continue ||
            d2 != BatchControlDecision::Cancel) {
            ec = fail("check10: decisions after framed manifest != "
                      "Continue,Cancel", 102);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: framed manifest then decision lines share "
                     "one stream\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
DWORD WINAPI threadProc(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    __try {
        runTest(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        st->exitCode = 99;
    }
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_serve_framing: writeFramed/readFramed (unit) ===\n");

    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    int exitCode = st.exitCode;
    std::fprintf(stdout,
                 "\n=== test_serve_framing complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
