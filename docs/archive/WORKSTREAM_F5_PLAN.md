# Workstream F5 — TVS-F.5: Case-Insensitive `exists` + Verbatim Stored Name

**Status:** COMPLETE (2026-08-19) — ALL sub-tasks S1–S20 DONE. 8/8 C++ regression suites + 60/60 Python tests PASS.
**Date:** 2026-08-20 (planning); 2026-08-19 (implementation complete)
**Author:** Planner subagent
**Purpose:** Atomic subtask decomposition for TVS-F.5, ready for PM execution.
**RE basis:** `docs/RE/RE_F5_enumeration_findings.md` (ACCEPTED, adversarially reviewed)

> **Implementation note (post-completion):** The actual implementation diverged from the original plan in the `enumerateRecords` approach. Instead of a single-phase `_LoadMasterRecsEX@16` with `filterMode=0` (which required cmd 0x0F + cmd 0x4E cache refresh), the implementation uses a two-phase approach: Phase 1 = `_LoadMasterRecsEX@16` (cached, `_stricmp` search, two-pass without/with refreshCache); Phase 2 = `FindExistPos` (BST, case-insensitive) + `getSource` (verbatim stored name from `{ name }` comment block) for newly created records. New signature: `FUN_101cfce0_thread_check_INT3` (cmd 0x0D dispatcher) — 19 total signatures. Test file: `tests/test_exists_ci.ps1` (9 test cases). Python E2E test: `test_exists_case_insensitive_e2e`.

---

## 1. Scope Summary

TVS-F.5 extends the existing `exists` subcommand (TVS-F.1, exact-match) with:

1. A `--case-insensitive` (short `-i`) flag on the C++ CLI `exists` subcommand.
2. Case-insensitive name matching via orlib20 enumeration (`_LoadMasterRecsEX@16`), NOT `findExistPos`.
3. A `stored_name` field in the output (both human and JSON modes) containing the verbatim (correctly-cased) stored item name.
4. Python wrapper `TsDevClient.exists` gains a `case_insensitive: bool = False` param.
5. Python CLI `exists` subcommand gains `-i`/`--case-insensitive`.

### Key design decisions

- **Exact-match (no `-i`):** `stored_name` = the query name as-is (since `findExistPos` is exact-match, the stored name IS the query name when found). No enumeration overhead. `stored_name` is empty/omitted when not found.
- **Case-insensitive (`-i`):** Uses `_LoadMasterRecsEX@16` with `filterMode=0` (filtered mode, matching the authoritative caller `FUN_1015b760`). Iterates 0x10A-byte records, reads the name from offset `0x17` (inline long name — see RE §4 CRITICAL note: offset 0x02 is overwritten with ProgId by lazy-init), compares with `_stricmp`. Returns the verbatim stored name + ProgId.
- **TradingApp handling:** Category 2 (Indicator) enumeration includes TradingApp records (expanded type `0x1402`). Filter by `rec[0xE9] == 0x14` for TradingApp, `rec[0xE9] != 0x14` for Indicator.
- **All 3 new signatures added** (cmd 0x08, 0x09, 0x07) to the JSON for documentation and future use. Patches for 0x08 and 0x09 are applied in the constructor (needed for `filterMode=0` enumeration). The 0x07 patch is NOT applied (only needed for `filterMode != 0`, which we don't use), but the signature is available.
- **Thread-check array grows from [4] to [6]** (adds tc5=cmd 0x08, tc6=cmd 0x09).

---

## 2. Dependency Graph (ALL DONE)

```
F5-S1 (signatures + codegen counts) [DONE]
  │
  ├──► F5-S2 (write C++ test for enumerateRecords — RED) [DONE]
  │      │
  │      └──► F5-S3 (build + run C++ test — confirm RED) [DONE]
  │             │
  │             └──► F5-S4 (implement enumerateRecords in TsDevLib — GREEN) [DONE]
  │                    │
  │                    └──► F5-S5 (build + run C++ test — confirm GREEN) [DONE]
  │                           │
  │                           ├──► F5-S6 (adversarial review: TsDevLib impl) [DONE]
  │                           │
  │                           └──► F5-S7 (write PowerShell E2E test — RED) [DONE]
  │                                  │
  │                                  └──► F5-S8 (build + run E2E test — confirm RED) [DONE]
  │                                         │
  │                                         └──► F5-S9 (implement CLI --case-insensitive — GREEN) [DONE]
  │                                                │
  │                                                └──► F5-S10 (build + run E2E test — confirm GREEN) [DONE]
  │                                                       │
  │                                                       ├──► F5-S11 (adversarial review: CLI impl) [DONE]
  │                                                       │
  │                                                       └──► F5-S12 (write Python tests — RED) [DONE]
  │                                                              │
  │                                                              └──► F5-S13 (run Python tests — confirm RED) [DONE]
  │                                                                     │
  │                                                                     └──► F5-S14 (implement Python wrapper — GREEN) [DONE]
  │                                                                            │
  │                                                                            └──► F5-S15 (run Python tests — confirm GREEN) [DONE]
  │                                                                                   │
  │                                                                                   ├──► F5-S16 (adversarial review: Python impl) [DONE]
  │                                                                                   │
  │                                                                                   └──► F5-S17 (run full regression suite) [DONE]
  │                                                                                          │
  │                                                                                          └──► F5-S18 (update E2E_TEST_SPEC.md) [DONE]
  │                                                                                                 │
  │                                                                                                 └──► F5-S19 (adversarial review: docs) [DONE]
  │                                                                                                        │
  │                                                                                                        └──► F5-S20 (E2E CLI verification — MANDATORY) [DONE]
```

---

## 3. Execution Order

```
S1 [DONE] → S2 [DONE] → S3 [DONE] → S4 [DONE] → S5 [DONE] → S6 [DONE] → S7 [DONE] → S8 [DONE] → S9 [DONE] → S10 [DONE] → S11 [DONE] → S12 [DONE] → S13 [DONE] → S14 [DONE] → S15 [DONE] → S16 [DONE] → S17 [DONE] → S18 [DONE] → S19 [DONE] → S20 [DONE]
```

---

## 4. Subtask Specifications

### F5-S1: Add 3 new signatures + 1 new export to signature JSON + bump codegen counts [DONE]

- **Phase:** Signature/codegen
- **Scope:** Edit 3 files (one coherent "signature count bump 13→16" change — acceptable per project rules since all 3 edits are part of a single logical count-bump):
  - `signatures/v9.50.01.3344.json`:
    - Add 3 new signature entries to the `signatures` array (AFTER the existing `FUN_101d0e80_thread_check_INT3` entry, BEFORE the closing `]`):
      1. `FUN_101d0c30_thread_check_INT3` — cmd 0x08, offset 40, `CC→90`, 51-byte pattern. Copy the exact JSON entry from RE §5.1.
      2. `FUN_101d16c0_thread_check_INT3` — cmd 0x09, offset 84, `CC→90`, 132-byte pattern. Copy the exact JSON entry from RE §5.2.
      3. `FUN_101d0d90_thread_check_INT3` — cmd 0x07, offset 47, `CC→90`, 58-byte pattern. Copy the exact JSON entry from RE §5.3.
    - Add 1 new export entry to the `exports` array (AFTER `_LoadTextFileEX@16`):
      ```json
      {
        "name": "_LoadMasterRecsEX@16",
        "rva": "0x1A5360",
        "callingConvention": "stdcall"
      }
      ```
  - `tools/sigcodegen.py`:
    - Bump `EXPECTED_SIGNATURE_COUNT` from `13` to `16` (line 37).
    - Bump `EXPECTED_EXPORT_COUNT` from `8` to `9` (line 39).
  - `tests/test_sigtool.ps1` (the sigtool regression test hardcodes the signature count and stale regexes that must be bumped in lockstep with the JSON/codegen, otherwise S1's own verification and S17's full regression fail):
    - Bump `$expectedSignatureCount` from `13` to `16` (line 40).
    - Update the `'13\s*/\s*13'` regexes to `'16\s*/\s*16'` at line 173 (Test 1, non-verbose `verify` ExtraCheck) and line 271 (Test 8, same pattern). These regexes match the single non-verbose summary line `"%d/%d signatures PASS\n"` emitted by `src/sigtool.cpp` (lines 501–505); after the bump sigtool prints `"16/16 signatures PASS\n"`, which the old `'13/13'` regex does not match, and the fallback `PASS`-line count is 1 (just the summary line) which is `< 16`, so both OR branches fail without this fix.
    - Update comments mentioning the old counts:
      - Line 36: `"# Committed signature set for v9.50.01.3344 (13 signatures, 4 patch_site)."` → `"(16 signatures, 7 patch_site)."` (3 new thread-check INT3 patch sites are added alongside the existing 4).
      - Line 163: `"#   and report all 13 signatures as PASS. Exit 0."` → `"all 16 signatures"`.
      - Line 195: `"#   Validates original bytes at every patch_site signature. All 4 patch sites"` → `"All 7 patch sites"`.
    - Note: `tests/test_orlib_resolver.cpp` and `tests/test_no_hardcoded_rvas.cpp` use `tsdev::kSignatureCount` from the generated header and auto-update — no manual change needed.
- **Dependencies:** None.
- **Verification:** Run `python tools/sigcodegen.py --manifest signatures/version_manifest.json --sigdir signatures --output build/signatures.gen.h` (or just rebuild the project). The codegen must succeed without count-mismatch errors. Then run `powershell -ExecutionPolicy Bypass -File tests\test_sigtool.ps1` to confirm all 16 signatures match the baseline DLL (Test 1 and Test 8 now expect `16/16` and `$expectedSignatureCount == 16`).
- **TDD note:** N/A (data/config change, no test-first applicable).

---

### F5-S2: Write failing C++ test for `enumerateRecords` (RED) [DONE]

- **Phase:** Tests (C++ library level)
- **Scope:** Edit 3 files (one coherent "add a new test" change):
  - `tests/test_exists_caseinsensitive.cpp` (NEW):
    - Create a test file mirroring `tests/test_exists.cpp` structure.
    - Test flow (on a 64MB-stack worker thread):
      1. Construct `TsDevLib`.
      2. Pre-clean: `findExistPos(kTestName, Strategy)` — delete if exists.
      3. `createEntry(Strategy, kTestName, errCode)` → assert ProgId > 0.
      4. `saveSource(Strategy, progId, kTestSource)` → assert success.
      5. `verifyModule(Strategy, progId)` → assert no errors.
      6. Call `lib.enumerateRecords(Strategy, kTestNameUpper)` where `kTestNameUpper` is `kTestName` converted to uppercase — assert `result.exists == true`, `result.storedName == kTestName` (verbatim), `result.progId > 0`.
      7. Call `lib.enumerateRecords(Strategy, "NONEXISTENTNAMEXYZ")` — assert `result.exists == false`, `result.storedName == ""`, `result.progId == 0`.
      8. Cleanup: `deleteEntry(Strategy, kTestName)`.
    - Constants: `kTestName = "zzz_test_ci_probe"`, `kTestNameUpper = "ZZZ_TEST_CI_PROBE"`, `kNonExistentName = "NONEXISTENTNAMEXYZ"`.
    - Use the same `kTestSource` as `test_exists.cpp`.
  - `CMakeLists.txt`:
    - Add `add_executable(test_exists_caseinsensitive tests/test_exists_caseinsensitive.cpp)` and `target_link_libraries(test_exists_caseinsensitive tsdev_lib)` after the `test_exists` target (around line 134).
    - Add `test_exists_caseinsensitive` to the `test_regression` target's list (around line 230).
  - `tests/run_unit_tests.ps1`:
    - Add `test_exists_caseinsensitive` to the test inventory comment and the `$Tests` array (after `test_exists`, around line 22/100). Set `RequiresTs = $true`.
- **Dependencies:** F5-S1 (the new signatures/export must be in the JSON so the generated header compiles — though the test itself doesn't use them directly, TsDevLib's constructor will fail if the counts mismatch).
- **Verification:** Visual inspection — the test file exists, CMakeLists has the target, run_unit_tests.ps1 has the entry.
- **TDD note:** This IS the RED test. The test calls `enumerateRecords()` which does not exist yet → compilation failure (RED). The test also references an `EnumerateResult` struct that does not exist yet.

---

### F5-S3: Build + run C++ test to confirm RED [DONE]

- **Phase:** Tests (C++ library level)
- **Scope:** Build the new test executable and attempt to run it.
- **Dependencies:** F5-S2.
- **Verification:**
  1. `cmake --build build --config Release --target test_exists_caseinsensitive` — expect COMPILATION FAILURE (the `enumerateRecords` method and `EnumerateResult` struct don't exist in `tsdev_lib.h`). This is the expected RED state.
  2. If the build somehow succeeds (it shouldn't), run `build\Release\test_exists_caseinsensitive.exe` and confirm it fails.
- **TDD note:** RED = compilation error (`enumerateRecords` is not a member of `TsDevLib`).

---

### F5-S4: Implement `enumerateRecords` in TsDevLib (GREEN) [DONE]

- **Phase:** TsDevLib
- **Scope:** Edit 2 files:
  - `src/tsdev_lib.h`:
    - Add a new result struct (after `Result`, around line 55):
      ```cpp
      // Result of a case-insensitive enumeration-based existence query.
      struct EnumerateResult {
          bool        exists = false;
          std::string storedName;  // verbatim stored name (empty if not found)
          int         progId  = 0; // ProgId (0 if not found)
      };
      ```
    - Add a new public method declaration (after `getSource`, around line 92):
      ```cpp
      // Case-insensitive existence query via enumeration. Enumerates all
      // master records of the given category via _LoadMasterRecsEX@16,
      // compares names case-insensitively (_stricmp), and returns the
      // verbatim stored name + ProgId if found.
      // For TradingApp (category 2, subtype 0x14), filters by rec[0xE9].
      EnumerateResult enumerateRecords(ComponentType type,
                                       const std::string& searchName);
      ```
    - Add a new typedef for the export (after `LoadTextFileExFn`, around line 221):
      ```cpp
      typedef int (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                                   const char* path, int filterMode);
      ```
    - Add a new member (after `m_loadTextFileEx`, around line 230):
      ```cpp
      LoadMasterRecsExFn    m_loadMasterRecsEx = nullptr;
      ```
    - Grow the thread-check arrays from [4] to [6] (lines 180-184):
      - `uintptr_t m_threadCheckRvas[6] = {};`
      - `unsigned char m_threadCheckOriginalBytes[6] = {};`
  - `src/tsdev_lib.cpp`:
    - In the constructor (after `m_loadTextFileEx` resolution, around line 531), add:
      ```cpp
      m_loadMasterRecsEx = (LoadMasterRecsExFn)m_resolver->resolveExport("_LoadMasterRecsEX@16");
      ```
    - In the destructor (around line 588), update the comment `"Restore all 5 patch sites"` to `"Restore all 7 patch sites"` (S4 grows the thread-check sites from 4 to 6; 1 persistence + 6 thread-check = 7 total). Then change the loop bound from `4` to `6`:
      ```cpp
      for (int i = 0; i < 6; i++) {
      ```
    - In `applyThreadCheckPatches()` (after tc4, around line 1127), add tc5 and tc6:
      ```cpp
      // Thread-check 5: command 0x08 (type select, used by _LoadMasterRecsEX@16).
      PatchSiteDescriptor tc5 = m_resolver->resolvePatchSite("FUN_101d0c30_thread_check_INT3");
      m_threadCheckRvas[4] = tc5.patchRva;
      m_threadCheckOriginalBytes[4] = *reinterpret_cast<const unsigned char*>(
          m_resolver->baseAddress() + m_threadCheckRvas[4]);
      m_resolver->applyPatch("FUN_101d0c30_thread_check_INT3");
      m_patchesApplied = true;

      // Thread-check 6: command 0x09 (item retrieval, conditionally used by
      // _LoadMasterRecsEX@16 in headless/local mode). Applied defensively.
      PatchSiteDescriptor tc6 = m_resolver->resolvePatchSite("FUN_101d16c0_thread_check_INT3");
      m_threadCheckRvas[5] = tc6.patchRva;
      m_threadCheckOriginalBytes[5] = *reinterpret_cast<const unsigned char*>(
          m_resolver->baseAddress() + m_threadCheckRvas[5]);
      m_resolver->applyPatch("FUN_101d16c0_thread_check_INT3");
      m_patchesApplied = true;
      ```
    - Add a new SEH-protected call helper (before the `TsDevLib` methods, near `callLoadTextFileEx` around line 308):
      ```cpp
      // SEH-protected call to _LoadMasterRecsEX@16 (__stdcall, 4 args).
      struct LoadMasterRecsExSehResult {
          int result;  // -1 if SEH raised
      };
      bool callLoadMasterRecsEx(LoadMasterRecsExFn fn, int category, HGLOBAL* outBuf,
                                const char* path, int filterMode,
                                LoadMasterRecsExSehResult* out) {
          out->result = -1;
          __try {
              out->result = fn(category, outBuf, path, filterMode);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
              return false;
          }
          return true;
      }
      ```
    - Implement `enumerateRecords()` (after `getSource`, around line 705):
      ```cpp
      EnumerateResult TsDevLib::enumerateRecords(ComponentType type,
                                                   const std::string& searchName) {
          EnumerateResult result;

          if (!m_loadMasterRecsEx) {
              // Export not resolved — fail gracefully.
              return result;
          }

          int category = getCategory(type);
          int subType = getSubType(type);
          const char* path = getMyWorkBinPath();

          // NOTE: getMyWorkBinPath() returns the discovered MyWork\Bin path,
          // which matches the RE's example path for _LoadMasterRecsEX@16.
          // The exact path format expected by FUN_101a9650 (GetWarehouse) is
          // not 100% confirmed by the RE, but the call is SEH-protected via
          // callLoadMasterRecsEx below, so a wrong/invalid path fails
          // gracefully (returns {exists: false}) rather than crashing.

          // Call _LoadMasterRecsEX@16 with filterMode=0 (filtered mode).
          HGLOBAL buf = NULL;
          LoadMasterRecsExSehResult seh;
          if (!callLoadMasterRecsEx(m_loadMasterRecsEx, category, &buf,
                                    path, 0, &seh)) {
              // SEH exception — fail gracefully.
              return result;
          }
          int count = seh.result;
          if (count <= 0 || buf == NULL) {
              // No records or allocation failure — not found.
              if (buf) GlobalFree(buf);
              return result;
          }

          // Iterate records (each 0x10A = 266 bytes).
          char* records = (char*)GlobalLock(buf);
          if (!records) {
              GlobalFree(buf);
              return result;
          }

          const int kRecordSize = 0x10A;
          const int kNameOffset = 0x17;   // inline long name (max 60 chars + NUL)
          const int kProgIdOffset = 0x00; // short ProgId
          const int kSubTypeOffset = 0xE9; // category/subtype byte

          for (int i = 0; i < count; i++) {
              char* rec = records + i * kRecordSize;
              const char* storedName = rec + kNameOffset;

              // For TradingApp (subType 0x14), only match records with
              // rec[0xE9] == 0x14. For Indicator (subType 0), exclude
              // TradingApp records (rec[0xE9] != 0x14).
              if (category == 2) {
                  unsigned char recSubType = (unsigned char)rec[kSubTypeOffset];
                  if (subType == 0x14 && recSubType != 0x14) continue;
                  if (subType == 0x00 && recSubType == 0x14) continue;
              }

              if (_stricmp(storedName, searchName.c_str()) == 0) {
                  result.exists = true;
                  result.storedName = std::string(storedName);
                  result.progId = *(short*)(rec + kProgIdOffset);
                  break;
              }
          }

          GlobalUnlock(buf);
          GlobalFree(buf);
          return result;
      }
      ```
    - Update the comment block at the top of `applyThreadCheckPatches()` (lines 1087–1096 in `src/tsdev_lib.cpp`) to list the 2 new patch sites. (Note: an earlier draft referenced "around line 85-88" — that is incorrect; line 85-88 is in the middle of unrelated path-discovery code. The actual `applyThreadCheckPatches()` comment block is at lines 1087–1096.)
- **Dependencies:** F5-S1 (signatures + export must be in JSON), F5-S3 (RED confirmed).
- **Verification:** Visual inspection — the method, typedef, member, SEH helper, and patch applications are present. The thread-check arrays are [6]. The destructor loop is `i < 6`.
- **TDD note:** This is the GREEN implementation. The test from S2 should now compile and pass.

---

### F5-S5: Build + run C++ test to confirm GREEN [DONE]

- **Phase:** Tests (C++ library level)
- **Scope:** Build and run `test_exists_caseinsensitive`.
- **Dependencies:** F5-S4.
- **Verification:**
  1. `cmake --build build --config Release --target test_exists_caseinsensitive` — expect build SUCCESS.
  2. `build\Release\test_exists_caseinsensitive.exe` — expect exit code 0 (PASS).
  3. The test output should show: `PASS: enumerateRecords correctly finds item case-insensitively`.
- **TDD note:** GREEN = test compiles and passes (enumerateRecords finds the probe by uppercase name, returns verbatim stored name + ProgId, and correctly reports not-found for nonexistent names).

---

### F5-S6: Adversarial review — TsDevLib implementation [DONE]

- **Phase:** Review
- **Scope:** Independent reviewer checks F5-S4 (and F5-S1 if not already reviewed).
- **Dependencies:** F5-S5.
- **Verification:** Reviewer APPROVES or DISAPPROVES with findings. Check for:
  - Complete execution of S4 instructions (all code changes present).
  - No scope creep (no unrelated changes).
  - No drift (matches RE findings — name offset 0x17, ProgId offset 0x00, subtype filter 0xE9, filterMode=0).
  - No undue shortcuts (SEH protection present, GlobalLock/GlobalFree balanced, defensive handling of count <= 0 and NULL buf).
  - Thread-check array growth from [4] to [6] is consistent (declaration, destructor loop, applyThreadCheckPatches).
  - The 0x07 signature is in the JSON but NOT applied (correct — filterMode=0 doesn't need it).

---

### F5-S7: Write failing PowerShell E2E test for `exists --case-insensitive` (RED) [DONE]

- **Phase:** Tests (C++ CLI level)
- **Scope:** Edit 2 files (one coherent "add E2E test" change):
  - `tests/test_exists_caseinsensitive.ps1` (NEW):
    - Mirror the structure of `tests/test_exists.ps1` (same `Invoke-Cli`, `Invoke-Test`, `Print-Summary` helpers).
    - Test cases:
      - **D2.1** Insert a probe strategy `E2E_<runId>_CIProbe`, then `exists strategy E2E_<RUNID>_CIPROBE -i --json` (uppercase query). Expect: exit 0, JSON `exists: true`, `prog_id > 0`, `stored_name == "E2E_<runId>_CIProbe"` (verbatim).
      - **D2.2** `exists strategy E2E_<RUNID>_CIPROBE --json` (uppercase, NO `-i` flag). Expect: exit 0, JSON `exists: false` (exact-match fails for different case). This verifies the flag is actually needed.
      - **D2.3** `exists strategy NONEXISTENTNAMEXYZ -i --json`. Expect: exit 0, JSON `exists: false`, no `stored_name` field.
      - **D2.4** `exists strategy E2E_<runId>_CIProbe -i --json` (exact case, WITH `-i`). Expect: exit 0, JSON `exists: true`, `stored_name == "E2E_<runId>_CIProbe"`.
      - **D2.5** Delete the probe, then `exists strategy E2E_<RUNID>_CIPROBE -i --json`. Expect: exit 0, JSON `exists: false`.
    - Cleanup in `finally` block (delete probe if it exists).
  - `tests/run_regression.ps1`:
    - Add a new sub-suite entry for `test_exists_caseinsensitive.ps1` (mirror the `test_exists.ps1` entry: script path variable, Skip parameter, invocation in the main block). Add `-SkipExistsCI` switch parameter.
- **Dependencies:** F5-S5 (library method must work for the CLI to use it).
- **Verification:** Visual inspection — the test file and regression wiring exist.
- **TDD note:** This IS the RED test. The CLI doesn't recognize `-i`/`--case-insensitive` yet → the test will fail (RED).

---

### F5-S8: Build + run E2E test to confirm RED [DONE]

- **Phase:** Tests (C++ CLI level)
- **Scope:** Build `tsdev_cli` and run the E2E test.
- **Dependencies:** F5-S7.
- **Verification:**
  1. `cmake --build build --config Release` — expect build SUCCESS (the CLI hasn't changed yet, but the library has).
  2. `powershell -ExecutionPolicy Bypass -File tests\test_exists_caseinsensitive.ps1` — expect FAIL (the `-i` flag is not recognized; the CLI returns a usage error or ignores the flag; `stored_name` is not in the JSON output).
- **TDD note:** RED = E2E test fails because `--case-insensitive` is not recognized and `stored_name` is not emitted.

---

### F5-S9: Implement `--case-insensitive` flag + `stored_name` in CLI (GREEN) [DONE]

- **Phase:** CLI
- **Scope:** Edit 1 file:
  - `src/tsdev_cli.cpp`:
    - Add `storedName` field to `CliResult` struct (around line 197, after `sourceText`):
      ```cpp
      std::string storedName;  // "exists" op: verbatim stored name (case-insensitive)
      ```
    - Add `caseInsensitive` field to `ExistsParams` struct (around line 848):
      ```cpp
      bool           caseInsensitive = false;
      ```
    - Modify `doExists()` (around line 478) to accept a `bool caseInsensitive` parameter:
      ```cpp
      CliResult doExists(TsDevLib& lib, ComponentType type,
                         const std::string& typeStr, const std::string& name,
                         bool caseInsensitive) {
      ```
      - When `caseInsensitive` is true: call `lib.enumerateRecords(type, name)` instead of `lib.findExistPos(name, type)`. Populate `r.exists`, `r.progId`, `r.storedName` from the `EnumerateResult`.
      - When `caseInsensitive` is false: keep the existing `findExistPos` path. Set `r.storedName = name` when found (exact-match = query name IS the stored name). Leave `storedName` empty when not found.
      - Update human-readable output to include the stored name:
        - Found: `"EXISTS: <type> "<name>" found (ProgId=N, stored_name="<storedName>")\n"`
        - Not found: `"NOT FOUND: <type> "<name>" does not exist in the warehouse.\n"`
    - Modify `runExists()` (around line 855) to pass `p->caseInsensitive` to `doExists`:
      ```cpp
      p->result = doExists(*lib, p->type, p->typeStr, p->name, p->caseInsensitive);
      ```
    - Modify `cmdExists()` (around line 879) to accept a new `bool caseInsensitive` parameter (the flag is parsed in `main()`, NOT here — `cmdExists` receives only already-parsed `typeStr`/`name` strings and has no access to `argv`/`reducedArgc`):
      ```cpp
      int cmdExists(const std::string& typeStr, const std::string& name,
                    const wchar_t* tsPathOverride, bool jsonMode,
                    bool caseInsensitive) {
      ```
      - Set `params.caseInsensitive = caseInsensitive` in the `ExistsParams` initialization. Do NOT scan `argv` here.
    - Rewrite the `exists` dispatch in `main()` (around lines 1682–1697) to parse `-i`/`--case-insensitive` BEFORE dispatching, mirroring the existing `get-source` scan-and-extract pattern at lines 1716–1778 (which scans `reducedArgv` for `--output`, builds a positional-args vector, checks `size() == 2`, then calls `cmdGetSource`). The current `reducedArgc != 4` guard runs BEFORE `cmdExists` and rejects the extra `-i` arg (`reducedArgc` becomes 5 → "exists requires exactly 2 arguments" → exit 1), so it MUST be replaced with the scan-and-extract approach. New logic:
      ```cpp
      if (iequals(subcommand, "exists")) {
          // exists takes <type> <name> plus an optional -i/--case-insensitive
          // flag. The flag is exists-specific (not a global flag), so it
          // remains in reducedArgv and is parsed here (mirroring the
          // get-source --output pattern at lines 1716–1778). Valid forms:
          //   exists <type> <name>                  (reducedArgc == 4)
          //   exists <type> <name> -i               (reducedArgc == 5)
          //   exists <type> <name> --case-insensitive (reducedArgc == 5)
          bool caseInsensitive = false;
          std::vector<char*> exArgs;
          for (int i = 2; i < reducedArgc; i++) {
              std::string a = reducedArgvPtr[i];
              if (a == "-i" || a == "--case-insensitive") {
                  caseInsensitive = true;
              } else {
                  exArgs.push_back(reducedArgvPtr[i]);
              }
          }
          if (exArgs.size() != 2) {
              CliResult r;
              r.op = "exists";
              r.exitCode = 1;
              r.errorMessage = "exists requires exactly 2 arguments: <type> <name>";
              if (jsonMode) {
                  emitOutput(r, jsonMode);
              } else {
                  printUsage();
              }
              return 1;
          }
          return cmdExists(exArgs[0], exArgs[1],
                           tsPathWidePtr, jsonMode, caseInsensitive);
      }
      ```
      - The guard now accepts 2 positional args + the optional `-i`/`--case-insensitive` flag (any number of flag occurrences), NOT a fixed `reducedArgc != 4`. The positional count is checked via `exArgs.size() != 2` after flag extraction, exactly like `get-source` checks `gsArgs.size() != 2`.
    - Add `stored_name` to the JSON output in `emitOutput()` (around line 258, in the `r.op == "exists"` block):
      ```cpp
      if (r.op == "exists") {
          j["exists"] = r.exists;
          if (!r.storedName.empty()) j["stored_name"] = r.storedName;
      }
      ```
    - Update `printUsage()` (around line 127): change the exists line to:
      ```
      "  tsdev_cli [--json] [--ts-path <directory>] exists  <type> <name> [-i|--case-insensitive]\n"
      ```
    - Update the usage comment at the top of the file (around line 9):
      ```
      //   tsdev_cli [--json] [--ts-path <directory>] exists  <type> <name> [-i|--case-insensitive]
      ```
- **Dependencies:** F5-S8 (RED confirmed).
- **Verification:** Visual inspection — all changes present. The `-i`/`--case-insensitive` flag is parsed, `doExists` branches on it, `stored_name` is in JSON output.
- **TDD note:** This is the GREEN implementation. The E2E test from S7 should now pass.

---

### F5-S10: Build + run E2E test to confirm GREEN [DONE]

- **Phase:** Tests (C++ CLI level)
- **Scope:** Build `tsdev_cli` and run the E2E test.
- **Dependencies:** F5-S9.
- **Verification:**
  1. `cmake --build build --config Release` — expect build SUCCESS.
  2. `powershell -ExecutionPolicy Bypass -File tests\test_exists_caseinsensitive.ps1` — expect ALL TESTS PASSED (exit 0).
  3. Verify: D2.1 (case-insensitive found, stored_name correct), D2.2 (exact-match fails for different case), D2.3 (nonexistent not found), D2.4 (exact case + -i works), D2.5 (after delete, not found).
- **TDD note:** GREEN = all E2E test cases pass.

---

### F5-S11: Adversarial review — CLI implementation [DONE]

- **Phase:** Review
- **Scope:** Independent reviewer checks F5-S9.
- **Dependencies:** F5-S10.
- **Verification:** Reviewer APPROVES or DISAPPROVES with findings. Check for:
  - Complete execution of S9 instructions (all code changes present).
  - No scope creep (only `exists` subcommand modified, no other subcommands affected).
  - No drift (matches the design decisions: exact-match returns query name as stored_name, case-insensitive uses enumeration).
  - The `-i`/`--case-insensitive` flag parsing is robust (doesn't interfere with `--json` or `--ts-path` pre-scan).
  - `stored_name` is only emitted in JSON when non-empty (not found → omitted).
  - Human-readable output includes stored_name for case-insensitive found.
  - Exit codes unchanged (0 for both found and not-found; 2 for unknown type).

---

### F5-S12: Write failing Python tests for `case_insensitive` exists (RED) [DONE]

- **Phase:** Tests (Python)
- **Scope:** Edit 1 file:
  - `tests/test_tsdev_client.py`:
    - Add a `_exists_ci_json` helper (near `_exists_json`, around line 287):
      ```python
      def _exists_ci_json(type_: str, name: str, exists: bool, prog_id: int | None = None,
                          stored_name: str | None = None) -> str:
          obj: dict = {"op": "exists", "type": type_, "name": name, "success": True, "exit_code": 0, "exists": exists}
          if prog_id is not None:
              obj["prog_id"] = prog_id
          if stored_name is not None:
              obj["stored_name"] = stored_name
          return json.dumps(obj)
      ```
    - Add tests to the `TestExists` class (around line 339):
      - `test_exists_case_insensitive_found`: mock returns `_exists_ci_json("strategy", "MyStrat", True, prog_id=1234, stored_name="MyStrat")`. Call `client.exists("strategy", "MYSTRAT", case_insensitive=True)`. Assert `r.exists is True`, `r.prog_id == 1234`, `r.stored_name == "MyStrat"`. Assert `seen["cmd"]` contains `"-i"` (or `"--case-insensitive"`).
      - `test_exists_case_insensitive_not_found`: mock returns `_exists_ci_json("strategy", "Nope", False)`. Call `client.exists("strategy", "NOPE", case_insensitive=True)`. Assert `r.exists is False`, `r.prog_id == 0`, `r.stored_name == ""` (or `None`).
      - `test_exists_case_insensitive_flag_not_passed_by_default`: call `client.exists("strategy", "MyStrat")` (no `case_insensitive`). Assert `"-i"` NOT in `seen["cmd"]`.
    - Add tests to `TestCliNewSubcommands` class (around line 772):
      - `test_cli_exists_case_insensitive`: FakeClient.exists accepts `case_insensitive` param. Invoke `_main(["exists", "-i", "strategy", "MyStrat"])`. Assert `calls["exists"] == ("strategy", "MyStrat")` with `case_insensitive=True`. Assert output mentions stored_name.
      - `test_cli_exists_case_insensitive_long_flag`: same but with `--case-insensitive`.
- **Dependencies:** F5-S11 (CLI impl reviewed).
- **Verification:** Visual inspection — test methods present.
- **TDD note:** This IS the RED test. `TsDevClient.exists` doesn't accept `case_insensitive` param yet → `TypeError`. `TsDevResult` doesn't have `stored_name` field → `AttributeError`. Python CLI doesn't parse `-i` → assertion failure.

---

### F5-S13: Run Python tests to confirm RED [DONE]

- **Phase:** Tests (Python)
- **Scope:** Run the new Python tests.
- **Dependencies:** F5-S12.
- **Verification:**
  1. `python -m pytest tests/test_tsdev_client.py -k "case_insensitive or CaseInsensitive" -x` — expect FAILURES (TypeError for `case_insensitive` param, AttributeError for `stored_name`).
- **TDD note:** RED = Python tests fail because the wrapper and CLI don't support `case_insensitive` yet.

---

### F5-S14: Implement `case_insensitive` in Python wrapper (GREEN) [DONE]

- **Phase:** Python wrapper
- **Scope:** Edit 3 files (one coherent "Python case-insensitive exists" change):
  - `src/tsdev_automation/results.py`:
    - Add `stored_name` field to `TsDevResult` (around line 57, after `source`):
      ```python
      stored_name: str = ""
      ```
    - In `from_json()` (around line 85), add:
      ```python
      stored_name=str(obj.get("stored_name", "")),
      ```
  - `src/tsdev_automation/client.py`:
    - Modify `exists()` (around line 152) to accept `case_insensitive`:
      ```python
      def exists(self, type: str, name: str, *, case_insensitive: bool = False) -> TsDevResult:
      ```
    - Change the implementation to use a custom command builder (like `get_source` does) instead of `_single`, because `_single` doesn't support extra flags:
      ```python
      t = _validate_type(type)
      sub = _SINGLE_SUBCMD["exists"]
      cmd = self._base_cmd() + ["--json", sub, t, name]
      if case_insensitive:
          cmd.append("-i")
      result = self._run_cli(cmd)
      if not result.success:
          assert result.error is not None
          raise exception_for(result)
      return result
      ```
    - Update the docstring to mention `case_insensitive` and `stored_name`.
  - `src/tsdev_automation/_cli.py`:
    - Add `-i`/`--case-insensitive` flag to the `exists` subparser (around line 66):
      ```python
      sp = sub.add_parser("exists", help="query whether a component exists.")
      sp.add_argument("type", help="strategy | function | indicator | tradingapp")
      sp.add_argument("name", help="component name")
      sp.add_argument("-i", "--case-insensitive", action="store_true", default=False,
                      help="perform case-insensitive name matching")
      ```
    - Modify the `exists` dispatch (around line 100) to pass `case_insensitive`:
      ```python
      if args.command == "exists":
          r = client.exists(args.type, args.name, case_insensitive=args.case_insensitive)
          if r.exists:
              extra = f", stored_name={r.stored_name!r}" if r.stored_name else ""
              print(f"[OK] exists {r.type} {r.name!r} -> ProgId={r.prog_id}{extra}")
          else:
              print(f"[OK] exists {r.type} {r.name!r} -> not found")
          return 0
      ```
- **Dependencies:** F5-S13 (RED confirmed).
- **Verification:** Visual inspection — all 3 files modified, `case_insensitive` param and `stored_name` field present.
- **TDD note:** This is the GREEN implementation. The Python tests from S12 should now pass.

---

### F5-S15: Run Python tests to confirm GREEN [DONE]

- **Phase:** Tests (Python)
- **Scope:** Run the Python test suite.
- **Dependencies:** F5-S14.
- **Verification:**
  1. `python -m pytest tests/test_tsdev_client.py -k "case_insensitive or CaseInsensitive" -x` — expect ALL PASS.
  2. `python -m pytest tests/test_tsdev_client.py` — expect ALL PASS (no regressions in existing tests).
- **TDD note:** GREEN = all Python tests pass, including new case-insensitive tests and existing exists tests.

---

### F5-S16: Adversarial review — Python implementation [DONE]

- **Phase:** Review
- **Scope:** Independent reviewer checks F5-S14.
- **Dependencies:** F5-S15.
- **Verification:** Reviewer APPROVES or DISAPPROVES with findings. Check for:
  - Complete execution of S14 instructions (all 3 files modified).
  - No scope creep (only `exists` method and `stored_name` field changed).
  - `case_insensitive` is keyword-only (`*,` separator) to avoid breaking positional compat.
  - `stored_name` defaults to `""` (not `None`) for consistency with `source`.
  - `from_json` correctly reads `stored_name` from the JSON object.
  - Python CLI dispatch passes `case_insensitive` to `client.exists`.
  - Existing exists tests still pass (no regression).

---

### F5-S17: Run full regression suite [DONE]

- **Phase:** Tests (regression)
- **Scope:** Run the complete regression suite.
- **Dependencies:** F5-S16.
- **Verification:**
  1. `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` — expect ALL PASS.
  2. Verify: sigtool tests (16 signatures), unit tests (including new `test_exists_caseinsensitive`), E2E tests, exists E2E tests, new case-insensitive E2E tests.
  3. No regressions in any existing test.
- **TDD note:** N/A (regression verification).

---

### F5-S18: Update E2E_TEST_SPEC.md with case-insensitive exists test cases [DONE]

- **Phase:** Docs
- **Scope:** Edit 1 file:
  - `docs/E2E_TEST_SPEC.md`:
    - Update the `exists` bullet in Part D (around line 409) to reflect the implemented case-insensitive variant.
    - Add a new subsection (e.g., "Part D.1: exists --case-insensitive") with structured test cases:
      - D2.1: Case-insensitive match (uppercase query, mixed-case stored name) — expect exists=true, stored_name=verbatim, prog_id>0.
      - D2.2: Exact-match fails for different case (no -i flag) — expect exists=false.
      - D2.3: Case-insensitive nonexistent — expect exists=false.
      - D2.4: Case-insensitive exact case — expect exists=true, stored_name=query name.
      - D2.5: Case-insensitive after delete — expect exists=false.
    - Follow the existing Part A/B/C structure (test ID, type, name, expected exit, procedure, cleanup).
- **Dependencies:** F5-S17 (all tests passing).
- **Verification:** Visual inspection — the spec is updated with the new test cases.
- **TDD note:** N/A (documentation).

---

### F5-S19: Adversarial review — docs [DONE]

- **Phase:** Review
- **Scope:** Independent reviewer checks F5-S18.
- **Dependencies:** F5-S18.
- **Verification:** Reviewer APPROVES or DISAPPROVES. Check for:
  - Complete execution of S18 instructions.
  - Test cases match the actual implementation behavior.
  - No scope creep (only the exists case-insensitive section added).
  - Follows existing Part A/B/C structure.

---

### F5-S20: E2E CLI verification (MANDATORY after TVS) [DONE]

- **Phase:** E2E verification
- **Scope:** Run a manual end-to-end test with the real production CLI.
- **Dependencies:** F5-S19.
- **Verification:**
  1. Build: `cmake --build build --config Release`
  2. Insert a probe: `build\Release\tsdev_cli.exe insert strategy E2E_F5_TestProbe <source_file>` — expect exit 0.
  3. Case-insensitive exists: `build\Release\tsdev_cli.exe --json exists strategy e2e_f5_testprobe -i` — expect JSON with `exists: true`, `stored_name: "E2E_F5_TestProbe"`, `prog_id > 0`.
  4. Exact-match exists (different case, no -i): `build\Release\tsdev_cli.exe --json exists strategy e2e_f5_testprobe` — expect JSON with `exists: false`.
  5. Exact-match exists (exact case): `build\Release\tsdev_cli.exe --json exists strategy E2E_F5_TestProbe` — expect JSON with `exists: true`, `stored_name: "E2E_F5_TestProbe"`, `prog_id > 0`.
  6. Cleanup: `build\Release\tsdev_cli.exe delete strategy E2E_F5_TestProbe` — expect exit 0.
  7. Also run at least one insert/delete cycle for another type (e.g., Function) to confirm no regression.
  8. The E2E CLI test must PASS before the TVS is considered fully done.
- **TDD note:** N/A (final verification).

---

## 5. Risks and Mitigations (from RE findings)

### 5.1 Command 0x09 necessity in headless mode (RE §8.2)

**Risk:** `FUN_101e10d0` (item retrieval) calls `FUN_101d16c0` (command 0x09) when items are not cached. In a fresh headless session, items may not be cached, requiring command 0x09. If the INT3 patch is not applied, the process crashes.

**Mitigation:** The 0x09 patch (`FUN_101d16c0_thread_check_INT3`) is applied defensively in `applyThreadCheckPatches()` (tc6), regardless of whether command 0x09 is actually triggered. This is harmless (NOP) if not triggered, and prevents a crash if it is. The RE notes that Frida confirmation is needed for whether 0x09 is triggered, but applying the patch defensively eliminates the risk without needing confirmation.

### 5.2 Warehouse mode — server vs. local (RE §8.3)

**Risk:** `FUN_101d0540` checks `[warehouse+0xC2]`. In server mode (`== 1`), command 0x09 is not used. In local mode (`!= 1`), it is. The project's headless scenario is likely local mode, but this is unconfirmed.

**Mitigation:** Same as 5.1 — the 0x09 patch is applied defensively. Whether the warehouse is in server or local mode, the patch is harmless if 0x09 is not triggered, and protective if it is.

### 5.3 Empty warehouse / no .MST files (RE §8.4)

**Risk:** If the warehouse is not properly initialized, `_LoadMasterRecsEX@16` returns 0 (filtered mode) or allocates a default-count buffer of hidden records (unfiltered mode). No `.MST` files exist on disk.

**Mitigation:** The implementation uses `filterMode=0` (filtered mode). If `_LoadMasterRecsEX@16` returns `<= 0`, the method returns `{exists: false}` immediately (no iteration). If it returns a positive count but all records are hidden/filtered, the loop simply finds no match and returns `{exists: false}`. The `GlobalFree` is called in all paths (including the `count <= 0` path if `buf != NULL`).

### 5.4 Record count from DAT_10488990 (RE §8.5)

**Risk:** When the warehouse is unavailable and `filterMode != 0`, `_LoadMasterRecsEX@16` reads a default count of 1000 from `DAT_10488990` and allocates a buffer of hidden records.

**Mitigation:** The implementation uses `filterMode=0`, which does NOT trigger this fallback path. With `filterMode=0` and an unavailable warehouse, the function returns 0 (no records). The 0x07 patch (for `FUN_101d0d90`, which is only called when `filterMode != 0`) is NOT applied, but this is safe because we never use `filterMode != 0`.

### 5.5 Name offset reliability (RE §8.1)

**Risk:** Reading the name from the wrong offset would always fail to match.

**Mitigation:** The implementation reads the name from offset `0x17` (inline long name), which the RE confirms is the RELIABLE and correct read location for ALL records from `_LoadMasterRecsEX@16`. Offset `0x02` is explicitly avoided (it is overwritten with the ProgId by lazy-init `FUN_101c5b10`). No Frida confirmation is needed for this — it is a static finding confirmed by Ghidra decompilation.

### 5.6 TradingApp/Indicator category overlap (RE §3.2)

**Risk:** Category 2 (Indicator) enumeration includes TradingApp records (expanded type `0x1402`). Without filtering, a case-insensitive search for an Indicator might match a TradingApp of the same name, or vice versa.

**Mitigation:** The `enumerateRecords` method filters by `rec[0xE9]` (subtype byte) when the category is 2:
- For `ComponentType::TradingApp` (subType `0x14`): only match records where `rec[0xE9] == 0x14`.
- For `ComponentType::Indicator` (subType `0x00`): only match records where `rec[0xE9] != 0x14` (exclude TradingApps).
- For other categories (0, 4): no subtype filter needed (single expanded type per category).

### 5.7 SEH / GlobalLock / GlobalFree balance

**Risk:** If `_LoadMasterRecsEX@16` raises an SEH exception, or if `GlobalLock` fails, the buffer might leak.

**Mitigation:** The `callLoadMasterRecsEx` SEH helper wraps the call. If SEH is raised, the method returns `{exists: false}` immediately (no buffer to free). If the call succeeds but returns `count <= 0`, `GlobalFree` is called if `buf != NULL`. If `GlobalLock` fails, `GlobalFree` is called before returning. In the normal path, `GlobalUnlock` + `GlobalFree` are called after iteration. All paths are covered.

### 5.8 Defensive failure on missing export/patches

**Risk:** If `_LoadMasterRecsEX@16` cannot be resolved (export not found), or if the 0x08/0x09 patches cannot be applied (signature mismatch), the enumeration will fail or crash.

**Mitigation:**
- If `m_loadMasterRecsEx == nullptr`, `enumerateRecords` returns `{exists: false}` immediately (defensive check at the top of the method).
- If the 0x08/0x09 patches fail to apply, the `TsDevLib` constructor throws `OrlibResolverError` (via `applyPatch`), which prevents the CLI from proceeding. The CLI catches this and emits exit code 4 (init_failure) with a clear error message. This is the existing pattern for all patch failures.

---

## 6. Files Touched (Summary)

| File | Subtask | Change |
|------|---------|--------|
| `signatures/v9.50.01.3344.json` | S1 | +3 signatures, +1 export |
| `tools/sigcodegen.py` | S1 | Bump EXPECTED_SIGNATURE_COUNT 13→16, EXPECTED_EXPORT_COUNT 8→9 |
| `tests/test_sigtool.ps1` | S1 | Bump $expectedSignatureCount 13→16, update '13/13'→'16/16' regexes (lines 173, 271), update count comments |
| `tests/test_exists_caseinsensitive.cpp` | S2 | NEW — C++ integration test for enumerateRecords |
| `CMakeLists.txt` | S2 | Add test_exists_caseinsensitive target |
| `tests/run_unit_tests.ps1` | S2 | Add test_exists_caseinsensitive to inventory |
| `src/tsdev_lib.h` | S4 | +EnumerateResult struct, +enumerateRecords decl, +LoadMasterRecsExFn typedef, +m_loadMasterRecsEx member, thread-check arrays [4]→[6] |
| `src/tsdev_lib.cpp` | S4 | +resolveExport, +tc5/tc6 patches, +callLoadMasterRecsEx SEH helper, +enumerateRecords impl, destructor loop 4→6 |
| `tests/test_exists_caseinsensitive.ps1` | S7 | NEW — E2E CLI test for exists -i |
| `tests/run_regression.ps1` | S7 | Add case-insensitive exists sub-suite |
| `src/tsdev_cli.cpp` | S9 | +storedName field, +caseInsensitive in ExistsParams, modify doExists/cmdExists/runExists, +stored_name in JSON, update usage |
| `tests/test_tsdev_client.py` | S12 | +case_insensitive tests (unit + CLI) |
| `src/tsdev_automation/results.py` | S14 | +stored_name field in TsDevResult |
| `src/tsdev_automation/client.py` | S14 | +case_insensitive param in exists() |
| `src/tsdev_automation/_cli.py` | S14 | +-i/--case-insensitive flag in exists subparser |
| `docs/E2E_TEST_SPEC.md` | S18 | +Part D.1 case-insensitive exists test cases |

---

## 7. Adversarial Review Summary

| Subtask | Review ID | What to review |
|---------|-----------|----------------|
| S1 (signatures + codegen) | S6 (combined with S4 review) | Signature JSON entries match RE doc exactly; counts bumped correctly |
| S4 (TsDevLib impl) | S6 | Complete execution, no scope creep, RE compliance, defensive handling |
| S9 (CLI impl) | S11 | Flag parsing, JSON output, human output, no regressions |
| S14 (Python impl) | S16 | Keyword-only param, stored_name field, CLI dispatch |
| S18 (docs) | S19 | Test cases match implementation, follows existing structure |

---

## 8. Record Layout Reference (from RE §4)

| Offset | Size | Field | Read strategy |
|--------|------|-------|---------------|
| `0x00` | 2 | ProgId (`short`) | `*(short*)(rec + 0x00)` |
| `0x02` | 4 | **DO NOT READ** — overwritten with ProgId (int) by lazy-init | N/A |
| `0x17` | 61 | **Long name** (NUL-terminated, inline) | `rec + 0x17` — ALWAYS read name from here |
| `0x73` | 1 | Name storage type | Irrelevant for raw records |
| `0x79` | 1 | Hidden flag (bit 0) | Filtered by `_LoadMasterRecsEX@16` (filterMode=0) |
| `0xD8` | 1 | Hidden/deleted flag (bit 1) | Filtered by `_LoadMasterRecsEX@16` (filterMode=0) |
| `0xE9` | 1 | Category/subtype (`byte`) | `rec[0xE9]` — filter TradingApp (0x14) vs Indicator (!=0x14) |

Record size: `0x10A` = 266 bytes.
