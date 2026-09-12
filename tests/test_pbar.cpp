// test_pbar.cpp
//
// Unit test for the --pbar progress-bar renderer core in src/tsdev_cli.cpp:
// the three PURE helpers (no I/O) plus the PbarRenderer emitter that glues
// them onto stderr.
//
//   - std::string pbarLine(const std::string& op, size_t done, size_t total,
//         const std::string& type, const std::string& name,
//         size_t failures);
//   - std::string pbarEraseSeq(int cells);
//   - std::string pbarDrawSeq(const std::string& line, int cells);
//   - struct PbarRenderer { PbarRenderer(op, total); start();
//         itemStarted(type,name); itemCompleted(); noteFailure();
//         eventLine(line); finish(); }
//
// This is a PURE UNIT TEST — it does NOT load orlib20.dll and does NOT touch
// the TradeStation installation. The body runs on a 64 MB-stack worker
// thread only to match the project's test-harness convention (the stack is
// harmless for a unit test).
//
// Linkage mechanism: identical to test_batch_progress.cpp — the helpers live
// inside tsdev_cli.cpp's anonymous namespace (internal linkage), so the test
// #defines TSDEV_CLI_TEST and #includes the .cpp directly (same TU; the
// file's main() is excluded by the TSDEV_CLI_TEST guard).
//
// Pinned line format (one physical line, no '\r'/'\n'/ANSI/trailing spaces,
// NEVER wider than kPbarMaxCells = 79 display cells):
//   <op> [<bar>] <done>/<total> <pct>%[ <type> "<name>"][ — <n> failed]
//   <bar>   exactly 30 cells: U+2588 '█' * floor(30*done/total) then U+2591
//           '░' fill (total==0 -> all '░'; done>total clamps to a full bar).
//   <pct>   done*100/total integer, right-aligned to width 3 + '%'.
//   label   <type> "<name>" with the name through the existing jsonQuote()
//           helper; empty when type and name are both empty; truncated with
//           a trailing "..." so the whole line stays <= 79 cells.
//   suffix  " — <n> failed" (U+2014 em dash) only when failures > 0; part of
//           the width budget.
// Cell width = display cells: every emitted glyph occupies exactly 1 console
// cell, counted as bytes with (b & 0xC0) != 0x80 (UTF-8 continuation bytes
// don't count).
//
// PbarRenderer capture (check 16): stderr's C fd is _dup2()'d onto a binary-
// mode temp file for the duration of the scenario so the emitted byte stream
// can be asserted exactly. _setmode(_O_BINARY) on fd 2 keeps the CRT from
// translating '\n' -> "\r\n" inside the capture (translation is an
// environment property of the fd flag, not renderer logic). With stderr
// redirected, _isatty(fileno(stderr)) is false so the renderer
// deterministically takes the fwrite path; the WriteConsoleW console path is
// reachable only on a real console and is not pinned here.
//
// Checks:
//   1. pbarLine zero progress: all-shade bar, "  0%", no label/suffix.
//   2. pbarLine mid progress + label: 15 full / 15 shade, " 50%",
//      `strategy "MyStrat"` appended.
//   3. pbarLine full bar: 30 full, "100%".
//   4. pbarLine total==0: all shade, "0/0   0%" (no division by zero).
//   5. pbarLine done>total: bar clamps to 30 full, pct stays formulaic 150%.
//   6. pbarLine failures only: " — 2 failed" U+2014 suffix, no label.
//   7. pbarLine label + failure suffix together.
//   8. Label truncation: exactly 79 cells, ends in "...", no failures.
//   9. Label truncation with the failures suffix eating budget: still
//      exactly 79 cells.
//  10. Multibyte-safe truncation: the label is cut only on UTF-8 code-point
//      boundaries (a 2-byte glyph at the cut is kept whole, never split).
//  11. jsonQuote consistency: a name with '"' + LF renders escaped
//      (\" and \n) so the bar stays one physical line.
//  12. Degenerate labels: type-only -> `type`; name-only -> `"name"`;
//      label dropped when even " ..." cannot fit; an op that overflows the
//      width budget alone -> hard 79-cell truncation.
//  13. pbarEraseSeq: '\r' + N spaces + '\r'.
//  14. pbarDrawSeq: '\r' + line + pad-to-cells; already-wide -> no pad;
//      multibyte glyphs counted in cells.
//  15. Invariant sweep: every rendered line is one physical line of <=79
//      cells with no trailing space.
//  16. PbarRenderer end-to-end: start/itemStarted/itemCompleted/noteFailure/
//      eventLine/finish emit the exact expected erase/draw byte stream to
//      stderr (composition pinned via the pure helpers).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

// Exclude tsdev_cli.cpp's main() so this test can define its own.
#define TSDEV_CLI_TEST

// Include the CLI source directly so the anonymous-namespace internals
// (the pbar helpers, PbarRenderer, jsonQuote, etc.) are visible.
#include "../src/tsdev_cli.cpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <io.h>
#include <fcntl.h>
#include <windows.h>

namespace {

// UTF-8 encodings of the pinned glyphs, spelled as \x escapes so this file
// stays ASCII-only (no /utf-8 compile flag; the implementation does the
// same). Each escape is immediately followed by another escape or a
// non-hex character, so the greedy \x parsing cannot overrun.
const std::string kFull   = "\xE2\x96\x88";  // U+2588 FULL BLOCK   '█'
const std::string kShade  = "\xE2\x96\x91";  // U+2591 LIGHT SHADE  '░'
const std::string kEmDash = "\xE2\x80\x94";  // U+2014 EM DASH      '—'
const std::string kAlpha  = "\xCE\xB1";      // U+03B1 GREEK SMALL LETTER ALPHA 'α'

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Test-side display-cell width under the pinned rule: count bytes that are
// not UTF-8 continuation bytes. Independent restatement of the spec'd
// algorithm used to model the renderer's maxCells tracking.
size_t cellWidth(const std::string& s) {
    size_t cells = 0;
    for (unsigned char b : s)
        if ((b & 0xC0) != 0x80) ++cells;
    return cells;
}

// s repeated n times (for building expected bars of multibyte glyphs).
std::string rep(const std::string& s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

// Returns true when `line` is exactly one physical line — non-empty, no
// '\n'/'\r' anywhere, no trailing space.
bool isSingleLine(const std::string& line) {
    return !line.empty() &&
           line.find('\n') == std::string::npos &&
           line.find('\r') == std::string::npos;
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_pbar: --pbar progress-bar renderer (unit) ===\n");

    // ------------------------------------------------------------------
    // Check 1: pbarLine zero progress, no label, no failures.
    //   pbarLine("insert", 0, 10, "", "", 0)
    // Expected: insert [░░x30] 0/10   0%
    // ------------------------------------------------------------------
    {
        std::string line = pbarLine("insert", 0, 10, "", "", 0);
        std::fprintf(stdout, "  [1] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kShade, 30) + "] 0/10   0%";
        if (line != want) {
            ec = fail("check1: zero-progress line bytes wrong", 10);
            return 0;
        }
        if (!isSingleLine(line) || cellWidth(line) > 79 ||
            line.back() == ' ') {
            ec = fail("check1: line invariant violated", 11);
            return 0;
        }
        std::fprintf(stdout, "    PASS: empty bar exact\n");
    }

    // ------------------------------------------------------------------
    // Check 2: mid progress with label.
    //   pbarLine("batch-insert", 5, 10, "strategy", "MyStrat", 0)
    //   floor(30*5/10)=15 full + 15 shade; pct 50 -> " 50%"
    // Expected: batch-insert [█x15░x15] 5/10  50% strategy "MyStrat"
    // ------------------------------------------------------------------
    {
        std::string line =
            pbarLine("batch-insert", 5, 10, "strategy", "MyStrat", 0);
        std::fprintf(stdout, "  [2] %s\n", line.c_str());
        const std::string want =
            "batch-insert [" + rep(kFull, 15) + rep(kShade, 15) +
            "] 5/10  50% strategy \"MyStrat\"";
        if (line != want) {
            ec = fail("check2: mid-progress line bytes wrong", 20);
            return 0;
        }
        if (cellWidth(line) != 74) {
            ec = fail("check2: expected 74 display cells", 21);
            return 0;
        }
        std::fprintf(stdout, "    PASS: mid bar + label exact\n");
    }

    // ------------------------------------------------------------------
    // Check 3: full bar at 100%.
    //   pbarLine("insert", 10, 10, "function", "F", 0)
    // Expected: insert [█x30] 10/10 100% function "F"
    // ------------------------------------------------------------------
    {
        std::string line =
            pbarLine("insert", 10, 10, "function", "F", 0);
        std::fprintf(stdout, "  [3] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kFull, 30) + "] 10/10 100% function \"F\"";
        if (line != want) {
            ec = fail("check3: full bar bytes wrong", 30);
            return 0;
        }
        std::fprintf(stdout, "    PASS: full bar exact\n");
    }

    // ------------------------------------------------------------------
    // Check 4: total == 0 -> all shade, pct 0, no division by zero.
    //   pbarLine("delete", 0, 0, "", "", 0)
    // Expected: delete [░x30] 0/0   0%
    // ------------------------------------------------------------------
    {
        std::string line = pbarLine("delete", 0, 0, "", "", 0);
        std::fprintf(stdout, "  [4] %s\n", line.c_str());
        const std::string want =
            "delete [" + rep(kShade, 30) + "] 0/0   0%";
        if (line != want) {
            ec = fail("check4: total==0 line bytes wrong", 40);
            return 0;
        }
        std::fprintf(stdout, "    PASS: total==0 exact\n");
    }

    // ------------------------------------------------------------------
    // Check 5: done > total -> bar clamps to a full 30 cells; pct stays the
    // pinned formula value (150, width-3 field overflows to "150%").
    //   pbarLine("x", 15, 10, "", "", 0)
    // Expected: x [█x30] 15/10 150%
    // ------------------------------------------------------------------
    {
        std::string line = pbarLine("x", 15, 10, "", "", 0);
        std::fprintf(stdout, "  [5] %s\n", line.c_str());
        const std::string want =
            "x [" + rep(kFull, 30) + "] 15/10 150%";
        if (line != want) {
            ec = fail("check5: done>total clamp bytes wrong", 50);
            return 0;
        }
        std::fprintf(stdout, "    PASS: done>total clamps bar only\n");
    }

    // ------------------------------------------------------------------
    // Check 6: failures suffix only (no label).
    //   pbarLine("insert", 7, 10, "", "", 2)
    //   floor(30*7/10)=21 full + 9 shade; pct 70 -> " 70%"
    // Expected: insert [█x21░x9] 7/10  70% — 2 failed
    // ------------------------------------------------------------------
    {
        std::string line = pbarLine("insert", 7, 10, "", "", 2);
        std::fprintf(stdout, "  [6] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kFull, 21) + rep(kShade, 9) +
            "] 7/10  70% " + kEmDash + " 2 failed";
        if (line != want) {
            ec = fail("check6: failures-suffix line bytes wrong", 60);
            return 0;
        }
        std::fprintf(stdout, "    PASS: failures suffix exact\n");
    }

    // ------------------------------------------------------------------
    // Check 7: label + failure suffix together.
    //   pbarLine("modify", 3, 4, "showme", "S", 1)
    //   floor(30*3/4)=22 full + 8 shade; pct 75 -> " 75%"
    // Expected: modify [█x22░x8] 3/4  75% showme "S" — 1 failed
    // ------------------------------------------------------------------
    {
        std::string line = pbarLine("modify", 3, 4, "showme", "S", 1);
        std::fprintf(stdout, "  [7] %s\n", line.c_str());
        const std::string want =
            "modify [" + rep(kFull, 22) + rep(kShade, 8) +
            "] 3/4  75% showme \"S\" " + kEmDash + " 1 failed";
        if (line != want) {
            ec = fail("check7: label+suffix line bytes wrong", 70);
            return 0;
        }
        if (cellWidth(line) != 70) {
            ec = fail("check7: expected 70 display cells", 71);
            return 0;
        }
        std::fprintf(stdout, "    PASS: label + suffix exact\n");
    }

    // ------------------------------------------------------------------
    // Check 8: label truncation (no failures). Budget for " " + label is
    // 79 - 49(prefix) = 30 cells; the 51-cell label is cut to 26 cells +
    // "...". The quote stays unterminated — truncation is mechanical.
    //   pbarLine("insert", 0, 10, "strategy", <40-char name>, 0)
    // Expected: insert [░x30] 0/10   0% strategy "ABCDEFGHIJKLMNOP...
    // ------------------------------------------------------------------
    {
        const std::string name40 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";  // 40 cells
        std::string line = pbarLine("insert", 0, 10, "strategy", name40, 0);
        std::fprintf(stdout, "  [8] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kShade, 30) +
            "] 0/10   0% strategy \"ABCDEFGHIJKLMNOP...";
        if (line != want) {
            ec = fail("check8: truncated label bytes wrong", 80);
            return 0;
        }
        if (cellWidth(line) != 79) {
            ec = fail("check8: truncated line != exactly 79 cells", 81);
            return 0;
        }
        std::fprintf(stdout, "    PASS: truncation lands exactly 79 cells\n");
    }

    // ------------------------------------------------------------------
    // Check 9: label truncation with the failures suffix eating budget.
    //   prefix 49 + suffix " — 5 failed" (11) = 60 used; label budget is
    //   19 cells -> 15 label cells + "...".
    //   pbarLine("insert", 0, 10, "strategy", <40-char name>, 5)
    // Expected: insert [░x30] 0/10   0% strategy "ABCDE... — 5 failed
    // ------------------------------------------------------------------
    {
        const std::string name40 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";
        std::string line = pbarLine("insert", 0, 10, "strategy", name40, 5);
        std::fprintf(stdout, "  [9] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kShade, 30) +
            "] 0/10   0% strategy \"ABCDE... " + kEmDash + " 5 failed";
        if (line != want) {
            ec = fail("check9: truncated label+suffix bytes wrong", 90);
            return 0;
        }
        if (cellWidth(line) != 79) {
            ec = fail("check9: truncated line != exactly 79 cells", 91);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: truncation respects suffix budget\n");
    }

    // ------------------------------------------------------------------
    // Check 10: multibyte-safe truncation. The name is 15 ASCII letters +
    // 'α' (2-byte UTF-8) + more letters, so the 26-cell cut lands right
    // after the multibyte glyph: a byte-naive cut would emit a lone lead
    // byte or drop the glyph.
    //   pbarLine("insert", 0, 10, "strategy",
    //            "ABCDEFGHIJKLMNO" + α + "PQRSTUVWXYZabcdefg", 0)
    // Expected: insert [░x30] 0/10   0% strategy "ABCDEFGHIJKLMNOα...
    // ------------------------------------------------------------------
    {
        const std::string name =
            "ABCDEFGHIJKLMNO" + kAlpha + "PQRSTUVWXYZabcdefg";
        std::string line = pbarLine("insert", 0, 10, "strategy", name, 0);
        std::fprintf(stdout, "  [10] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kShade, 30) +
            "] 0/10   0% strategy \"ABCDEFGHIJKLMNO" + kAlpha + "...";
        if (line != want) {
            ec = fail("check10: multibyte truncation split a code point",
                      100);
            return 0;
        }
        if (cellWidth(line) != 79) {
            ec = fail("check10: truncated line != exactly 79 cells", 101);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: truncation never splits a code point\n");
    }

    // ------------------------------------------------------------------
    // Check 11: jsonQuote consistency — a name containing '"' and LF
    // renders through the same escaping as BATCH lines, keeping the bar
    // one physical line.
    //   pbarLine("insert", 1, 1, "function", "A\"B\nC", 0)
    //   jsonQuote -> "A\"B\nC"  (as text: "A\"B\nC")
    // Expected: insert [█x30] 1/1 100% function "A\"B\nC"
    // ------------------------------------------------------------------
    {
        const std::string name = "A\"B\nC";
        std::string line = pbarLine("insert", 1, 1, "function", name, 0);
        std::fprintf(stdout, "  [11] %s\n", line.c_str());
        const std::string want =
            "insert [" + rep(kFull, 30) +
            "] 1/1 100% function \"A\\\"B\\nC\"";
        if (line != want) {
            ec = fail("check11: jsonQuote-escaped name bytes wrong", 110);
            return 0;
        }
        if (!isSingleLine(line)) {
            ec = fail("check11: escaped name leaked a raw newline", 111);
            return 0;
        }
        std::fprintf(stdout, "    PASS: jsonQuote escaping in label\n");
    }

    // ------------------------------------------------------------------
    // Check 12: degenerate labels + overflow safety.
    //   (a) type only -> label is just `type` (no quoting).
    //   (b) name only -> label is just `"name"`.
    //   (c) label dropped when not even " ..." fits (budget 3).
    //   (d) op alone wider than the budget -> hard 79-cell truncation.
    //   (e) op sized so the hard-clamp cut lands on the "] " separator
    //       space -> trailing space stripped, line ends at ']' (78 cells).
    // ------------------------------------------------------------------
    {
        std::string a = pbarLine("ins", 0, 1, "function", "", 0);
        const std::string wantA =
            "ins [" + rep(kShade, 30) + "] 0/1   0% function";
        if (a != wantA) {
            ec = fail("check12a: type-only label bytes wrong", 120);
            return 0;
        }

        std::string b = pbarLine("ins", 0, 1, "", "Solo", 0);
        const std::string wantB =
            "ins [" + rep(kShade, 30) + "] 0/1   0% \"Solo\"";
        if (b != wantB) {
            ec = fail("check12b: name-only label bytes wrong", 121);
            return 0;
        }

        // op 34 cells -> prefix uses 76 cells; budget for " " + label is 3
        // (< 4, too small even for " ...") so the label is dropped.
        std::string c = pbarLine(rep("x", 34), 0, 1, "t", "n", 0);
        const std::string wantC =
            rep("x", 34) + " [" + rep(kShade, 30) + "] 0/1   0%";
        if (c != wantC) {
            ec = fail("check12c: label not dropped on tiny budget", 122);
            return 0;
        }

        // op 60 cells -> prefix alone is 102 cells; the hard clamp keeps
        // the invariant: first 79 cells = 60 x's + " [" + 17 shades.
        std::string d = pbarLine(rep("x", 60), 0, 1, "", "", 0);
        const std::string wantD =
            rep("x", 60) + " [" + rep(kShade, 17);
        if (d != wantD || cellWidth(d) != 79) {
            ec = fail("check12d: over-wide op not clamped to 79 cells",
                      123);
            return 0;
        }

        // op 45 cells -> the 79-cell cut lands exactly on the "] "
        // separator space; the trailing space must be stripped so the line
        // ends at ']' (78 cells) — pins the no-trailing-space invariant
        // through the hard clamp.
        std::string e = pbarLine(rep("x", 45), 0, 1, "", "", 0);
        const std::string wantE =
            rep("x", 45) + " [" + rep(kShade, 30) + "]";
        if (e != wantE || cellWidth(e) != 78 || e.back() == ' ') {
            ec = fail("check12e: hard clamp left a trailing space", 124);
            return 0;
        }
        std::fprintf(stdout, "    PASS: degenerate labels + overflow\n");
    }

    // ------------------------------------------------------------------
    // Check 13: pbarEraseSeq — '\r' + cells spaces + '\r'.
    // ------------------------------------------------------------------
    {
        if (pbarEraseSeq(0) != std::string("\r\r")) {
            ec = fail("check13: pbarEraseSeq(0) bytes wrong", 130);
            return 0;
        }
        if (pbarEraseSeq(5) != std::string("\r     \r")) {
            ec = fail("check13: pbarEraseSeq(5) bytes wrong", 131);
            return 0;
        }
        if (pbarEraseSeq(79) !=
            "\r" + rep(" ", 79) + "\r") {
            ec = fail("check13: pbarEraseSeq(79) bytes wrong", 132);
            return 0;
        }
        std::fprintf(stdout, "    PASS: erase sequences exact\n");
    }

    // ------------------------------------------------------------------
    // Check 14: pbarDrawSeq — '\r' + line + pad to `cells`; already-wide
    // lines get no pad; multibyte glyphs count as 1 cell each.
    // ------------------------------------------------------------------
    {
        if (pbarDrawSeq("abc", 6) != std::string("\rabc   ")) {
            ec = fail("check14: pbarDrawSeq pad bytes wrong", 140);
            return 0;
        }
        if (pbarDrawSeq("abc", 3) != std::string("\rabc")) {
            ec = fail("check14: pbarDrawSeq exact-fit bytes wrong", 141);
            return 0;
        }
        if (pbarDrawSeq("abc", 2) != std::string("\rabc")) {
            ec = fail("check14: pbarDrawSeq over-wide must not pad/truncate",
                      142);
            return 0;
        }
        // 2-cell line of 3-byte glyphs -> 3 pad spaces, not 3 per byte.
        if (pbarDrawSeq(kFull + kShade, 5) !=
            "\r" + kFull + kShade + "   ") {
            ec = fail("check14: multibyte cell accounting wrong", 143);
            return 0;
        }
        std::fprintf(stdout, "    PASS: draw sequences exact\n");
    }

    // ------------------------------------------------------------------
    // Check 15: invariant sweep — for a representative matrix every line is
    // a single physical line, <= 79 display cells, no trailing space.
    // ------------------------------------------------------------------
    {
        struct Case {
            const char* op;
            size_t done;
            size_t total;
            const char* type;
            const char* name;
            size_t failures;
        };
        const std::string longName(80, 'n');
        const Case cases[] = {
            {"insert",       0, 10, "",         "",          0},
            {"batch-delete", 3, 10, "strategy", "MyStrat",   0},
            {"batch-upsert", 9, 10, "function", "Fn",        3},
            {"modify",       10, 10, "showme",  "S",         0},
            {"delete",       0, 0,  "",         "",          0},
            {"x",            15, 10, "",        "",          0},
            {"insert",       0, 10, "strategy", longName.c_str(), 0},
            {"insert",       0, 10, "strategy", longName.c_str(), 9},
            {"ins",          0, 1,  "function", "",          0},
            {"ins",          0, 1,  "",         "Solo",      0},
        };
        // Separately sweep over-wide ops (the hard-clamp path): the cut may
        // land on a separator/pad space and must never leak one at the end.
        for (size_t opLen = 34; opLen <= 60; ++opLen) {
            const std::string op(opLen, 'x');
            std::string line = pbarLine(op, 0, 1, "", "", 0);
            if (!isSingleLine(line) || cellWidth(line) > 79 ||
                line.back() == ' ') {
                ec = fail("check15: over-wide op leaked trailing space",
                          151);
                return 0;
            }
        }
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const Case& c = cases[i];
            std::string line = pbarLine(c.op, c.done, c.total, c.type,
                                        c.name, c.failures);
            if (!isSingleLine(line) || cellWidth(line) > 79 ||
                line.back() == ' ') {
                ec = fail("check15: invariant violated in sweep", 150);
                return 0;
            }
        }
        std::fprintf(stdout, "    PASS: invariants hold across sweep\n");
    }

    // ------------------------------------------------------------------
    // Check 16: PbarRenderer end-to-end composition. stderr's C fd is
    // redirected to a binary temp file; the emitted bytes must equal the
    // reference model built from the pure helpers with the same maxCells
    // tracking (draw pads to the widest line so far; erase clears it).
    // Scenario:
    //   start()                 -> draw L0
    //   itemStarted(f,"Alpha")  -> erase + draw L1
    //   itemCompleted()         -> draw L2           (done=1)
    //   noteFailure()           -> draw L3           (failures=1)
    //   eventLine("EVT one")    -> erase + "EVT one\n" + draw L3
    //   itemStarted(f,"Beta")   -> erase + draw L5
    //   itemCompleted()         -> draw L6           (done=2)
    //   finish()                -> draw L6 + "\n"
    // ------------------------------------------------------------------
    {
        // Redirect C stderr to a binary temp file and remember the original
        // fd so it can be restored afterwards.
        FILE* tmp = std::tmpfile();
        if (!tmp) {
            ec = fail("check16: tmpfile() failed", 160);
            return 0;
        }
        int savedFd = _dup(_fileno(stderr));
        if (savedFd < 0) {
            std::fclose(tmp);
            ec = fail("check16: _dup(stderr) failed", 161);
            return 0;
        }
        if (_dup2(_fileno(tmp), _fileno(stderr)) != 0) {
            _close(savedFd);
            std::fclose(tmp);
            ec = fail("check16: _dup2(tmp, stderr) failed", 162);
            return 0;
        }
        // Binary mode pins the exact bytes writeRaw emits — no '\n' ->
        // "\r\n" CRT translation inside the capture. _isatty(fileno) is now
        // false, so the renderer deterministically uses the fwrite path.
        _setmode(_fileno(stderr), _O_BINARY);

        {
            PbarRenderer r("insert", 2);
            r.start();
            r.itemStarted("function", "Alpha");
            r.itemCompleted();
            r.noteFailure();
            r.eventLine("EVT one");
            r.itemStarted("function", "Beta");
            r.itemCompleted();
            r.finish();
        }
        std::fflush(stderr);

        // Restore stderr, then read back the capture.
        if (_dup2(savedFd, _fileno(stderr)) != 0) {
            _close(savedFd);
            std::fclose(tmp);
            ec = fail("check16: stderr restore failed", 163);
            return 0;
        }
        _close(savedFd);
        _setmode(_fileno(stderr), _O_TEXT);

        std::string got;
        std::fseek(tmp, 0, SEEK_SET);
        char buf[256];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
            got.append(buf, n);
        std::fclose(tmp);

        // Reference model: identical sequencing through the pure helpers.
        const std::string L0 = pbarLine("insert", 0, 2, "", "", 0);
        const std::string L1 =
            pbarLine("insert", 0, 2, "function", "Alpha", 0);
        const std::string L2 =
            pbarLine("insert", 1, 2, "function", "Alpha", 0);
        const std::string L3 =
            pbarLine("insert", 1, 2, "function", "Alpha", 1);
        const std::string L5 =
            pbarLine("insert", 1, 2, "function", "Beta", 1);
        const std::string L6 =
            pbarLine("insert", 2, 2, "function", "Beta", 1);

        std::string want;
        int m = 0;
        auto draw = [&](const std::string& l) {
            int c = static_cast<int>(cellWidth(l));
            if (c > m) m = c;
            want += pbarDrawSeq(l, m);
        };
        auto erase = [&]() { want += pbarEraseSeq(m); };

        draw(L0);            // start
        erase(); draw(L1);   // itemStarted
        draw(L2);            // itemCompleted
        draw(L3);            // noteFailure
        erase(); want += "EVT one\n"; draw(L3);  // eventLine
        erase(); draw(L5);   // itemStarted
        draw(L6);            // itemCompleted
        draw(L6); want += "\n";                  // finish

        if (got != want) {
            std::fprintf(stderr,
                         "  [16] got %u bytes, want %u bytes\n",
                         static_cast<unsigned>(got.size()),
                         static_cast<unsigned>(want.size()));
            ec = fail("check16: renderer byte stream mismatch", 164);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: renderer emits exact erase/draw stream\n");
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
                 "=== test_pbar: --pbar progress-bar renderer (unit) ===\n");

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
                 "\n=== test_pbar complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
