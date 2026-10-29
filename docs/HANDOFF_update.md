# Handoff: `tsdev_cli update` (self-update)

Maintainer map for the self-update feature. The contract (exact grammar,
output text, JSON shapes, exit codes) lives in `tmp/UPDATE_DESIGN.md` --
this file is a map, not a copy. Spec section numbers below refer to that
document.

## Preconditions

- The update feed only works once the GitHub repo is PUBLIC. While
  private, every anonymous `/releases` call 404s and every check exits
  `25` `update_check_failed` -- expected pre-flip behavior, not a bug
  (spec 0.1, Appendix C).

## Feature map

| Piece | Files | Contract |
|---|---|---|
| Release feed: GET `<base>/releases`, semver parse/compare, max-published selection, draft/prerelease/non-semver filtering, digest extraction, rate-limit handling | `src/update_feed.{h,cpp}` | spec 1.2, 1.3 |
| Apply: download, sha256 verify, staging dir, staged swap (`*.old` renames), rollback, deferred apply, cleanup helper, update mutex | `src/update_apply.h` (+ cpp) | spec 9.1-9.8 |
| CLI wiring: `update` dispatch, `--yes`/`--force`/hidden `--update-url`/global `--update-result-file`, `--version` flag, version stamp (`TSDEV_VERSION_STRING`, `kTsdevCliVersion`, `kMinSelfUpdateVersion`) | `src/tsdev_cli.cpp` | spec 1, 4, 5, 6, 8, App. B |
| CMake stamp + dist bundle | `CMakeLists.txt` | spec App. B |
| Python client (`check_update`, `update`, runas launch + result-file capture) | `src/tsdev_automation/{client,results,exceptions,_cli}.py` | spec 10 |

`update` never loads orlib20/WHServer -- no TradeStation involvement
(spec 9.10). `--ts-path` is accepted and ignored.

## Contract pointers (read the spec, not this file)

- Grammar + `--version`: spec 1, 6. Versions render normalized (`v0.1.6`,
  dev stamps verbatim `0.0.0-dev`); input accepts optional leading `v`.
- Exact human output lines: spec 2. Exact JSON frames: spec 3.
- Confirmation (capability loss below `kMinSelfUpdateVersion`), `--yes`,
  `--force` scope: spec 4.
- Error table + exact message text: spec 5.
- Testing overrides `--update-url`/`TSDEV_UPDATE_URL` (full API-base
  replacement) and `--update-result-file` (global hidden flag, update
  only, written at every JSON emission point incl. not_elevated): spec 8.
- UAC copy for docs: spec 7. Docs checklist: spec 11.

## Exit codes

`25` update_check_failed | `26` update_not_found | `27` update_failed |
`28` update_refused | `12` cancelled (declined prompt) | `1`/`11`
unchanged. Full map: spec 5 + Appendix A. Codes 13 and 20-24 were already
taken -- do not renumber.

## Safety design (spec 9)

- Download verified against the asset's published sha256 `digest`;
  missing/mismatched digest refuses (27), never verify-optional.
- Staging `<install_dir>\.tsdev_update_tmp_<pid>`; completeness gate
  (exe + `tsdev_automation\` >=1 file + `README.md`) BEFORE any rename.
- Swap = rename each payload member to `*.old`, move new in; rollback =
  rename `*.old` back. `*.old` cleanup by a detached helper after exit;
  stale leftovers reaped at the start of every apply run.
- Interactivity predicate requires a VISIBLE console window (hidden
  `runas` consoles must exit 28, never block on `ReadConsoleW`) -- spec
  4.1; load-bearing, do not weaken.
- Per-install-dir named mutex `Local\tsdev_cli_update_<sha256-16>`;
  second concurrent update waits 60 s then exits 27.
- Non-bundle gate: apply requires `tsdev_automation\__init__.py` +
  `README.md` next to the exe (exit 28 unless `--force`).
- Mid-update power loss: exe missing + `*.old` present -> rename `*.old`
  back (documented for users in `docs/BUNDLE_README.md`).

## Python surface (spec 10)

`client.check_update()` / `client.update(version=None, yes=False,
force=False)`. Never routed via the serve broker; non-elevated sessions
elevate per call via `ShellExecuteExW` runas with `SEE_MASK_NO_CONSOLE`
+ `--update-result-file` for result capture (bounded wait 15 min; on
timeout terminate + `TsDevError`). `TSDEV_UPDATE_URL` is forwarded as
`--update-url` because runas drops env. New result fields and exceptions
listed in `docs/BUNDLE_README.md`; `broker_restart_required` note when a
live broker outlives the update.

## Tests

`tests/test_update_feed.cpp` exists (feed parse/select/digest). Planned
siblings `test_update_apply.cpp` / `test_update_cmd.cpp` may not have
landed yet and names may change -- check `tests/` before referencing.
Update is fully testable without TS (`-NoTs`): feed fixtures via
`TSDEV_UPDATE_URL`/`--update-url` pointing at a local HTTP server
(spec 8 harness shape). Suite inventory: `tests/run_unit_tests.ps1` /
`tests/run_regression.ps1` (SSOT).

## Deliberately excluded (spec 12)

Channels/prerelease opt-in, automatic background checking, an
`update --rollback` subcommand, a `version` subcommand (the `--version`
flag only), GitHub auth/tokens, install-dir relocation config.
