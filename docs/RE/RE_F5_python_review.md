# Adversarial Review: Python `case_insensitive` exists implementation (TVS-F.5)

**Reviewer:** Independent adversarial subagent
**Date:** 2025-01-30
**Files reviewed:**
- `src/tsdev_automation/client.py` — `TsDevClient.exists()` method (lines 152–176)
- `src/tsdev_automation/results.py` — `TsDevResult.stored_name` field (lines 57, 86)
- `src/tsdev_automation/_cli.py` — `--case-insensitive` flag and dispatch (lines 69–74, 106–112)
- `tests/test_tsdev_client.py` — new `TestExists` tests and E2E test (lines 362–434, 1200–1242)
- `src/tsdev_cli.cpp` — C++ CLI `stored_name` JSON emission (line 264) and `doExists` logic (lines 501–536)

**Method:** Read all modified files, ran `git diff HEAD`, verified C++ CLI JSON schema,
ran unit tests (`uv run pytest -m "not e2e"` → 50 passed, 10 deselected).

---

## Verdict: APPROVE

The implementation is correct, clean, well-tested, and backward-compatible.
No blocking issues were found. The findings below are minor recommendations only.

---

## Findings

### 1. [LOW] `case_insensitive` is keyword-only — correct and well-chosen

The signature `def exists(self, type: str, name: str, *, case_insensitive: bool = False)`
uses `*` to force `case_insensitive` as keyword-only. This is the right choice: it is a
behavioral modifier flag, not a primary positional argument, and keyword-only prevents
positional confusion with future parameters. This is actually *more* correct than
`get_source`'s `output_file` parameter, which is technically positional-or-keyword (no `*`
separator). The inconsistency is minor and in the safe direction.

### 2. [INFO] Default `False` maintains backward compatibility

The previous implementation called `self._single("exists", type, name, None)`, which built
the command `--json exists <type> <name>` with no extra flags. The new default
(`case_insensitive=False`) produces the identical command — no `--case-insensitive` flag
is appended. The inlining of the logic (no longer delegating to `_single`) is equivalent
because `exists` never takes a source file, so the cleanup machinery in `_single` was a
no-op. Verified by `git diff` and by the `test_exists_case_insensitive_default_false_no_flag`
unit test.

### 3. [INFO] Return type and fields are correct

`TsDevResult` now includes:
- `exists: bool = False` (pre-existing)
- `stored_name: str = ""` (newly added)
- `prog_id: int | None = None` (pre-existing, defaults to 0 via `from_json` when absent)

`from_json` correctly parses all three:
- `exists=bool(obj.get("exists", False))`
- `stored_name=str(obj.get("stored_name", ""))`
- `prog_id=obj.get("prog_id") if obj.get("prog_id") is not None else 0`

The C++ CLI (`tsdev_cli.cpp` line 264) emits `j["stored_name"] = r.storedName` for the
`exists` op, and emits `prog_id` only when `r.progId > 0` (line 259). The Python side
handles the absent `prog_id` by defaulting to 0. The schema is consistent end-to-end.

### 4. [INFO] CLI integration is correct

- `--case-insensitive` is registered as `action="store_true", default=False` on the
  `exists` subparser (`_cli.py` lines 69–74). Argparse converts the hyphenated flag to
  `args.case_insensitive` (underscore).
- Dispatch forwards it: `client.exists(args.type, args.name, case_insensitive=args.case_insensitive)`
  (`_cli.py` line 107).
- The CLI output for a found item now includes `stored_name={r.stored_name!r}`
  (`_cli.py` line 109).
- Error handling: if the CLI returns `success=False`, `exists()` raises via
  `exception_for(result)`, mapping error codes to typed exceptions — same pattern as all
  other methods.

### 5. [LOW] Unit tests are comprehensive but missing one edge case

The `TestExists` class covers:
- Found (exact match) with prog_id and command tail verification
- Not-found (no raise, prog_id=0)
- ts_path pass-through
- No source file appended
- `stored_name` parsed from JSON (found case)
- `stored_name` empty when not found
- `case_insensitive=True` appends `--case-insensitive` flag
- `case_insensitive=False` (default) does not append the flag
- `case_insensitive=True` with not-found result

**Gap:** There is no unit test for `exists()` when the CLI returns `success=False` (a real
CLI error, e.g., `init_failure` or `usage_error`). The error-handling path
(`if not result.success: raise exception_for(result)`) is shared with all other methods
and is tested for `insert`/`modify`/`delete`, so the risk is low. A dedicated test like
`test_exists_init_failure_raises` would close this gap.

### 6. [LOW] CLI dispatch tests do not verify `stored_name` in output

`test_cli_exists_found` and `test_cli_exists_case_insensitive_flag` assert that the output
contains the name and prog_id, but do not assert that `stored_name` appears in the
human-readable output. The `_cli.py` code prints
`stored_name={r.stored_name!r}` for found items, but no test verifies this string is
present. Adding `assert "stored_name" in out` to these tests would close the gap.

### 7. [INFO] E2E test is well-designed with proper cleanup

`test_exists_case_insensitive_e2e` covers the three key scenarios:
1. Case-insensitive query (uppercase) matches lowercase item → `exists=True`,
   `stored_name` = verbatim lowercase name, `prog_id > 0`
2. Case-sensitive query (uppercase) does NOT match lowercase item → `exists=False`
3. Case-sensitive query (exact lowercase) matches → `exists=True`, `stored_name` = name

Cleanup is proper: the test uses `try/finally` to delete the component, and the autouse
`_real_client` fixture sweeps `"zzz_py_ci_probe"` across all 4 types. The name is also
included in `_TEST_NAMES` (line 1059) so `test_no_orphans_after_suite` verifies no
orphan remains.

### 8. [INFO] E2E test depends on C++ implementation detail (`{ name }` comment block)

The E2E test inserts source with a `{ zzz_py_ci_probe }` comment block, which is the
convention used by `enumerateRecords` Phase 2 to extract the verbatim stored name. This
is a C++ implementation detail leaking into the Python test. The dependency is documented
in the test docstring, so it is acceptable, but if the C++ enumeration strategy changes,
this test may need updating.

### 9. [INFO] Code duplication between `exists()` and `_single()` is minimal and justified

The `exists()` method inlines the command-building and error-handling logic that was
previously in `_single()`, rather than extending `_single()` with an optional
`case_insensitive` parameter. This avoids polluting `_single()` with an
exists-specific parameter and keeps the method self-contained. The duplication is limited
to 3 lines (validate, build cmd, raise on error) and is acceptable.

### 10. [INFO] `_exists_json` test helper is well-designed

The helper correctly models the C++ CLI schema:
- `prog_id` is omitted from JSON when the item is not found (matching C++ behavior)
- `stored_name` defaults to `name` when `exists=True` and `stored_name` is not explicitly
  passed (matching exact-match behavior), and `""` when `exists=False`
- Case-insensitive tests explicitly pass a different `stored_name` to verify it is
  surfaced correctly

---

## Required fixes

None. The implementation is correct and all tests pass.

---

## Recommendations (optional, non-blocking)

1. **Add a unit test for `exists()` with a CLI error** (e.g., `init_failure` or
   `usage_error` returning `success=False`) to explicitly verify the
   `exception_for(result)` path for the `exists` method.

2. **Add `assert "stored_name" in out`** to `test_cli_exists_found` and
   `test_cli_exists_case_insensitive_flag` to verify the human-readable CLI output
   includes the `stored_name` field for found items.

3. **Consider making `output_file` keyword-only in `get_source()`** for consistency with
   `exists()`'s `case_insensitive` — though this is a separate concern and should not
   be part of this TVS.
