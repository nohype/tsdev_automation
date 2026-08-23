# Workstream G: Distributable Bundle — Plan

> **Status:** APPROVED (adversarial review: APPROVED, user-approved 2026-08-20). Ready for implementation.
> **Date:** 2026-08-20

## 1. Goal

Produce a single root-level output directory (`dist/`) containing both the C++ CLI (`tsdev_cli.exe`) and the Python wrapper package (`tsdev_automation/`), plus an automatically generated timestamped zip archive in `artifacts/`, so that users can:

- **(a) Build from source:** Clone the repo, run the standard CMake build, and automatically get a populated `dist/` directory AND a ready-to-distribute zip in `artifacts/` at the repo root (both gitignored).
- **(b) Receive pre-built:** Be given the zip from `artifacts/` (or the `dist/` directory), extract it, and use it directly — no build step, no repo checkout required.

From `dist/`, users can:
- Run the C++ CLI directly: `dist\tsdev_cli.exe insert ...`
- Import the Python wrapper in their own projects: add `dist/` to `sys.path` or `PYTHONPATH`, then `from tsdev_automation import TsDevClient`.

## 2. Current State (from investigations)

### C++ CLI
- **Build system:** CMake + Visual Studio generator (`-A Win32`), 32-bit only, C++17.
- **Build commands:** `cmake -S . -B build -A Win32` then `cmake --build build --config Release`.
- **Output:** `build\Release\tsdev_cli.exe` (single self-contained static exe, ~248 KB). No DLLs, no JSONs, no signature files copied next to it.
- **Runtime deps:** `orlib20.dll` + 7 support DLLs loaded dynamically from the TradeStation 9.5 install directory (via `SetDllDirectoryW` + `LoadLibraryW`). Resolved by `--ts-path` override → registry → `%ProgramFiles(x86)%\TradeStation 9.5` default. **Nothing is bundled.**
- **Signature JSONs:** Build-time only (compiled into the exe via `signatures.gen.h`). No runtime JSON dependency.
- **No C++ distribution/packaging exists.** No `packaging/`, `release/`, `scripts/`, `.github/`, or `INSTALL*`. The only distribution path is build-from-source.

### Python wrapper
- **Location:** `src/tsdev_automation/` (6 `.py` files: `__init__.py`, `__main__.py`, `_cli.py`, `client.py`, `exceptions.py`, `results.py`).
- **Runtime deps:** **stdlib-only** (`json`, `os`, `subprocess`, `tempfile`, `dataclasses`, `pathlib`, `typing`). The declared `capstone>=5.0.9` dependency is **not used by the wrapper** and is **not used by `tools/sigcodegen.py`** (which is explicitly stdlib-only). It is only imported by historical RE probe scripts under `tmp/re_artifacts/` (gitignored, not part of any distributed artifact). It is therefore a stale/misdeclared runtime dependency that should be removed (see TVS-G.4).
- **Python version:** `>=3.14` (64-bit required — wrapper drives 32-bit CLI as subprocess).
- **Packaging:** `pyproject.toml` with `uv_build` backend. pip-installable (`pip install .` or `uv sync`). Wheel exists at `dist/tsdev_automation-0.1.0-py3-none-any.whl` but does NOT bundle the exe.
- **CLI exe resolution (`_resolve_cli_path()` in `client.py`):**
  1. Explicit `cli_path` arg to `TsDevClient.__init__`
  2. `TSDEV_CLI_PATH` env var
  3. `__file__`-relative walk-up: walks up from `client.py` looking for `CMakeLists.txt`, then returns `<that_dir>/build/Release/tsdev_cli.exe`
  4. Fallback: `here.parents[1] / "build/Release/tsdev_cli.exe"`
  - **Problem:** Auto-discovery is tied to the CMake source-tree layout. It breaks when the package is copied to a bundle directory or installed as a wheel into site-packages.

### `.gitignore`
- Root `.gitignore` ignores: `build/`, `/build*/`, `/ghidra_project/`, `/tmp/`, `__pycache__/`, `*.pyc`, `.venv/`, `*.log`, `*.obj`, `*.tsc`.
- `dist/.gitignore` contains `*` (ignores all contents of `dist/`). The `dist/.gitignore` file itself is committed, so `dist/` exists in a fresh clone but is empty.
- `/dist/` is NOT in root `.gitignore`.

## 3. Design Decisions

### D1: Output directory = `dist/` (root-level)
- `dist/` already exists, is gitignored (via `dist/.gitignore` = `*`), and is conventional for distribution output.
- The bundle will coexist with Python wheel artifacts if those are also built. For the zip use case (b), the user zips `dist/` — the wheel is small (~14 KB) and harmless to include; alternatively, the zip command can be selective.

### D2: Bundle contents (minimal)
```
dist/
├── tsdev_cli.exe              # The C++ CLI (static, self-contained)
├── tsdev_automation/          # The Python wrapper package
│   ├── __init__.py
│   ├── __main__.py
│   ├── _cli.py
│   ├── client.py
│   ├── exceptions.py
│   └── results.py
└── README.md                  # Bundle-specific quick-start guide
```
- **Excluded:** `sigtool.exe` (developer tool, not for end users), `*.lib` (static libs, not runtime), `test_*.exe` (tests), `signatures/*.json` (build-time only), `pyproject.toml` (users import via sys.path, not pip install — keeps it minimal), `__pycache__/` (build artifact).
- **Not pip-installable:** The bundle is intentionally NOT a pip-installable package (no `pyproject.toml`). Users import via `sys.path` / `PYTHONPATH` / `cd`. This keeps the bundle minimal and avoids implying third-party deps (the wrapper is stdlib-only). Users who want pip-install can use the repo root (`pip install .`).
- **No DLLs bundled.** The CLI loads orlib20.dll from the TradeStation install at runtime. This is by design (AGENTS.md: TradeStation install paths are READ-ONLY and must not be modified; we rely on the existing install).

### D3: Assembly mechanism = CMake POST_BUILD custom command
- Add a `POST_BUILD` custom command on the `tsdev_cli` target that copies:
  - `$<TARGET_FILE:tsdev_cli>` → `${CMAKE_SOURCE_DIR}/dist/tsdev_cli.exe` (via `cmake -E copy_if_different`)
  - `src/tsdev_automation/*.py` → `${CMAKE_SOURCE_DIR}/dist/tsdev_automation/*.py` (selective `.py` glob + per-file `copy_if_different`)
  - `docs/BUNDLE_README.md` → `${CMAKE_SOURCE_DIR}/dist/README.md` (via `copy_if_different`)
- **Zero extra build steps.** Running `cmake --build build --config Release` automatically produces/updates `dist/`.
- CMake creates `dist/` and `dist/tsdev_automation/` via `file(MAKE_DIRECTORY ...)` at configure time (so the directory exists even before the first build, and the POST_BUILD copy succeeds).
- Uses `copy_if_different` to avoid unnecessary timestamp updates on unchanged files.
- **Implementation note:** The POST_BUILD writes into `${CMAKE_SOURCE_DIR}/dist` (the source tree), NOT the build tree. This is intentional — the goal is a root-level `dist/` that is gitignored and can be zipped for distribution. A one-line comment in `CMakeLists.txt` should explain this so future maintainers don't "fix" it to the build dir.
- **CMake syntax note (for implementer):** Expanding a GLOB'd list inside a POST_BUILD `foreach` can be fiddly. Prefer `cmake -E copy_if_different` with explicit source/dest pairs, or generate the command list via `add_custom_command` with multiple `COMMAND` lines, or use a small CMake helper function. The exact mechanism is an implementation detail left to the implementer.

### D4: Python wrapper bundle-layout auto-discovery
- Modify `_resolve_cli_path()` in `src/tsdev_automation/client.py` to add a new resolution step **before** the CMakeLists.txt walk-up:
  - **Bundle layout:** Check if `tsdev_cli.exe` exists in `Path(__file__).resolve().parent.parent` (the directory containing the `tsdev_automation/` package folder). If yes, return it.
  - This handles: `dist/tsdev_automation/client.py` → `dist/tsdev_cli.exe` (bundle layout).
  - Also handles: `src/tsdev_automation/client.py` → `src/tsdev_cli.exe` (source layout — doesn't exist, falls through to next step).
- **New resolution priority:**
  1. Explicit `cli_path` arg
  2. `TSDEV_CLI_PATH` env var
  3. **Bundle layout:** `<package_parent>/tsdev_cli.exe` (NEW)
  4. Source-tree layout: walk up for `CMakeLists.txt` → `build/Release/tsdev_cli.exe` (existing)
  5. Fallback: `here.parents[1] / "build/Release/tsdev_cli.exe"` (existing)
- **Backward compatible:** Steps 1, 2, 4, 5 are unchanged. Step 3 is a new check that only activates when the exe is a sibling of the package directory.

### D5: No pip install from bundle
- Users import via `sys.path.insert(0, "dist")` or `PYTHONPATH=dist` or `cd dist`.
- No `pyproject.toml` in the bundle. This keeps the bundle minimal and avoids implying the wrapper has third-party deps (it doesn't — stdlib-only).
- Users who want pip-install can use the repo root (`pip install .`).

### D6: `.gitignore` update
- Add `/dist/` and `/artifacts/` to root `.gitignore` (explicit, clear).
- Remove `dist/.gitignore` (no longer needed — root `.gitignore` handles it).
- CMake `file(MAKE_DIRECTORY)` creates `dist/` and `artifacts/` at configure time, so they exist even without committed `.gitignore` placeholders.

### D7: Bundle README source template
- Create `docs/BUNDLE_README.md` as the source template (committed to repo).
- CMake POST_BUILD copies it to `dist/README.md`.
- Content: what's in the bundle, CLI quick-start, Python wrapper quick-start (sys.path/PYTHONPATH/cd), prerequisites (TradeStation 9.5 installed; Python 3.14+ for wrapper), `--ts-path` override note.

### D8: Manual zip one-liner (supplementary, for ad-hoc use)
- The build now **automatically** produces a timestamped zip in `artifacts/` (see D9). This D8 one-liner is a supplementary manual alternative for ad-hoc use.
- Document in the root README (run from repo root): `Compress-Archive -Path dist\tsdev_cli.exe,dist\tsdev_automation,dist\README.md -DestinationPath tsdev_automation-bundle.zip` (PowerShell).
- The zip is **selective** — it includes only the 3 bundle components, NOT any stale wheel/sdist artifacts that may happen to live in `dist/`. This prevents confusion (e.g. a user trying to `pip install` a bundled wheel that lacks the exe).

### D9: Automatic timestamped zip creation (build-time)
- **Mechanism:** A CMake POST_BUILD command (appended after all `dist/` copy commands) that invokes a committed PowerShell script to create a timestamped zip from the selective bundle contents.
- **Output location:** `artifacts/` directory at the repo root (sibling of `dist/`), created via `file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/artifacts)` at configure time.
- **Filename format:** `tsdev_automation_<yymmdd><HHMM>.zip` — e.g. `tsdev_automation_2608201430.zip` for a build on 2026-08-20 at 14:30.
- **Contents:** Same selective set as D8 — `dist/tsdev_cli.exe`, `dist/tsdev_automation/`, `dist/README.md`. NOT any stale wheel/sdist artifacts.
- **Implementation — committed `.ps1` script (avoids CMake/PowerShell quote-nesting hell):**
  - Create `tools/create_bundle_zip.ps1` (committed to repo) containing:
    ```powershell
    param([Parameter(Mandatory)][string]$SourceDir)
    $ts = Get-Date -Format 'yyMMddHHmm'
    $zipPath = Join-Path $SourceDir "artifacts\tsdev_automation_$ts.zip"
    Compress-Archive -Path "$SourceDir\dist\tsdev_cli.exe","$SourceDir\dist\tsdev_automation","$SourceDir\dist\README.md" -DestinationPath $zipPath -Force
    ```
  - CMake POST_BUILD invokes it cleanly with no inline quoting:
    ```cmake
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/create_bundle_zip.ps1 -SourceDir ${CMAKE_SOURCE_DIR}
    ```
  - This approach eliminates the 3-layer quote-nesting problem (CMake → shell → PowerShell) that would occur with an inline `-Command "..."` string. The script file handles all PowerShell quoting internally.
- **`-Force`** overwrites if a zip with the same timestamp (same minute) already exists — the latest build wins. This is the desired behavior.
- **PowerShell dependency is acceptable:** This is a Windows-only project (32-bit exe, loads orlib20.dll from the TradeStation install). PowerShell is always available on Windows. No cross-platform concern.
- **Build-time timestamp (not configure-time):** PowerShell `Get-Date` runs at build time (inside POST_BUILD), so the timestamp reflects when the build ran, not when CMake configured. CMake's `string(TIMESTAMP)` is configure-time only and would produce a stale timestamp on incremental builds.
- **Accumulation note:** Each build creates a new timestamped zip. `artifacts/` will accumulate zips over time. This is by design (preserves build history). Users can manually clean `artifacts/` if desired. No automatic cleanup is implemented (keeps it simple — the user did not request it).
- **Zero extra build steps:** The zip is produced as part of the same `cmake --build build --config Release` command that assembles `dist/`. No separate build target or manual step required.
- **Failure behavior:** If the zip step fails (disk full, permission issue, path error), CMake treats the POST_BUILD command as failed, which **fails the entire build**. This is the desired behavior (fail loudly — consistent with the project philosophy of failing on errors rather than silently producing incomplete output). The `dist/` directory is still assembled (the copy commands run before the zip command), but the build exit code will be non-zero.

## 4. TVS Decomposition

### Recommended Execution Order
**Execute in this order:** G.1 → G.2 → G.3 → G.5. G.4 is optional and may be done at any point or deferred to a separate workstream.

**Rationale:**
- **G.1 first** — creates the POST_BUILD framework and `dist/` + `artifacts/` directories that all other TVSs depend on. Highest ROI: unblocks everything.
- **G.2 second** — adds bundle-layout auto-discovery to the Python wrapper. Independent of G.3 but should be done before G.5 (G.5's zip extraction test verifies G.2 auto-discovery works on the zipped bundle).
- **G.3 third** — adds the README copy to CMake POST_BUILD and creates the bundle README template. Must be done before G.5 (G.5's zip includes the README).
- **G.5 fourth** — adds the automatic zip creation. Depends on G.1 (POST_BUILD framework + `artifacts/` dir) and G.3 (README in `dist/`). G.2's auto-discovery is verified by G.5's zip extraction test.
- **G.4 optional** — independent packaging-correctness fix (remove stale capstone dep). Does NOT block or affect the bundle. May be done during this workstream, deferred to a separate cleanup workstream, or skipped entirely.

### Definition of Done
**Workstream G is complete when:** G.1, G.2, G.3, and G.5 are implemented and adversarially reviewed (APPROVED); all per-TVS verifications pass; the §5 full E2E (7 steps) passes; and AGENTS.md + PM_WORKING_MEMORY.md are updated. **G.4 is NOT required** for Workstream G completion — it is an independent packaging-correctness fix that may be done during this workstream or deferred.

### TVS-G.1: CMake POST_BUILD bundle assembly + .gitignore
**Scope:** CMake changes to auto-assemble `dist/` on every build and create the `artifacts/` directory. `.gitignore` update for both `dist/` and `artifacts/`.

**Changes:**
1. `CMakeLists.txt`:
   - `file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)` and `file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist/tsdev_automation)` and `file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/artifacts)` at configure time.
   - `file(GLOB TSDEV_PY_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/tsdev_automation/*.py)` to list Python files.
   - `add_custom_command(TARGET tsdev_cli POST_BUILD ...)` with:
     - `cmake -E copy_if_different $<TARGET_FILE:tsdev_cli> ${CMAKE_SOURCE_DIR}/dist/tsdev_cli.exe`
     - `cmake -E remove_directory ${CMAKE_SOURCE_DIR}/dist/tsdev_automation` (clean stale `.py` files that were renamed/deleted in source, then re-copy fresh)
     - `cmake -E make_directory ${CMAKE_SOURCE_DIR}/dist/tsdev_automation` (recreate after remove)
     - `foreach` loop: `cmake -E copy_if_different <each .py> ${CMAKE_SOURCE_DIR}/dist/tsdev_automation/<name>`
     - **(README copy is NOT in G.1 — it is added in G.3.)**
   - `CONFIGURE_DEPENDS` on the GLOB so new `.py` files are picked up on re-configure.
   - **Stale file cleanup rationale:** `copy_if_different` only updates changed files; it does NOT remove files deleted/renamed in source. The `remove_directory` + `make_directory` + copy loop ensures `dist/tsdev_automation/` is a clean mirror of `src/tsdev_automation/*.py` on every build.
2. `.gitignore`: Add `/dist/` and `/artifacts/` lines.
3. Remove `dist/.gitignore` (root `.gitignore` now handles it). Verify `dist/` and `artifacts/` are fully ignored by git.

**Verification:**
- Clean `dist/` (delete contents).
- `cmake -S . -B build -A Win32 && cmake --build build --config Release`
- Verify `dist/tsdev_cli.exe` exists and is the same binary as `build/Release/tsdev_cli.exe` (compare hashes).
- Verify `dist/tsdev_automation/` has exactly 6 `.py` files, no `__pycache__/`.
- Verify `dist/tsdev_cli.exe --help` works.
- Verify `git status` shows `dist/` as ignored (no untracked files in `dist/`).

**Dependencies:** None (first TVS). Note: the README copy will produce a warning until G.3 creates `docs/BUNDLE_README.md`. To avoid this, either (a) sequence G.3 before G.1, or (b) guard the README copy with `if(EXISTS ...)`. Recommendation: create a placeholder `docs/BUNDLE_README.md` in G.1 (minimal content), then G.3 fills it in. OR: split so G.1 only copies exe + .py, and G.3 adds the README copy to CMake. **Decision: G.1 copies exe + .py only. G.3 adds the README copy command to CMake + creates the template.** This keeps each TVS atomic.

### TVS-G.2: Python wrapper bundle-layout auto-discovery
**Scope:** Modify `_resolve_cli_path()` to detect bundle layout. Add unit test.

**Changes:**
1. `src/tsdev_automation/client.py` — `_resolve_cli_path()`:
   - After env var check (step 2), before CMakeLists.txt walk (current step 3):
   - Add: `bundle_exe = Path(__file__).resolve().parent.parent / "tsdev_cli.exe"; if bundle_exe.is_file(): return bundle_exe`
   - Update docstring to document the new priority.
2. `tests/test_tsdev_client.py`:
   - Add unit test: create a temp dir with `tsdev_automation/` subfolder containing a fake `client.py` (or use the real package), place a fake `tsdev_cli.exe` in the temp dir (parent of `tsdev_automation/`), verify `_resolve_cli_path(None)` returns the bundle exe path.
   - Test must NOT depend on the real build or TradeStation — use a fake exe file (empty file) like existing `fake_cli` fixture.

**Verification:**
- Unit test passes (mocked, no real CLI needed).
- Manual: `cd dist && python -c "from tsdev_automation import TsDevClient; c = TsDevClient(); print(c.cli_path); assert c.cli_path.name == 'tsdev_cli.exe'"` — prints `dist\tsdev_cli.exe`.
- Existing unit tests still pass (no regression).
- Full Python test suite: `pytest tests/` (60 tests, all pass).

**Dependencies:** Should be tested against the bundle produced by G.1. Can be developed in parallel but verified after G.1.

### TVS-G.3: Bundle README + root README/docs update
**Scope:** Create bundle README template, wire it into CMake POST_BUILD, update root README and BUILD.md.

**Changes:**
1. Create `docs/BUNDLE_README.md` — bundle quick-start guide:
   - What's in the bundle (tsdev_cli.exe + tsdev_automation/ package).
   - Prerequisites: TradeStation 9.5 installed (for orlib20.dll); Python 3.14+ 64-bit (for wrapper).
   - CLI usage: `tsdev_cli.exe <subcommand> ...` with brief examples.
   - `--ts-path <path>` override if TradeStation is not at the default location.
   - Python wrapper usage: `sys.path.insert(0, ".")` or `PYTHONPATH` then `from tsdev_automation import TsDevClient`.
   - Pointer to full docs (root README.md, docs/BUILD.md) for advanced usage.
2. `CMakeLists.txt` — add to the POST_BUILD command:
   - `cmake -E copy_if_different ${CMAKE_SOURCE_DIR}/docs/BUNDLE_README.md ${CMAKE_SOURCE_DIR}/dist/README.md`
3. `README.md` (root) — add a "Distribution / Bundle" section:
   - Explain that `cmake --build build --config Release` produces `dist/` with the CLI + Python wrapper AND a timestamped zip in `artifacts/`.
   - Explain how to use `dist/` (CLI directly, Python via sys.path).
   - Explain how to use `artifacts/` (the auto-produced zip is ready to distribute; extract and use).
   - Manual zip one-liner (supplementary): `Compress-Archive -Path dist\tsdev_cli.exe,dist\tsdev_automation,dist\README.md -DestinationPath tsdev_automation-bundle.zip`
4. `docs/BUILD.md` — add a "Bundle output" subsection:
   - Document that the POST_BUILD step assembles `dist/` and creates a timestamped zip in `artifacts/`.
   - Document the bundle layout and the zip naming convention (`tsdev_automation_<yymmdd><HHMM>.zip`).

**Verification:**
- `cmake --build build --config Release` copies `docs/BUNDLE_README.md` → `dist/README.md`.
- `dist/README.md` content is correct and helpful.
- **E2E CLI smoke test (mandatory per AGENTS.md):** `dist\tsdev_cli.exe --help` succeeds (exit 0, prints usage). This confirms the build-affecting CMake change (README copy added to POST_BUILD) did not break the build.
- Root README and BUILD.md are accurate.

**Dependencies:** After G.1 (adds the CMake POST_BUILD framework).

### TVS-G.4 (optional, low priority, NOT blocking the bundle): Remove stale capstone runtime dependency
**Scope:** Fix `pyproject.toml` so the wrapper package declares zero runtime deps (it's stdlib-only). This is a packaging-correctness fix orthogonal to the bundle; it is strictly optional and does NOT block TVS-G.1–G.3.

**Changes:**
1. `pyproject.toml`:
   - Remove `capstone>=5.0.9` from `[project] dependencies` entirely.
   - Do NOT add it to `[dependency-groups] dev` or any optional group — `capstone` is only used by historical RE probe scripts under `tmp/re_artifacts/` (gitignored, not part of any distributed artifact). `tools/sigcodegen.py` is explicitly stdlib-only and does not use capstone. There is no legitimate production use of capstone in this repo.
2. `README.md` — correct the Python wrapper deps note: remove the false claim that capstone is a dependency (it was never needed by the wrapper or the build tooling).

**Rationale:** The wrapper is stdlib-only at runtime. `capstone` was misdeclared as a runtime dependency — it is not used by the wrapper, not used by `tools/sigcodegen.py`, and only referenced by gitignored historical RE scripts. Declaring it forces users who `pip install tsdev-automation` to install capstone unnecessarily. This aligns with "avoid dependencies that do not add significant value."

**Verification:**
- `pip install .` does not install capstone.
- `python -c "import tsdev_automation"` works without capstone installed.
- `tools/sigcodegen.py` still works (it never needed capstone).
- Python tests pass.

**Dependencies:** Independent. Can be done anytime. Strictly optional — does not block or affect the bundle (G.1–G.3). Could be deferred to a separate packaging-cleanup workstream if the PM prefers a truly minimal bundle workstream.

### TVS-G.5: Automatic timestamped zip creation in artifacts/
**Scope:** Add a committed PowerShell script + CMake POST_BUILD command that automatically creates a timestamped zip of the bundle contents in `artifacts/` after every build.

**Changes:**
1. Create `tools/create_bundle_zip.ps1` (committed to repo) — see D9 for the exact script content. The script takes a `-SourceDir` parameter, generates a build-time timestamp via `Get-Date -Format 'yyMMddHHmm'`, and creates a selective zip via `Compress-Archive -Force`.
2. `CMakeLists.txt` — append a POST_BUILD command as the **last** command in the `tsdev_cli` POST_BUILD chain (after all `dist/` copy commands, including the README copy from G.3):
   - `COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/create_bundle_zip.ps1 -SourceDir ${CMAKE_SOURCE_DIR}`
   - This invokes the committed script file, avoiding inline PowerShell quoting issues entirely.
   - The `artifacts/` directory is already created by G.1's `file(MAKE_DIRECTORY)`.
3. No `.gitignore` change needed — G.1 already adds `/artifacts/`.

**Verification:**
- `cmake --build build --config Release` produces a zip in `artifacts/` matching the pattern `tsdev_automation_<yymmdd><HHMM>.zip`.
- Verify the zip filename timestamp matches the current build time (within the same minute).
- Extract the zip to a temp directory and verify it contains exactly: `tsdev_cli.exe`, `tsdev_automation/` (6 `.py` files), `README.md` — no wheel/sdist artifacts, no `__pycache__/`.
- **E2E CLI smoke test (mandatory per AGENTS.md):** Run the extracted `tsdev_cli.exe --help` → succeeds (exit 0). This confirms the zip contains a working binary.
- Run the extracted Python wrapper: `cd <extracted> && python -c "from tsdev_automation import TsDevClient; c = TsDevClient(); print(c.cli_path)"` → prints the extracted exe path (validates G.2 auto-discovery works on the zipped bundle).
- Verify `git status` shows `artifacts/` as ignored (no untracked files in `artifacts/`).
- Rebuild and verify a NEW zip is created with a new timestamp (or same timestamp with `-Force` overwrite if within the same minute).

**Dependencies:** After G.1 (POST_BUILD framework + `artifacts/` directory creation) and G.3 (README copy to `dist/` — the zip should include the README). G.2 is not a hard dependency but the zip verification tests G.2 auto-discovery on the extracted bundle.

## 5. E2E Verification (after G.1 + G.2 + G.3 + G.5)

Per AGENTS.md mandatory E2E CLI verification after every TVS.

**Note on per-TVS E2E scope:** The G TVSs modify build/packaging (CMake POST_BUILD, .gitignore, Python path resolution, zip creation) — NOT CLI logic. The per-TVS verifications therefore use `dist\tsdev_cli.exe --help` (a smoke test confirming the build produced a working binary) rather than full insert/delete/modify cycles. The full insert/delete/modify E2E (step 2 below) is run once after all TVSs are complete, per `docs/E2E_TEST_SPEC.md`. This is a deliberate scope-appropriate interpretation of the AGENTS.md E2E mandate.

1. **Clean build:** Delete `dist/` and `artifacts/` entirely. Run `cmake -S . -B build -A Win32 && cmake --build build --config Release`. Verify `dist/` is recreated with `tsdev_cli.exe`, `tsdev_automation/` (6 `.py` files), `README.md`. Verify `artifacts/` contains a zip matching `tsdev_automation_<yymmdd><HHMM>.zip`.
2. **CLI works from bundle:** Create a temp ASCII source file (per `docs/E2E_TEST_SPEC.md` §2.6 — use `[System.IO.File]::WriteAllText($srcFile, $src, [System.Text.Encoding]::ASCII)`), then (positional args — the CLI does NOT support `--type`/`--name`/`--source` flags): `dist\tsdev_cli.exe insert Strategy E2E_BundleTest <srcFile>` → success. `dist\tsdev_cli.exe exists Strategy E2E_BundleTest` → exists=1. `dist\tsdev_cli.exe delete Strategy E2E_BundleTest` → success.
3. **Python wrapper from bundle:** `cd dist && python -c "from tsdev_automation import TsDevClient; c = TsDevClient(); r = c.exists('Strategy', 'NonExistent'); print(r.exists, r.prog_id)"` → `False 0`. Then a real insert/delete cycle via the wrapper.
4. **PYTHONPATH from elsewhere (PowerShell):** `$env:PYTHONPATH = "<path-to-your-clone>\dist"; cd C:\; python -c "from tsdev_automation import TsDevClient; c = TsDevClient(); print(c.cli_path)"` → prints the dist exe path. (Note: use `$env:PYTHONPATH` — PowerShell `set` is an alias for `Set-Variable` and does NOT set environment variables.)
5. **Auto-produced zip:** Extract the zip from `artifacts/` to `tmp\bundle_extracted\`. Verify it contains exactly `tsdev_cli.exe`, `tsdev_automation/` (6 `.py` files), `README.md` — no wheel/sdist/`__pycache__`. Run `tmp\bundle_extracted\tsdev_cli.exe --help` → works. `cd tmp\bundle_extracted && python -c "from tsdev_automation import TsDevClient; c = TsDevClient(); print(c.cli_path)"` → prints the extracted exe path.
6. **Full regression suite:** `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` — all 8 C++ suites + 60 Python tests pass.
7. **Git status:** `dist/` and `artifacts/` are fully ignored (no untracked files shown).

## 6. Post-completion Checklist
After all required TVSs (G.1, G.2, G.3, G.5) pass §5 E2E:
1. **Update AGENTS.md** — `### Active Workstream` table: mark G.1/G.2/G.3/G.5 as COMPLETE. If G.4 was done, mark it COMPLETE too; if deferred, move it to "Candidate Future Work".
2. **Update PM_WORKING_MEMORY.md** — move Workstream G from "NEXT WORKSTREAM" to "Completed Work Summary".
3. **Run full regression suite:** `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` — all 8 C++ suites + 60 Python tests pass.
4. **Commit all changes** (per AGENTS.md git rules — use `git commit -F tmp\commit_msg.txt` on PowerShell, no heredoc). Suggested message: `Workstream G: Distributable bundle (G.1-G.3, G.5) complete`.
5. **Archive this plan** to `docs/archive/WORKSTREAM_G_PLAN.md` (per project convention for completed workstream plans).

## 7. Out of Scope

- **Bundling TradeStation DLLs (orlib20.dll etc.):** NOT done. The CLI loads these from the TradeStation install at runtime. Bundling them would violate the READ-ONLY constraint on TS install files and is unnecessary (users must have TradeStation installed anyway).
- **Bundling sigtool.exe:** NOT done. It's a developer tool for signature verification, not for end users.
- **Bundling pyproject.toml / pip-install from bundle:** NOT done. Users import via sys.path. Keeps bundle minimal. Can be added later if needed.
- **CI/CD (GitHub Actions etc.):** NOT done. No `.github/` exists. Out of scope for this workstream.
- **Multi-version orlib20.dll support:** NOT done. Only one version exists. Inherent limitation.
- **4 unconfirmed component types (ActivityBar, PaintBar, ProbabilityMap, ShowMe):** NOT done. Separate RE workstream.

## 8. Risks & Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| POST_BUILD copy fails if `dist/` doesn't exist | Low | `file(MAKE_DIRECTORY)` at configure time |
| `copy_directory` copies `__pycache__/` | Medium | Use `file(GLOB ... *.py)` + per-file `copy_if_different`, NOT `copy_directory` |
| Stale `.py` files persist in `dist/tsdev_automation/` after rename/delete in source | Medium | `remove_directory` + `make_directory` + copy loop in G.1 ensures clean mirror on every build |
| Bundle auto-discovery conflicts with source-tree layout | Low | Bundle check is before CMakeLists.txt walk; in source tree, `src/tsdev_cli.exe` doesn't exist, so it falls through |
| `dist/` mixing with Python wheel artifacts | Low | Acceptable — wheels are small; zip command is selective (only 3 bundle components) |
| PowerShell `Compress-Archive` not available | Very Low | Windows-only project; PowerShell is always present on Windows. `powershell -NoProfile` avoids profile issues. |
| Zip timestamp collision (two builds in same minute) | Low | `-Force` overwrites — latest build wins. Acceptable behavior. |
| `artifacts/` accumulates many zips over time | Medium | By design (build history). Users can manually clean. No auto-cleanup (keeps it simple). Documented in D9. |
| PowerShell quoting/escaping in CMake POST_BUILD | Low | Eliminated by using a committed `.ps1` script file (D9) instead of inline `-Command "..."`. No quote nesting. |
| CMake POST_BUILD runs on Debug builds too | Low | Acceptable — Debug bundle works; document "build Release for production" |
| `CONFIGURE_DEPENDS` causes re-configure on every build | Low | Only when `.py` files change; acceptable |
