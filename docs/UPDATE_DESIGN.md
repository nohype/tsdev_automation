# Self-update UX/design spec -- `tsdev_cli update`

Status: **DESIGN SPEC** (not yet implemented). Audience: implementer +
reviewer. This is a UX/architecture contract: exact grammar, exact output
text, exact JSON shapes, exit codes, and safety semantics. Internals are
specified only where they are user-visible or test-contractual.

Source conventions verified against: `src/tsdev_cli.cpp` (usage text,
`CliResult`, `cliResultToJson`, `errorCodeString`, CONIN$ prompt precedent,
env-mirror precedence, elevation guard, subcommand dispatch),
`src/tsdev_automation/{client.py,results.py,exceptions.py,_cli.py}`,
`CMakeLists.txt` (dist/ bundle assembly), `.github/workflows/ci.yml`
(release zip name `tsdev_automation_<tag>.zip`, one asset per release),
`README.md`, `docs/BUNDLE_README.md`, `AGENTS.md`.

Hard rule for everything below: **ASCII only** in output strings and docs.
stdout = results only; stderr = `tsdev_cli: `-prefixed diagnostics plus
unprefixed progress lines (batch-progress precedent); detail goes to
`--log-file`, not the console.

---

## 0. Verified facts + corrections

### 0.1 Facts this design builds on

- One release asset per release: `tsdev_automation_<tag>.zip`, zip root =
  bundle layout (`tsdev_cli.exe`, `tsdev_automation/`, `README.md`).
  Confirmed by `ci.yml` (`cd bundle && zip -r ../tsdev_automation_${tag}.zip .`)
  and `CMakeLists.txt` POST_BUILD (dist/ = exe + `tsdev_automation/` +
  `README.md` copied from `docs/BUNDLE_README.md`).
- GitHub REST `GET https://api.github.com/repos/nohype/tsdev_automation/releases`
  returns a JSON array; each release has `tag_name`, `draft`, `prerelease`,
  `assets[]`; each asset has `name`, `browser_download_url`, `size`,
  `digest` (`"sha256:<64-hex>"`, or `null` on very old assets). Anonymous
  rate limit: 60 requests/hour per IP; HTTP 403 with
  `X-RateLimit-Remaining: 0` and `X-RateLimit-Reset` (epoch seconds).
  Draft releases are NEVER returned to anonymous callers (defense in
  depth: the filter still skips `draft==true`).
- RELEASE PRECONDITION: the update feed requires the GitHub repo to be
  PUBLIC; while private, every anonymous call 404s and the spec'd
  behavior is exit 25 `update_check_failed` (unreachable/HTTP-error
  class) for all users -- this is the expected pre-flip failure mode.
- Running exe cannot be overwritten but CAN be renamed in-volume; loaded
  DLLs same. Bundle dir = `GetModuleFileNameW` exe directory.
- Elevation: manifest `requireAdministrator` + in-process `isElevated()`
  gate -> exit 11 `not_elevated`. Non-negotiable (AGENTS.md).
- Prompt precedent (`--cache-backup-policy=prompt`): opens `CONIN$`
  (`CreateFileW(L"CONIN$", GENERIC_READ|GENERIC_WRITE)`), reads via
  `ReadConsoleW`, never stdin. Interactivity = CONIN$ opens AND stderr is
  a console AND the console window is VISIBLE (`GetConsoleWindow() !=
  NULL && IsWindowVisible(GetConsoleWindow())`): a console exe spawned
  via `ShellExecuteExW` runas + `SW_HIDE` (the Python client's elevated
  launch, 10) gets a REAL but INVISIBLE console -- CONIN$ opens and
  `GetConsoleMode` succeeds, so without the visibility clause
  `ReadConsoleW` would block forever on a prompt nobody can see (4.1).
- Env mirrors: `TSDEV_*`; explicit flag beats env; booleans are
  presence-not-"0"; garbage values keep defaults.
- `--json` emits one result object on stdout; `op`/`type`/`name` omitted
  when unknown; failure shape
  `{"success":false,"exit_code":N,"error":{"code","message",...}}`.
- Python client: `_run_cli` uses `subprocess.run` + `--json`; non-elevated
  `use_serve=False` raises `ElevationRequiredError` pre-launch; elevated
  `use_serve=True`... a NON-elevated `use_serve=True` session cannot
  `subprocess.run` a `requireAdministrator` exe (CreateProcess fails,
  WinError 740) -- the client starts the broker via `ShellExecuteExW`
  runas instead. `runas` does NOT inherit the caller's environment block
  (noted in `tsdev_cli.cpp` for `TSDEV_WHSERVER_*`; applies to
  `TSDEV_UPDATE_URL` too -- the client must forward it as a flag).

### 0.2 Corrections to the brief's assumptions

1. **"13 and 20+ are free" is wrong.** `tsdev_cli.cpp:117-140` documents:
   `13` = diag-build watchdog abort (`tsdev_cli_diag`), `20-24` =
   own-process fatal-termination containment (std::terminate / abort /
   CRT invalid-parameter / pure-call / defensive-UEF). New update codes
   start at **25**.
2. **"Every launch triggers a UAC prompt"** is true in practice (manifest),
   but `isElevated()==false` is still reachable in odd contexts (UAC
   disabled, shims, restricted tokens). The in-process gate stays; update
   inherits it unchanged.
3. **`GET /releases/latest` must NOT be used.** GitHub's "latest" is the
   most-recent-created/managed-flag release, not the semver maximum.
   "Latest release from main" = max semver over published entries (1.2).

---

## 1. Command grammar

```
tsdev_cli [--json] [--pbar] update                      # update to latest release
tsdev_cli [--json] [--pbar] update check                # check only, change nothing
tsdev_cli [--json] [--pbar] update <version>            # update OR downgrade to <version>
tsdev_cli [--json] [--pbar] update latest               # alias for bare "update"
tsdev_cli [--json] update <version-or-check> [--yes] [--force]
tsdev_cli [--json] --version                            # new global flag (Section 6)
```

- `update` is a first-class subcommand dispatched in `main()` beside
  `insert`/`modify`/... (`iequals` match, case-insensitive).
- Sub-argument `argv[2]` (update-local, parsed inside the dispatch --
  the `--case-insensitive`-in-`exists` precedent, NOT the global
  pre-scan): `check` (literal, case-insensitive), `latest` (literal,
  case-insensitive), or `<version>`.
- Accepted version spellings: `v0.1.5` and `0.1.5` (leading `v`/`V`
  optional). Grammar: `^[vV]?[0-9]+[.][0-9]+[.][0-9]+$`. Anything else
  (`v0.1.5-beta`, `0.1.5.1`, `0.2.0_remote-control`, `foo`) -> usage
  error, exit 1.
- Update-local flags may appear in ANY position among the update args
  (`update --yes v0.1.5`, `update v0.1.5 --yes`, and
  `update --force check` are all legal). They are removed from the
  update arg list inside the dispatch, so `insert ... --yes` stays a
  usage error:
  - `--yes` / `-y` presence flag, repeatable-harmless: pre-answers the
    capability-loss confirmation (4.1).
  - `--force` presence flag, repeatable-harmless: overrides the
    dev-build and non-bundle-layout refusals and forces reinstall at the
    same version (4.3, 5).
  - `--update-url <base>` value flag, non-repeatable, **hidden**
    (accepted, not listed in usage text): test override (8).
- `--update-result-file <path>` is a GLOBAL hidden value flag
  (non-repeatable), NOT an update-local one: it is recognized and
  consumed by the global flag pre-scan -- the
  `--batch-progress-control` precedent (recognized+removed globally,
  then validated post-elevation-guard per-subcommand). Being parsed
  before the elevation guard and before dispatch, it is
  position-free anywhere on the command line and is already in hand
  for every pre-dispatch exit (not_elevated included). Validation:
  allowed ONLY with the `update` subcommand (any form); on any other
  subcommand -> usage error exit 1. Contract: writes the JSON result
  object to `<path>` (atomic `.tmp` + rename, the
  `--serve-pipe-file` precedent) at every JSON-frame emission point
  (8). Used by the Python client's elevated-launch path which cannot
  capture stdout (8, 10).
- Positional grammar: `update` accepts 0-or-1 positional token --
  `check` | `latest` | `<version>`. More than one positional (`update
  check extra-arg`, `update v0.1.5 extra`, `update check v0.1.5`) or an
  unknown token (`update foo`) -> usage error exit 1.
- Flags on forms that do not need them: `--yes`/`--force` on bare
  `update` or `update check` are accepted and simply unused (presence
  flags; consistent treatment). Two documented subtleties: `--force` on
  bare `update` DOES have a real effect -- it forces the apply path
  past the dev-build and non-bundle gates (4.3, item 9.9); `--yes` on
  bare `update` is a true no-op -- the capability-loss confirmation it
  pre-answers can never trigger on the latest path (target = max
  published semver >= `kMinSelfUpdateVersion` once this feature has
  shipped), so passing it is harmless.

### 1.1 What each form does

| Invocation | Behavior |
|---|---|
| `update` | Latest = max published semver. If current < latest: download+install. If current == latest: `You are up to date (vX.Y.Z).`, exit 0. If current > latest: ahead message, exit 0. Dev build: refuse unless `--force`. |
| `update check` | Read-only check. Prints current vs latest and whether an update exists. Writes nothing anywhere -- no staging dir, no `.old` reaping, never downloads the zip (only an explicitly requested `--update-result-file` is honored, 8/9.5). |
| `update <version>` | Installs exactly `<version>` -- upgrade, same-version (no-op unless `--force`), or downgrade. Version must resolve to a published non-draft non-prerelease release, else exit 26. |
| `update latest` | Identical to bare `update`. |
| `update --help`, `update -h` | Not special-cased: `--help` is not a version/`check`/`latest` token -> usage error exit 1, full usage on stderr (there is no per-subcommand help in this CLI; consistency beats a one-off). |

### 1.2 "latest" -- concrete definition

`latest = max semver` over releases where `draft == false`,
`prerelease == false`, and `tag_name` parses under the grammar above
(with optional `v`). Comparison is NUMERIC per component: `v0.1.10 >
v0.1.9`. The draft `v0.2.0_remote-control` is excluded twice: `draft` is
true AND the tag is not semver. Releases with non-semver tags are
invisible to both "latest" and explicit `update <version>` (their tag
cannot be spelled as a `<version>` argument).

Feed query: `GET <base>/releases?per_page=100`, first page only (if the
repo ever exceeds 100 published releases, revisit; log a note at
`-vv`).

### 1.3 Version comparison + parse

- Parse `^[vV]?(\d+)\.(\d+)\.(\d+)$`, each component `0..99999` -- a
  stricter bound avoids overflow and is plenty for this project.
- Compare `(major, minor, patch)` numerically.
- Output normalization (human AND JSON): clean semver stamps render
  with a `v` prefix (stored `0.1.6` -> `v0.1.6`); dev stamps render
  verbatim (`0.0.0-dev`). Input accepts either spelling (1); output
  always normalizes.
- The RUNNING binary's own version comes from the compile define
  `TSDEV_VERSION_STRING` (Appendix B), not from any file/registry.

---

## 2. Human-mode output -- exact text (every path)

Conventions: results on stdout; progress on stderr (unprefixed, like
`BATCH` lines); diagnostics on stderr prefixed `tsdev_cli: `; errors
`tsdev_cli: ERROR ...`. `[...]` below = substituted values.

### 2.1 `update check`

Behind (update available):
```
(stdout) Update available: v0.1.5 -> v0.1.6.
         Run: tsdev_cli update
(stderr) Checking for updates ...
```
Up to date:
```
(stdout) You are up to date (v0.1.5).
```
Ahead (current newer than latest -- dev or newer-than-released):
```
(stdout) Your version (v0.2.0) is newer than the latest release
         (v0.1.6). Nothing to do.
```
Dev build:
```
(stdout) Development build (0.0.0-dev) -- not installed from a release.
         Latest release: v0.1.6.
```
No published releases at all:
```
(stdout) No published releases were found to update to.
```
All `check` paths that reach a verdict exit **0**.

### 2.2 `update` / `update latest` / `update <version>`

Progress (stderr, human mode only; suppressed under `--json`):
```
Checking for updates ...
Downloading tsdev_automation_v0.1.6.zip (2.4 MB) ...
Installing v0.1.6 ...
```
With `--pbar`, a determinate pinned bar (Content-Length based) replaces
the `Downloading ...` line; other lines unchanged.

Success, stdout:
```
Updated: v0.1.5 -> v0.1.6.
```
Conditional stderr note -- emitted only when a toolhelp snapshot finds
ANOTHER `tsdev_cli.exe` process (likely the serve broker):
```
tsdev_cli: NOTE another tsdev_cli.exe is still running the old version.
           A running serve broker keeps serving v0.1.5 until it is restarted.
```
Downgrade to a still-capable version (`kMinSelfUpdateVersion <= target <
current`) adds one stderr line before `Installing`; a downgrade below
`kMinSelfUpdateVersion` takes the capability-loss confirm path (4.1)
instead and does NOT print this line:
```
Downgrading v0.1.7 -> v0.1.6 (this version can still update itself).
```

Deferred apply (the running exe could not be renamed, item 9.6): exit
stays 0 and stdout adds one line after the `Updated:` line:
```
Update will finish when this command exits.
```

Renamed-exe stderr note -- emitted when the running file's basename is
not `tsdev_cli.exe` (the user renamed the exe, item 9.12):
```
tsdev_cli: NOTE this copy was renamed to <name>.exe; the update
           installs a fresh tsdev_cli.exe -- launch that file (or
           rename it back).
```

No published releases at all (bare `update` / `update latest`): same
verdict as `update check` (2.1), exit 0:
```
(stdout) No published releases were found to update to.
```
JSON mirrors the check verdict fields with `"action":"apply"`
(`update_available:false`, `latest_version:null`,
`target_version:null`, `asset` omitted).

Already at target (`update` when current == latest, `update <v>` when
current == target):
```
(stdout) You are up to date (v0.1.5).          # bare update
(stdout) Already at v0.1.5 -- nothing to do.   # update <same-version>
```
(exit 0; hint appended to the second: ` Reinstall anyway with --force.`)

Ahead, bare `update` (stdout):
```
Your version (v0.2.0) is newer than the latest release (v0.1.6).
Nothing to do.
```

### 2.3 Success note the user always needs

None extra. One result line on stdout, three progress lines on stderr,
conditional broker note. That is the whole user-visible surface; URLs,
hashes, file lists, HTTP status codes go to `--log-file` only.

---

## 3. JSON shapes (`--json`)

Conventions: `op`/`type`/`name` omitted when unknown (update never emits
`type`/`name`). `action` discriminates check vs apply.

### 3.1 `update check` -- success

```json
{"op":"update","action":"check","success":true,"exit_code":0,
 "current_version":"v0.1.5","dev_build":false,
 "latest_version":"v0.1.6","update_available":true,
 "target_version":"v0.1.6",
 "asset":{"name":"tsdev_automation_v0.1.6.zip",
          "size_bytes":2488114,
          "sha256":"1a2b...64hex",
          "url":"https://github.com/nohype/tsdev_automation/releases/download/v0.1.6/tsdev_automation_v0.1.6.zip"}}
```

- `update_available`: `latest > current` by semver; `false` when equal,
  ahead, or no published release.
- `target_version`: release the user would get from bare `update`;
  `null` when `update_available` is false.
- `asset`: present only when a `tsdev_automation_<tag>.zip` asset was
  found on the latest release; `sha256` is the `digest` field with the
  `sha256:` prefix stripped (absent entirely when `digest` is null).
- Dev build: `"current_version":"0.0.0-dev","dev_build":true`; a dev
  build is treated as older than every release, so `update_available`
  is `true` and `requires_force` is added:
  `"requires_force":true`. `requires_confirmation` (4.1) and
  `requires_force` are added whenever they apply, in both check and
  apply objects.
- `latest_version`: `null` when the feed has no published release.

### 3.2 `update` -- success

```json
{"op":"update","action":"apply","success":true,"exit_code":0,
 "from_version":"v0.1.5","to_version":"v0.1.6","downgrade":false,
 "files_replaced":27,"install_dir":"C:\\Tools\\tsdev",
 "notes":["other_instances_running"]}
```

- `files_replaced`: count of payload members written into the install
  dir (exe + `tsdev_automation\` files + `README.md`).
- `notes`: array of machine tokens; defined values (all five):
  `other_instances_running` (the broker/stale-instance note fired,
  item 9.7), `staging_cleanup_deferred` (a `.old` file could not be
  scheduled for deletion -- cosmetic, update still succeeded,
  item 9.5), `deferred_apply` (the running exe could not be renamed;
  a detached helper finishes the swap when this process exits,
  item 9.6), `broker_restart_required` (the Python client left a live
  serve broker running the old version; restart it to pick up the new
  one, 10), `renamed_exe` (the running exe had been renamed; the
  update installed a fresh `tsdev_cli.exe`, item 9.12).
  Empty array = `[]`. Extensible.
- Already-current: `{"op":"update","action":"apply","success":true,
  "exit_code":0,"from_version":"v0.1.5","to_version":"v0.1.5",
  "already_current":true}` -- `files_replaced`/`notes` omitted.
- Empty feed on bare `update`/`update latest`: success verdict
  mirroring the check fields but pinned `"action":"apply"` (it IS
  the apply form): `current_version` and `dev_build` present,
  `latest_version:null`, `target_version:null`,
  `update_available:false`, `asset` omitted, exit 0 (2.2).
- Downgrade: `"downgrade":true`.

### 3.3 Failures

```json
{"op":"update","action":"check","success":false,"exit_code":25,
 "current_version":"v0.1.5",
 "error":{"code":"update_check_failed",
          "message":"the update service rate-limited this machine",
          "retry_after_secs":2700}}
```

```json
{"op":"update","action":"apply","success":false,"exit_code":27,
 "from_version":"v0.1.5","to_version":"v0.1.6","rolled_back":true,
 "error":{"code":"update_failed",
          "message":"cannot write to C:\\Tools\\tsdev (access denied)"}}
```

- `retry_after_secs`: present on rate-limit failures when
  `X-RateLimit-Reset`/`Retry-After` is parseable.
- `rolled_back`: present on apply failures after the swap started.
- `error.code` strings: `update_check_failed`, `update_not_found`,
  `update_failed`, `update_refused` (new table entries); plus existing
  `usage_error` (1), `cancelled` (12), `not_elevated` (11),
  `unknown_error` fallback.

---

## 4. Confirmation flows + flags

### 4.1 Capability-loss confirmation (mandated)

`kMinSelfUpdateVersion` is a compile-time constant = the first release
tag that ships this feature (expected `"0.1.6"`; set it to the actual
shipping tag before release -- every current release v0.1.0..v0.1.5 is
below it).

Digest precondition (verified fact): all currently published release
assets v0.1.0..v0.1.5 carry sha256 `digest` fields, so this
confirm-downgrade path cannot dead-end on the missing-digest refusal
(5, item 9.1) for any currently published version.

Trigger: `target < kMinSelfUpdateVersion`. Then (interactive only):

```
The requested version v0.1.5 cannot update itself.
If you install it, updating again later means downloading the release
zip from github.com/nohype/tsdev_automation and unzipping it by hand.
Install v0.1.5 anyway? [y/N]:
```

- Prompt mechanics: `CONIN$` + `ReadConsoleW` (existing precedent);
  `y`/`Y` accepts; bare Enter, `n`, EOF = decline; anything else
  re-prompts.
- Interactivity predicate (0.1): CONIN$ opens AND stderr is a console
  AND the console window is VISIBLE (`GetConsoleWindow() != NULL &&
  IsWindowVisible(GetConsoleWindow())`). The visibility clause is
  load-bearing and covers both the no-window case (`GetConsoleWindow`
  returns NULL -- no console at all) and the hidden-window case: an
  elevated child spawned with `SW_HIDE` (Python client, 10) owns a
  real but invisible console -- CONIN$ opens and `GetConsoleMode`
  succeeds, so a predicate without it would hang forever on
  `ReadConsoleW`. Hidden console => NON-interactive.
- Decline -> stdout `Update cancelled.`, exit **12** (`cancelled`,
  consistent with batch cooperative-cancel semantics).
- Non-interactive (CONIN$ unavailable, stderr not a console, or the
  console window absent/hidden): cannot prompt
  -> exit **28** `update_refused`:
  ```
  tsdev_cli: ERROR installing v0.1.5 requires confirmation: that version
             cannot update itself. Re-run interactively, or pass --yes
             to accept.
  ```
- `--yes`/`-y`: pre-answers the prompt (skips it even interactively).
  Required for scripts and for `client.update(yes=True)` (10).
- Under `--json`: the prompt still works (console present and
  visible); the result
  object is emitted after the decision. JSON carries
  `"requires_confirmation":true` on refusal frames.

### 4.2 Normal downgrade -- recommendation: NO extra confirmation

A downgrade to a version that CAN still update itself
(`kMinSelfUpdateVersion <= target < current`) does not prompt: typing an
explicit older version is already a deliberate act, and the escape hatch
is intact. The one-line `Downgrading ...` stderr notice (2.2) makes the
direction visible without adding friction. Rationale: the requirement
only mandates the capability-loss case; a second prompt trains users to
type-yes reflexes, weakening the prompt that actually matters.

### 4.3 `--force` policy overrides

`--force` bypasses:
- dev-build refusal (`0.0.0-dev` or any unparseable stamp),
- non-bundle-layout refusal (item 9.9),
- same-version no-op (`update v0.1.5` at v0.1.5 reinstalls -- the
  repair flow).

`--force` does NOT pre-answer the capability-loss confirmation; that is
`--yes`'s single job. (`update v0.1.5 --force` on a non-bundle dev copy
still prompts or exits 28.)

---

## 5. Error cases -- exit codes + exact messages

New production codes (see 0.2 -- 13 and 20-24 are taken):

| Exit | `error.code` | Meaning |
|---|---|---|
| 25 | `update_check_failed` | Cannot determine what exists: DNS/offline, HTTP error, rate limit, malformed feed JSON, no usable release list. |
| 26 | `update_not_found` | Feed worked; requested version is not a published release, or its release has no `tsdev_automation_<tag>.zip` asset. |
| 27 | `update_failed` | Fetch/verify/install failure: hash mismatch, missing digest, corrupt zip, extract failure, disk/write error, swap failure. |
| 28 | `update_refused` | Policy refusal needing explicit opt-in: dev build w/o `--force`, non-bundle dir w/o `--force`, required confirmation on a non-interactive channel w/o `--yes`. |
| 12 | `cancelled` | User answered `n`/Enter/EOF at the confirmation prompt. |
| 1 | `usage_error` | Bad grammar (`update foo`, `update check extra`, repeated value flag). |
| 11 | `not_elevated` | Existing gate; unchanged. |

Justification for few codes: callers (humans, Python) need exactly three
decisions -- "transient, retry" (25), "asked for something that doesn't
exist" (26), "local/integrity failure" (27), "needs a flag/consent" (28).
Per-failure-mode codes would put diagnostic detail in the exit code that
belongs in `--log-file`.

Exact human messages (stderr unless noted; `<dir>`/`v*` substituted):

| Case | Message | Exit |
|---|---|---|
| Offline/DNS/connect fail | `tsdev_cli: ERROR cannot check for updates -- the update service is unreachable. Check your internet connection and try again.` | 25 |
| HTTP error (non-403/5xx) | `tsdev_cli: ERROR the update service returned an unexpected response (HTTP <N>). Try again later.` | 25 |
| Rate limited (403 + `X-RateLimit-Remaining: 0`) | `tsdev_cli: ERROR the update service rate-limited this machine (too many requests). Try again in about <N> minutes.` | 25 |
| Malformed feed JSON | `tsdev_cli: ERROR the update service returned a response this tool cannot read. Try again later; if it repeats, reinstall manually from github.com/nohype/tsdev_automation/releases.` | 25 |
| Version not found | `tsdev_cli: ERROR no release "v9.9.9" was found. Published releases run from v0.1.0 to v0.1.5.` (range = min..max published semver; the whole `Published releases run from ...` clause is OMITTED when the feed has no published releases -- `update <v>` on an empty feed prints just `no release "v9.9.9" was found.`) | 26 |
| No bundle asset on the release | `tsdev_cli: ERROR release v0.1.6 has no install bundle. Reinstall manually from github.com/nohype/tsdev_automation/releases.` | 26 |
| Missing `digest` on asset | `tsdev_cli: ERROR release v0.1.6 cannot be verified (no checksum published); refusing to install it.` | 27 |
| Hash mismatch | `tsdev_cli: ERROR the downloaded update failed its integrity check and was discarded. Nothing was changed. Try again; if it repeats, reinstall manually.` | 27 |
| Corrupt zip / extract fail | `tsdev_cli: ERROR the update archive could not be unpacked (corrupt download). It was discarded; nothing was changed.` | 27 |
| No unpacker on this OS (tar.exe AND Expand-Archive absent) | `tsdev_cli: ERROR cannot unpack the update archive on this Windows version. Reinstall manually from github.com/nohype/tsdev_automation/releases.` | 27 |
| Archive missing payload members | `tsdev_cli: ERROR the update archive is missing required files; nothing was changed.` | 27 |
| Disk full / write denied | `tsdev_cli: ERROR cannot write to <dir> (<reason, e.g. "access denied"/"not enough space">). Nothing was changed.` | 27 |
| Swap fail, rollback OK | `tsdev_cli: ERROR update failed while installing; the previous version (v0.1.5) was restored. Try again, or reinstall manually from the release zip.` | 27 |
| Swap fail, rollback failed | `tsdev_cli: ERROR update failed while installing AND the previous version could not be fully restored. Reinstall manually: download tsdev_automation_<tag>.zip from github.com/nohype/tsdev_automation/releases and unzip it over <dir>.` | 27 |
| Update lock creation fails | `tsdev_cli: ERROR cannot acquire the update lock; nothing was changed. Try again; if it repeats, reinstall manually.` | 27 |
| Another update holds the lock | `tsdev_cli: ERROR another update is already running. Try again when it finishes.` | 27 |
| Dev build apply | `tsdev_cli: ERROR this is a development build (0.0.0-dev). Refusing to replace it with a release; pass --force to install the release anyway.` | 28 |
| Non-bundle dir | `tsdev_cli: ERROR <dir> does not look like a release bundle (missing tsdev_automation\ or README.md). Pass --force to update this copy anyway.` | 28 |
| Confirm on non-interactive | (4.1 text above) | 28 |
| `update foo` (bad token) | `Error: "foo" is not a valid version (expected e.g. v0.1.5 or 0.1.5, "check", or "latest")` + usage | 1 |
| `update check extra` (>1 positional) | `Error: too many arguments for update (expected at most one of: check, latest, <version>)` + usage | 1 |

`update check` output channels: verdict lines to stdout (success) or the
`tsdev_cli: ERROR` line to stderr (failure). Under `--json` the frame on
stdout replaces the stdout lines; stderr errors still print.

---

## 6. `--version` flag (new global)

- Global, position-free, non-repeatable-ish (repeats harmless), consumed
  by the global pre-scan like `--json`.
- Evaluated BEFORE the elevation guard: it is a global flag, not a
  subcommand; printing the tool's own version string is harmless.
  (Elevation is nearly unreachable anyway per 0.2.2, but ordering it
  first is the honest contract and makes support triage trivial.)
- Human output (stdout):
  ```
  tsdev_cli v0.1.5
  tsdev_cli 0.0.0-dev (development build)
  ```
- `--json`: `{"op":"version","success":true,"exit_code":0,
  "version":"v0.1.5"}` (adds `"dev_build":true` for dev stamps).
- Exit 0. `--version` combined with a subcommand: wins and exits --
  same "no subcommand, standalone" family as a lone `--backup-cache`;
  spec: `--version` prints and exits 0 regardless of other args (it is
  a metadata question, not an operation).
- Usage text: add one header line `tsdev_cli v<ver> -- manage the
  TSDev library without the TSDev UI` at the top of `printUsage()` and
  one usage line `  tsdev_cli [--json] --version` +
  `  tsdev_cli [--json] [--pbar] update [check | <version> | latest] [--yes] [--force]`
  in the command list. Any regression test asserting byte-exact usage
  text must be updated.

---

## 7. UAC expectations (copy for docs)

The tool already prompts for administrator permission on EVERY launch
(the exe manifest). `update` is no different: users see the same UAC
prompt they already click through; the update adds **zero** extra
prompts. Docs copy: *"Windows will ask for administrator permission when
the command runs -- that is normal; click Yes."* From the Python client
on a non-elevated session, `check_update()`/`update()` each cost one
UAC prompt (10).

---

## 8. `--update-url` / `TSDEV_UPDATE_URL` (testing override)

- Semantics: **full replacement of the API base URL.** Default
  `https://api.github.com/repos/nohype/tsdev_automation`; the CLI GETs
  `<base>/releases` and expects the GitHub releases-array JSON shape
  (1.2). Asset downloads use each asset's `browser_download_url`
  verbatim -- a test fixture just points that field back at the test
  server.
- Flag wins over env (standard mirror precedence); trailing `/` on the
  base tolerated.
- Marked **testing override**: hidden from usage text; documented only
  in BUNDLE_README's technical section. Env var additionally lets a
  parent process inject it without touching argv; the Python client
  forwards it as `--update-url` on elevated launches because `runas`
  drops the environment (0.1).
- Test harness contract (recommended shape): `python -m http.server`
  (or `pytest` fixture) serving:
  - `/releases` -> canned JSON array (the GitHub shape; fixtures for
    behind/up-to-date/draft-only/rate-limited-403 cases),
  - `/files/tsdev_automation_<tag>.zip` -> zips built from `dist/`.
  Client-side: `TSDEV_UPDATE_URL=http://127.0.0.1:<port>` env, or
  `--update-url http://127.0.0.1:<port>` on the command line.
- `--update-result-file <path>` (hidden, global flag, 1): the CLI
  ALWAYS attempts the write when the flag is given, and the precise
  rule is: at every point where a JSON result frame would be emitted
  under `--json`, `<path>` receives the same object whether or not
  `--json` was passed -- success, usage error (1, including
  pre-dispatch grammar rejections and the
  this-flag-on-a-non-update-subcommand usage error), cancelled (12),
  update_refused (28), every 25-27 failure, and not_elevated (11 --
  the elevation guard emits its JSON frame BEFORE dispatch, and the
  flag is already consumed by the global pre-scan by then). Exit
  paths that emit NO frame write nothing: a watchdog kill (13/15)
  and an SEH crash leave no file -- stated explicitly so the client
  can treat "no file" as "died before result emission" (10).
  Mechanics: atomic `.tmp` + rename (`--serve-pipe-file` precedent).
  If the atomic write itself fails, the terminal result is still
  reported on stdout/stderr normally and the exit code is UNCHANGED
  -- the result file is a capture convenience, never the contract;
  the failure is logged as
  `tsdev_cli: WARNING could not write update result file: <reason>`.
  Exists primarily for the Python client's elevated-launch capture
  (it is how the client obtains ANY result, 10); doubles as a test
  hook. Client-side mapping for all four exit/file combinations:
  (10).

---

## 9. Safety/robustness (user-visible semantics)

1. **Integrity:** after download, SHA-256 the zip bytes and compare to
   the release asset's `digest` (`sha256:` prefix). Missing/malformed
   digest -> refuse (exit 27), never verify-optional. Mismatch ->
   discard, exit 27.
2. **Staging:** download + extract into
   `<install_dir>\.tsdev_update_tmp_<pid>` (same volume -> rename-safe,
   cleaned/recreated each run). Extraction: OS-provided
   `C:\Windows\System32\tar.exe` (bsdtar, present on Windows 10 >=
   17063; zero new dependencies). Unpack capability is resolved BEFORE
   the download starts: `tar.exe` absent -> fall back to PowerShell
   `Expand-Archive`; both absent -> exit 27 `update_failed` ("cannot
   unpack the update archive on this Windows version", 5) before any
   bytes are fetched. Extract into the EMPTY staging dir,
   then copy only the whitelisted payload members (`tsdev_cli.exe`,
   `tsdev_automation\**`, `README.md`) -- `..`/absolute member names
   can never escape the staging sandbox because staging contents are
   moved explicitly, not trusted as paths.
3. **Staged swap:** completeness gate FIRST -- all whitelisted payload
   members must be present in staging before ANY rename happens:
   `tsdev_cli.exe`, a `tsdev_automation\` directory with >=1 file, and
   `README.md`. An incomplete payload -> exit 27 `update_failed`
   ("the update archive is missing required files; nothing was
   changed.", 5) -- fail closed like the integrity gate (9.1). Then
   for each payload member: rename existing `tsdev_cli.exe` ->
   `tsdev_cli.exe.old`, `tsdev_automation` -> `tsdev_automation.old`,
   `README.md` -> `README.md.old` (stale `.old` leftovers from a prior
   run are deleted first); move new files in. The running exe keeps
   working: rename does not disturb the loaded image.
4. **Rollback:** any failure before all members are in place -> rename
   `.old` files back over partial replacements, delete staging, exit 27
   with `rolled_back:true`. If restore itself fails -> `rolled_back:
   false` + the explicit reinstall-manually message (5). Atomicity is
   per-member best-effort; the `.old` set is the complete restore
   point.
5. **Cleanup:** a detached helper (spawned `CREATE_NO_WINDOW`, e.g.
   `powershell -NoProfile -Command "Wait-Process -Id <pid> -Timeout 60;
   Remove-Item ..."`) waits for this PID to exit (the `.old` exe is
   locked while we run) then deletes the `.old` siblings. Failure is
   harmless: stale `.old`/`.tsdev_update_tmp_*` leftovers are reaped
   best-effort at the start of every APPLY run and never affect
   correctness. `update check` is strictly read-only: it creates no
   staging dir, performs no reaping, and writes nothing anywhere --
   the single exception is an explicitly requested
   `--update-result-file` (8).
6. **Rename-failure fallback:** if renaming the running exe fails (ACL
   oddity): leave new files staged, spawn a detached script that waits
   for this PID then copies staging over the install dir; report exit 0
   with `Update will finish when this command exits.` on stdout and
   `"notes":["deferred_apply"]` in JSON.
7. **Other instances:** allowed unconditionally (rename trick tolerates
   them). Best-effort toolhelp snapshot for other `tsdev_cli.exe`
   processes -> conditional NOTE (2.2). A running `serve` broker holds
   the OLD image and keeps serving it until restarted -- a note, never
   a blocker.
8. **Self-concurrency:** a named mutex
   (`Local\tsdev_cli_update_<sha256-16>`) guards the apply phase,
   where `<sha256-16>` = first 16 hex chars of the lowercase SHA-256
   of the normalized install dir (full path lowercased, backslashes
   normalized to `/`, UTF-8 encoded) -- the raw path cannot be used:
   `\` is illegal in the object name itself (it separates namespace
   prefixes like `Local\` from the name), so embedding the install
   dir verbatim would make `CreateMutexW` fail every time.
   Mutex creation failure -> exit 27 `update_failed` (fail closed).
   A second concurrent `update` waits up to 60 s then exits 27
   `update_failed` ("another update is already running"). `update
   check` takes no mutex (read-only).
9. **Non-bundle layout gate (`--force` override lives in 4.3):** the apply path
   requires `tsdev_automation\__init__.py` AND `README.md` as siblings
   of the exe (a full release bundle). Missing -> exit 28 unless
   `--force` (covers stray copies of the exe, `build\Release` runs --
   those also hit the dev-build gate first since dev stamps are
   `0.0.0-dev`).
10. **No TS involvement:** `update` never loads orlib20/WHServer;
    `--ts-path` is accepted and ignored. `--backup-cache` combined with
    `update` still runs the cache phase first (uniform global contract);
    `--recycle-whserver`/`--batch-progress-control` on `update` are
    usage errors via the existing subcommand-restriction validators.
11. **HTTP details (log-only):** WinHTTP (system DLL, no new deps)
    opened with `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` (WPAD + system
    proxy -- zero-config for corporate users; the resolved proxy is
    logged at `-vv`); `User-Agent: tsdev_cli/<ver>` (GitHub requires
    one); TLS to `api.github.com` (and the asset's CDN redirect
    target); connect / send / receive timeouts 30/30/60 s; API
    response cap 4 MB; download streams to the staging file. Status
    codes, URLs, `X-RateLimit-*` headers -> `--log-file` only.
12. **Renamed exe:** if the `GetModuleFileNameW` basename is not
    `tsdev_cli.exe` (the user renamed the file), the swap proceeds
    normally -- the renamed binary becomes `<basename>.exe.old` and
    the payload always lands as a fresh `tsdev_cli.exe`. Emit the
    stderr note in 2.2 and add `renamed_exe` to JSON `notes` (3.2).

---

## 10. Python client surface

```python
client.check_update() -> TsDevResult
client.update(version: str | None = None, *, yes: bool = False,
              force: bool = False) -> TsDevResult
```

- **Never routed through the serve broker.** The broker IS the binary
  being replaced; both methods always launch `tsdev_cli --json update
  ...` directly, even when `use_serve=True` and a broker is running.
- Non-elevated session -- REGARDLESS of `use_serve` (the broker is not
  involved, so the flag changes nothing here): reuse the existing
  `ShellExecuteExW` runas elevation path -- one UAC prompt per call.
  Four launch details:
  1. `SEE_MASK_NO_CONSOLE` (`0x00008000`) is set in the
     `SHELLEXECUTEINFOW` so the console-subsystem child gets NO console
     at all -- belt-and-suspenders with the 4.1 visible-console
     predicate: even if a console materialized it would be invisible,
     so a confirmation need exits 28 instead of hanging on
     `ReadConsoleW` forever.
  2. stdout cannot be captured from a `runas` child, so the client
     passes the global `--update-result-file <tmp>` flag and reads
     the JSON result from `<tmp>` after the process exits.
  3. Bounded wait: reuse the existing `_wait_for_exit(handle,
     timeout_ms, terminate)` plumbing with a generous floor -- the
     client waits up to 15 minutes (never below 10 min / 600000 ms;
     a slow-link download plus the swap can take minutes). On
     timeout the client terminates the child and raises
     `TsDevError("update timed out")`. Terminate-mid-swap is
     permitted only because the apply phase's rollback + `*.old`
     recovery points make it safe (the client cannot observe which
     phase the child is in): `TerminateProcess` may deny the dying
     process cleanup time, skipping rollback and leaving `*.old`
     residue -- recover per the BUNDLE_README power-loss note.
     Timeout is exceptional, so that residue is acceptable.
  4. `TSDEV_UPDATE_URL`, when set in the parent env, is forwarded
     explicitly as `--update-url` (runas drops env).
  A declined UAC prompt raises `PermissionError` (same as broker
  start).
- Result-file contract (8): the file is written at every point a
  `--json` frame would be emitted, so the client maps child outcome
  to result over all four exit/file combinations:
  - exit 0 + file -> parse it into the `TsDevResult` (success);
  - exit 0 + no file -> `TsDevError` noting result capture failed
    (rare -- the write failed; the update itself may have
    succeeded, so the message says to check the install dir);
  - nonzero + file -> parse the frame and map `error.code` through
    `ERROR_CODE_MAP` as usual;
  - nonzero + no file -> the child died before result emission --
    an SEH/NTSTATUS-class code raises `TsDevCrashError`; any other
    code raises generic `TsDevError` with captured stderr text when
    present (runas captures none -> the message notes stderr was
    not capturable).
- Elevated session: plain `subprocess.run` via the existing `_run_cli`
  plumbing.
- Exit-28 mapping for the confirmation case: the CLI's stderr
  guidance is invisible through runas, so when `UpdateRefusedError`
  is raised for a missed confirmation the client appends the
  actionable remedy -- `re-run client.update(..., yes=True) to
  accept`.
- Args: `version=None` -> bare `update`; `"v0.1.5"`/`"0.1.5"`/`"latest"`
  passed through (CLI validates); `yes=True` -> `--yes`; `force=True`
  -> `--force`. `check_update()` takes no args.
- `TsDevResult` gains optional fields (all `None`/default on non-update
  ops), mirrored flat like `.exists`/`.stored_name`:
  `current_version`, `latest_version`, `update_available: bool`,
  `target_version`, `dev_build: bool`, `asset: dict | None`,
  `from_version`, `to_version`, `downgrade: bool`,
  `already_current: bool`, `files_replaced: int`, `notes: list[str]`,
  `rolled_back: bool`, `requires_force: bool`,
  `requires_confirmation: bool`.
- `TsDevErrorInfo` gains `retry_after_secs: int | None`.
- New exceptions + `ERROR_CODE_MAP` entries:
  `update_check_failed` -> `UpdateCheckError` (25),
  `update_not_found` -> `UpdateNotFoundError` (26),
  `update_failed` -> `UpdateFailedError` (27),
  `update_refused` -> `UpdateRefusedError` (28),
  `cancelled` -> `CancelledError` (12; also covers the batch-marker
  code if it ever surfaces through a raising path).
- `check_update()` never raises for "up to date"/"ahead"/"dev build" --
  those are verdicts, not failures (the `exists` not-found precedent).
  `update()` raises `CancelledError` on a declined prompt.
- When the client owns a live serve broker and `update()` succeeds:
  leave the broker running (not ours to kill mid-session), add
  `broker_restart_required` to `.notes`, and document that
  `client.close()` + re-enter (or a new `TsDevClient()`) picks up the
  new version. Old-broker requests remain valid -- the wire protocol is
  unchanged.

---

## 11. Docs impact checklist

- **`README.md`** -- new short section after "Useful flags":
  ```markdown
  ## Updating the tool

  The tool updates itself -- no installer, no re-download by hand:

      .\tsdev_cli.exe update check   # is a new version out?
      .\tsdev_cli.exe update         # install the latest version
      .\tsdev_cli.exe update v0.1.4  # switch to a specific version

  Windows asks for administrator permission as usual -- click Yes.
  To see your version: `.\tsdev_cli.exe --version`.
  ```
- **`docs/BUNDLE_README.md`** -- in the Native CLI section: the `update`
  grammar + `--yes`/`--force` flags; a new "Self-update" subsection
  covering: release feed (GitHub Releases, max published semver, drafts
  and prereleases ignored; PUBLIC-repo precondition), sha256
  verification, staged swap + `.old` cleanup, mid-update power-loss
  recovery (if the tool dir is left broken -- exe missing + `*.old`
  files present -- rename the `*.old` files back to recover), the
  serve-broker note, exit codes 25-28 + cancelled=12, the
  `--update-url`/`TSDEV_UPDATE_URL` + `--update-result-file` testing
  overrides (marked as such), `--version`. Python wrapper section:
  `check_update()`/`update()` semantics + new fields/exceptions + the
  never-via-broker + `broker_restart_required` notes.
- **`AGENTS.md`** -- Operations: add one line noting `update` is a
  seventh, NON-TS operation (no warehouse, no orlib20) with exit codes
  25-28; add `TSDEV_UPDATE_URL` to the env-var convention list; version
  stamping note (`TSDEV_VERSION_STRING` define, `0.0.0-dev` default).
- **`tests/run_unit_tests.ps1` / `tests/run_regression.ps1`** -- add the
  `-NoTs` update suites (feed-fixture pytest + any C++ unit tests for
  semver parse/compare); update is fully testable without TS.

---

## 12. Rationale + deliberate exclusions

Why this UX fits non-developers:

- **One command, zero config.** `tsdev_cli update` -- no URLs, no
  account, no tokens, no "channel" concept, no config file. The user
  never needs to know GitHub exists; only failure messages mention it,
  and only to say where a manual reinstall lives.
- **Plain words, honest numbers.** `Updated: v0.1.5 -> v0.1.6.` Every
  failure says what happened, that nothing (or everything) was changed
  back, and the single next step.
- **The safe default is always silence or refusal.** Integrity failures
  discard; policy questions refuse with the exact flag to pass; the one
  genuinely surprising outcome (a version that can never update again)
  is the ONLY thing that prompts.
- **Progress is three lines.** Non-developers need to know it is
  working, not watch bytes; diagnosable detail is `--log-file`'s job
  (existing convention).

Deliberately excluded (and why):

- Channels/track selection (`stable`/`beta`) -- one linear release
  line; prereleases are ignored, which IS the stable-channel policy.
- Prerelease opt-in flag -- the draft `v0.2.0_remote-control` must
  never be picked up; adding opt-in adds a way to install unfinished
  builds for zero end-user gain.
- Automatic/background update checking -- no scheduler, no phone-home
  on other subcommands; update happens only when asked.
- Rollback subcommand (`update --rollback`) -- `.old` files are
  transient swap artifacts, not a restore feature; `update <version>`
  already does everything.
- `version` subcommand AND `--version` flag both -- the flag alone.
- Authenticated GitHub access / token config -- 60/hr anonymous is
  ample for a manually-invoked command; no credential UX.
- Multiple `update`-path config, install-dir relocation -- the tool
  updates where it lives; users who want it elsewhere move the folder.

---

## Appendix A -- Exit-code map (all codes after this feature)

`0` success | `1` usage | `2` unknown type | `3` source/output file |
`4` init | `5` not_found/already_exists | `6` entry op | `7` saveSource |
`8` compile | `9` artifact | `10` thread | `11` not_elevated |
`12` cancelled (batch markers, Python elevation-required, AND update
decline) | `13` diag watchdog (diag build only) | `14` VAS abort |
`15` dispatch watchdog | `16` cache I/O | `17` cache cancelled |
`18` WHServer recycled | `19` recycle refused | `20-24` fatal
termination containment | **`25` update_check_failed** |
**`26` update_not_found** | **`27` update_failed** |
**`28` update_refused** | SEH NTSTATUS on crash.

## Appendix B -- Version stamping (build contract)

- CMake resolves `git describe --tags --exact-match HEAD`; on a tag
  `v0.1.6` -> `-DTSDEV_VERSION_STRING=0.1.6`; otherwise (dev/CI-less
  builds) -> `0.0.0-dev`. `kTsdevCliVersion` constant =
  `TSDEV_VERSION_STRING`.
- `kMinSelfUpdateVersion = "0.1.6"` constexpr -- the first release
  shipping this feature; update the literal if the shipping tag
  differs. A build whose own version < kMinSelfUpdateVersion can still
  self-update (the OLD binary does the swapping; capability gates the
  TARGET).

## Appendix C -- Adversarial edge matrix

| Input | Expected |
|---|---|
| `update` already latest | `You are up to date (vX.Y.Z).`, exit 0 |
| `update check` offline | `update service is unreachable`, exit 25 |
| `update check` rate-limited | minutes-until-retry message + `retry_after_secs`, exit 25 |
| `update 0.1.5` / `v0.1.5` / `latest` | equivalent target resolution |
| `update v0.1.5` at v0.1.5 | `Already at v0.1.5 -- nothing to do.`, exit 0 (`--force` reinstalls) |
| `update 0.2.0_remote-control` | usage error exit 1 (not semver) |
| `update v0.2.0` while draft | exit 26 not-found (drafts invisible) |
| `update check extra` | usage error exit 1 |
| `update` in `build\Release` | dev-build refusal exit 28 (stamp `0.0.0-dev`); `--force` then hits non-bundle check, also covered by `--force` |
| `update v0.1.4` (target < kMin) interactive | prompt; `n` -> `Update cancelled.` exit 12 |
| same, non-interactive | exit 28 + `--yes` guidance |
| same, `--yes` | installs without prompting |
| `update v0.1.10` vs `v0.1.9` | numeric semver: 0.1.10 > 0.1.9 |
| serve broker running during update | succeeds; `other_instances_running` note |
| second `update` mid-apply | waits 60 s -> `another update is already running`, exit 27 |
| release without `digest` | refuse exit 27 (never verify-optional) |
| `update --json` mid-flow decline | `{"op":"update","action":"apply","success":false,"exit_code":12,"error":{"code":"cancelled",...}}` |
| `update`/`update check` on empty published feed | `No published releases were found to update to.`, exit 0; JSON `update_available:false`, `latest_version:null` |
| `update v9.9.9` on empty published feed | `no release "v9.9.9" was found.` (range clause omitted), exit 26 |
| `update --yes v0.1.5` / `update v0.1.5 --yes` | flags legal in any position; identical behavior |
| `update check --force` | accepted, unused (strictly read-only, no writes) |
| repo still PRIVATE | every feed call 404s -> exit 25 `update_check_failed` (expected pre-flip) |
| exe renamed to `mytool.exe` | swap proceeds; fresh `tsdev_cli.exe` installed; `renamed_exe` note + stderr line |
| zip missing `tsdev_automation\` or `README.md` | exit 27 `missing required files` BEFORE any rename; nothing changed |
| tar.exe AND `Expand-Archive` absent | exit 27 before download (`cannot unpack ... on this Windows version`) |
| mutex creation fails | exit 27 `update_failed` (fail closed) |
| elevated child with hidden console hits confirm | invisible console => non-interactive => exit 28 + `--yes` guidance (NO hang) |
| client runas launch | `SEE_MASK_NO_CONSOLE` + global `--update-result-file` + bounded `_wait_for_exit` (15 min); timeout -> `TsDevError("update timed out")` |
| `insert ... --update-result-file f` | usage error exit 1 (global flag, update-only); `f` still gets the usage-error frame |
| nonzero exit + no result file | SEH-class code -> `TsDevCrashError`; else `TsDevError` (stderr noted as not capturable) |
| exit 0 + no result file | `TsDevError` -- result capture failed; message says check install dir |
| mid-update power loss | exe missing + `*.old` files present -> manual recovery: rename `*.old` back |
