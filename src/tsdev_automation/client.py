# SPDX-License-Identifier: MIT
"""The :class:`TsDevClient` wrapper around the ``tsdev_cli`` executable.

``tsdev_cli.exe`` is a 32-bit C++ binary that loads ``orlib20.dll`` from the
TradeStation 9.5 install.  It cannot be loaded in-process from 64-bit Python,
so this wrapper drives it as a subprocess using the ``--json`` structured
output flag (single-item) and the batch subcommands (``--json`` emits the
results array on stdout plus NDJSON progress on stderr).

The wrapper is **stdlib-only** (subprocess, tempfile, json, pathlib, ...).

Package-layout note
-------------------
The Python package lives at ``src/tsdev_automation/`` (uv_build src-layout,
auto-discovered).  The C++ sources live *directly* under ``src/`` and are
referenced by ``CMakeLists.txt`` via explicit paths (no globs), so the two
coexist without collision.
"""

from __future__ import annotations

import atexit
import ctypes
import json
import os
import queue
import subprocess
import tempfile
import threading
import time
import uuid
import warnings
from ctypes import wintypes
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Sequence

from . import _pipe
from .exceptions import ElevationRequiredError, TsDevCrashError, TsDevError, TsDevWatchdogAbortError, UpdateRefusedError, exception_for
from .results import BatchProgress, BatchProgressCallback, TsDevErrorInfo, TsDevResult

# The component types confirmed in tsdev_cli.cpp (parseType).
_VALID_TYPES: frozenset[str] = frozenset({"strategy", "function", "indicator", "tradingapp", "showme"})

# error.code strings a pre-validated manifest-line failure carries —
# the same mapping the C++ errorCodeString applies to the exit codes
# parseManifestLines can pre-populate (1 usage_error, 2 unknown_type,
# 3 source_file_error for non-get-source ops).
_PREVALID_ERROR_CODE: dict[int, str] = {
    1: "usage_error",
    2: "unknown_type",
    3: "source_file_error",
}

# --cache-backup-policy values accepted by the CLI (direct default "prompt";
# the Python/serve default is "abort").
_CACHE_BACKUP_POLICIES: frozenset[str] = frozenset({"prompt", "overwrite", "skip", "abort"})


def _validate_cache_policy(policy: str) -> str:
    """Lower-case and validate a cache backup policy; raise ``ValueError`` if unknown."""
    p = policy.strip().lower()
    if p not in _CACHE_BACKUP_POLICIES:
        raise ValueError(
            f"unknown cache backup policy {policy!r} (expected one of {sorted(_CACHE_BACKUP_POLICIES)})"
        )
    return p


def _serve_cache_policy(policy: str) -> str:
    """Map a cache backup policy to a valid serve ``policy`` value.

    ``"prompt"`` is unavailable under serve (the broker has no console), so
    it raises ``ValueError``; the other policies pass through verbatim.
    """
    if policy == "prompt":
        raise ValueError(
            "cache backup policy 'prompt' is unavailable under serve "
            "(no console): use 'skip', 'overwrite', or 'abort'"
        )
    return policy

# Subcommand name per high-level operation.
_SINGLE_SUBCMD: dict[str, str] = {"insert": "insert", "modify": "modify", "delete": "delete", "exists": "exists", "get-source": "get-source", "upsert": "upsert"}
_BATCH_SUBCMD: dict[str, str] = {"insert": "batch-insert", "modify": "batch-modify", "delete": "batch-delete", "upsert": "batch-upsert"}


# NT status codes that indicate a crash (VEH catches these and calls
# TerminateProcess with the NT status code as the process exit code).
_NT_CRASH_CODES: dict[int, str] = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC00000FD: "STACK_OVERFLOW",
    0xC0000409: "STACK_BUFFER_OVERRUN",
    0xC0000374: "HEAP_CORRUPTION",
}

# Exit code 15: the CLI's dispatch watchdog fired TerminateProcess(15) after
# a warehouse dispatch exceeded its bound or WHServer.exe crashed/vanished
# mid-flight (src/dispatch_watchdog.cpp).  This is NOT a crash —
# _is_crash_exit deliberately does not cover it — and the request must never
# be retried into the same wedged WHServer.
_WATCHDOG_ABORT_EXIT_CODE = 15


def _is_crash_exit(returncode: int) -> tuple[bool, str | None]:
    """Check if a subprocess returncode indicates a crash.

    Returns a ``(is_crash, crash_type_name)`` tuple.  ``crash_type_name`` is
    ``None`` when ``is_crash`` is ``False``.
    """
    if returncode is None:
        return False, None
    # subprocess.run returncode may be negative (signed) on Windows when the
    # process was terminated with a large NT status code; mask to 32 bits.
    rc = returncode & 0xFFFFFFFF
    if rc in _NT_CRASH_CODES:
        return True, _NT_CRASH_CODES[rc]
    return False, None


@dataclass
class BatchItem:
    """One item in a batch operation.

    ``source`` is required for insert/modify and must be ``None`` (or omitted)
    for delete.  ``source`` may be a path to an existing file (used directly)
    or a string of EasyLanguage source (written to a temporary ``.el`` file).

    ``prevalid_failure`` mirrors the C++ ``BatchItem::preValid=false``
    shape: a ``(exit_code, message)`` pair the console manifest reader
    (``_cli._read_manifest``) stamps on a line ``parseManifestLines``
    would reject.  Such an item is never dispatched — the batch path
    returns it as a per-item failure result in position instead of
    aborting the whole batch.
    """

    type: str
    name: str
    source: str | os.PathLike | None = None
    prevalid_failure: tuple[int, str] | None = None


def _validate_type(type_: str) -> str:
    """Lower-case and validate a component type; raise ``ValueError`` if unknown."""
    t = type_.strip().lower()
    if t not in _VALID_TYPES:
        raise ValueError(
            f"unknown component type {type_!r} (expected one of {sorted(_VALID_TYPES)})"
        )
    return t


_SERVE_PROGRESS_KEYS = frozenset({"batch_id", "index", "total", "phase"})


def _validate_serve_progress(obj: dict, batch_id: str, index: int, total: int) -> None:
    """Validate a tagged serve response's top-level ``progress`` member.

    Runs before :meth:`TsDevResult.from_json`: the member must be a dict with
    exactly ``batch_id``/``index``/``total``/``phase``, echoing the request's
    batch id and 1-based item position, with ``phase == "completed"``.  Any
    deviation raises :class:`TsDevError`.
    """
    if not isinstance(obj, dict):
        raise TsDevError(
            message=f"serve batch response is not a JSON object: {obj!r}"
        )
    progress = obj.get("progress")
    if not isinstance(progress, dict):
        raise TsDevError(
            message=f"serve batch response missing or malformed 'progress' member: {progress!r}"
        )
    if set(progress) != _SERVE_PROGRESS_KEYS:
        raise TsDevError(
            message="serve batch response 'progress' must have exactly "
            f"batch_id/index/total/phase members, got {sorted(progress)!r}"
        )
    pid = progress["batch_id"]
    if not isinstance(pid, str) or pid != batch_id:
        raise TsDevError(
            message=f"serve batch response 'progress.batch_id' mismatch: expected {batch_id!r}, got {pid!r}"
        )
    idx = progress["index"]
    if isinstance(idx, bool) or not isinstance(idx, int) or idx != index:
        raise TsDevError(
            message=f"serve batch response 'progress.index' mismatch: expected {index}, got {idx!r}"
        )
    tot = progress["total"]
    if isinstance(tot, bool) or not isinstance(tot, int) or tot != total:
        raise TsDevError(
            message=f"serve batch response 'progress.total' mismatch: expected {total}, got {tot!r}"
        )
    if progress["phase"] != "completed":
        raise TsDevError(
            message=f"serve batch response 'progress.phase' mismatch: expected 'completed', got {progress['phase']!r}"
        )


# Exit code the serve broker returns when the WHServer VAS-headroom guard
# refuses to dispatch a mutating request (error.code "whserver_vas_abort").
# Mirrors the batch-aggregate exit-14 contract of the direct-CLI path.
_VAS_ABORT_EXIT_CODE = 14

# Error-code string the broker sets on a VAS-guard refusal (from the C++
# errorCodeString for exit 14).  The batch abort check requires BOTH the
# exit code and this error code so a future, unrelated exit-14 producer
# cannot accidentally abort a batch.
_VAS_ABORT_ERROR_CODE = "whserver_vas_abort"

# Message used for not-run markers after a broker VAS-guard refusal —
# verbatim from the C++ kVasAbortMarkerMessage (distinct from the
# cooperative-cancel message so the list says WHY the item never ran).
_VAS_ABORT_MARKER_MESSAGE = (
    "Batch aborted (WHServer VAS headroom) before this item was attempted"
)

# Serve-path recycle boundary codes (design §4.3/§6.2): on a VAS-guard
# breach with recycle authorized, the broker writes the refused item's
# response carrying the recycle outcome as its exit code, flushes the
# pipe, and exits with the SAME code as its process exit code — so the
# exit code alone is the authoritative signal (the broker is dead either
# way; no error.code cross-check like the exit-14 path uses). 18 =
# WHServer recycled — respawn the broker and resubmit from the refused
# index; 19 = recycle refused — abort the batch (the refused item keeps
# its real exit-19 result, the remainder gets not-run markers).
_WHSERVER_RECYCLED_EXIT_CODE = 18
_WHSERVER_RECYCLE_REFUSED_EXIT_CODE = 19

# Bound on respawn+resubmit per batch call (design §4.3, max_recycles = 3):
# each recycle yields a fresh ~1899 MB server, so a re-breach means
# pathology — stop and report, don't loop.  Each respawn is another
# ShellExecuteExW runas launch — a non-elevated client pays one UAC
# prompt per respawn.
_MAX_RECYCLES = 3

# Not-run marker message after a recycle REFUSAL (exit 19) — verbatim
# from the C++ kRecycleRefusedMarkerMessage (distinct from the
# cooperative-cancel and VAS-abort messages so the list says WHY).
_RECYCLE_REFUSED_MARKER_MESSAGE = (
    "WHServer recycle refused before this item was attempted"
)

# Wait-only reap bound for the exit-18 recycle-respawn path.  An exit-18
# broker is mid-GRACEFUL-teardown: the boundary frame was written and
# flushed, then `delete lib` -> OrlibExit releases its WHServer m_nLock
# session refs before worker return / ExitProcess.  TerminateProcess on
# that broker would orphan the +7 lock refs for minutes — the respawned
# broker's census would then see lock>0 -> census_lock -> exit 19, the
# deadlock the recycle design exists to prevent.  So the respawn reap
# only WAITS, never kills.  The bound must outlast even a wedged
# teardown: OrlibExit runs under the broker's dispatch watchdog
# (dispatchEnter'd; killed at <= --dispatch-timeout-secs, broker default
# 300 s, forwarded from TSDEV_DISPATCH_TIMEOUT_SECS), so the bound is
# watchdog window + margin, floored at 360 s.  A configured watchdog of
# 0 disables the broker-side bound — this client bound is then the ONLY
# bound on the reap.
_BROKER_DISPATCH_WATCHDOG_DEFAULT_SECS = 300
_RECYCLE_RESPAWN_REAP_FLOOR_MS = 360_000
_RECYCLE_RESPAWN_REAP_MARGIN_MS = 60_000


def _recycle_respawn_reap_timeout_ms() -> int:
    """Wait-only reap bound (ms) for the exit-18 respawned-broker teardown.

    Derived from the dispatch-watchdog window forwarded to the broker as
    ``--dispatch-timeout-secs`` (env ``TSDEV_DISPATCH_TIMEOUT_SECS``;
    broker default 300 s; valid range 0..86400; ``0`` disables the
    watchdog entirely, in which case this client-side bound is the ONLY
    bound on the reap).  The bound is watchdog-window + 60 s margin,
    floored at 360 s, so even a fully wedged teardown self-kills inside
    the window and the wait returns instead of expiring.  Env validity
    mirrors the ``_build_serve_parameters`` forwarding rule — an invalid
    value is dropped (the broker keeps its default), not an error.
    """
    v = os.environ.get("TSDEV_DISPATCH_TIMEOUT_SECS")
    if v and v.isascii() and v.isdigit() and int(v) <= 86400:
        watchdog_ms = int(v) * 1000
    else:
        watchdog_ms = _BROKER_DISPATCH_WATCHDOG_DEFAULT_SECS * 1000
    return max(
        _RECYCLE_RESPAWN_REAP_FLOOR_MS,
        watchdog_ms + _RECYCLE_RESPAWN_REAP_MARGIN_MS,
    )


# Direct-path resume contract (design §4.1/§4.2): on exit 18 the CLI has
# already emitted the full-length results array (attempted head + not-run
# marker tail) and written a resume manifest covering the marker tail;
# its path rides the ``whserver_recycled`` NDJSON event on stderr.  The
# client resubmits that manifest file to a fresh ``batch-<op>``
# invocation (bounded by ``_MAX_RECYCLES``) and deletes it once the
# resumed batch has RETURNED — terminal outcomes included.  When the
# resumed run raises instead (crash, timeout, protocol error) the file
# stays on disk for manual resubmission/forensics.
def _resume_manifest_path(stderr: str) -> str | None:
    """Extract the resume-manifest path from the child's stderr events."""
    path: str | None = None
    for line in stderr.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except (json.JSONDecodeError, ValueError):
            continue
        if (
            isinstance(obj, dict)
            and obj.get("event")
                in ("whserver_recycled", "whserver_recycle_refused")
            and isinstance(obj.get("manifest"), str)
            and obj["manifest"]
        ):
            path = obj["manifest"]
    return path


# Marker text replacing the CLI's generic "resubmit remaining items"
# tail when the exit-18 event carried no usable manifest path: the tail
# was never re-attempted AND there is no path to resubmit — the generic
# text would advise an impossible action. The wording stays neutral:
# "unavailable" rather than "absent" — the stderr diagnostic (appended
# via _resume_manifest_diagnostic when present) distinguishes a writer
# refusal (NO manifest exists) from a written manifest whose path was
# dropped from the event channel (the file exists on disk).
_RESUME_MANIFEST_UNAVAILABLE_MARKER_MESSAGE = (
    "resume manifest unavailable -- remaining items not re-attempted"
)


def _resume_manifest_diagnostic(stderr: str) -> str | None:
    """First ``tsdev_cli:`` stderr diagnostic about the resume manifest.

    The exit-18/19 event only carries the manifest PATH; the WHY lives in
    the human stderr lines — a writer refusal ("resume manifest cannot
    represent item N ("<name>") ... no manifest written"), a write/convert
    failure ("failed to write resume manifest ..."), or a written
    manifest whose non-ASCII path cannot ride the JSON event channel
    ("resume manifest path is not representable ... automatic resume
    cannot use it"). The marker surfaces it because the caller never
    sees the child's stderr otherwise.
    """
    for line in stderr.splitlines():
        line = line.strip()
        if line.startswith("tsdev_cli:") and "manifest" in line:
            return line[len("tsdev_cli:"):].strip()
    return None


def _resume_manifest_path_dropped(stderr: str) -> bool:
    """True when stderr shows a manifest WAS written but its path was
    dropped from the event (non-ASCII path — the C++ emitLine "automatic
    resume cannot use it" diagnostic). Distinct from a writer refusal:
    the file exists on disk and still references this batch's sources."""
    return "automatic resume cannot use it" in stderr


# Sentinel for ``_retained_resume_manifest``: the CLI reported a manifest
# written but its path could not be recovered from the event/stderr (a
# non-ASCII path is dropped from the JSON event channel — this is an
# ANSI-codepage build). The file exists on disk and still references this
# batch's temp sources, so the caller's cleanup must spare ALL temps —
# the referenced set cannot be enumerated without the path.
_RETAIN_ALL_TEMP_SOURCES = object()


def _manifest_path_suffix(path: str) -> str:
    """Marker-text suffix surfacing a retained resume-manifest path —
    the CLI reports it only on the child's stderr event, which the
    caller never sees unless the marker names it."""
    return f" (resume manifest: {path})"


def _rewrite_marker_message(result: TsDevResult, message: str) -> TsDevResult:
    """Return ``result`` with its exit-12 marker message replaced.

    Used when a not-run tail is returned without a resubmission (missing
    manifest / recycle bound): the CLI-emitted "resubmit remaining items"
    text would claim an action that did not and cannot happen, so the
    markers are re-worded to state the real reason.  Non-marker results
    pass through unchanged.
    """
    if result.exit_code != 12:
        return result
    if result.error is None:
        return replace(
            result, error=TsDevErrorInfo(code="cancelled", message=message)
        )
    return replace(result, error=replace(result.error, message=message))


# Cache-phase flags a resume resubmission must NOT carry: the cache phase
# already ran in the first invocation, and resume continues where the
# batch left off — re-running backup would re-trigger the
# existing-backup policy (``abort`` -> exit 17 kills the resumed run
# before the batch starts and, worse, previously caused the pending
# manifest to be discarded on the raise).
_CACHE_PHASE_PRESENCE_FLAGS: frozenset[str] = frozenset(
    {"--backup-cache", "--restore-cache", "--no-cache-compress"}
)
# The only cache flag that takes a separate value token in its space form
# (``--cache-backup-policy <mode>``); the ``=`` form is one token.
_CACHE_PHASE_VALUE_FLAG = "--cache-backup-policy"


def _strip_cache_phase_args(args: list[str]) -> list[str]:
    """Drop cache-phase flags from an argv segment (both ``--flag=value``
    and ``--flag value`` forms); every other token is preserved verbatim.
    """
    out: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == _CACHE_PHASE_VALUE_FLAG:
            i += 2  # space form: the next token is the policy value
            continue
        if a.startswith(_CACHE_PHASE_VALUE_FLAG + "="):
            i += 1
            continue
        is_cache = a in _CACHE_PHASE_PRESENCE_FLAGS or any(
            a.startswith(f + "=") for f in _CACHE_PHASE_PRESENCE_FLAGS
        )
        if not is_cache:
            out.append(a)
        i += 1
    return out


def _resume_batch_cmd(cmd: list[str], manifest_path: str) -> list[str] | None:
    """Build the resubmission command: same base flags MINUS the
    cache-phase flags (the cache phase ran in the first invocation — see
    ``_strip_cache_phase_args``), the non-dir ``batch-<op>`` subcommand
    and the resume manifest path (a manifest file is the only well-defined
    resume input — ``--batch-stdin`` and the dir-batch arguments are
    dropped)."""
    for i, tok in enumerate(cmd):
        if tok.startswith("batch-"):
            sub = tok[:-len("-dir")] if tok.endswith("-dir") else tok
            return _strip_cache_phase_args(cmd[:i]) + [sub, manifest_path]
    return None


def _unlink_quiet(path: str) -> None:
    """Best-effort resume-manifest cleanup — never raises into the batch."""
    try:
        os.unlink(path)
    except OSError:
        pass


# The ONE whitespace class of the manifest line schema — the same 6-char
# C-locale ``std::isspace`` set (``" \t\n\v\f\r"``) the C++ side pins as
# ``kManifestWs``.  Token splitting, trimming, and ``_manifest_name_token``
# quoting/refusal all use it: Python's generic all-whitespace split would
# ALSO treat \x0b\x1c-\x1f\x85\xa0... as whitespace, diverging from the C++
# parser and corrupting the carve-out keep-set (or silently retargeting a
# name) for names containing '\v'/'\f'.
_MANIFEST_WS = " \t\n\v\f\r"


def _manifest_first_ws(s: str) -> int:
    """Index of the first manifest-whitespace char in ``s``, or -1."""
    for i, c in enumerate(s):
        if c in _MANIFEST_WS:
            return i
    return -1


def _manifest_line_tokens(
    line: str,
) -> tuple[str, str | None, str, str | None] | None:
    """Tokenize ONE manifest line exactly as C++ ``parseManifestLines``.

    Returns ``None`` for blank/comment lines; otherwise ``(type_tok,
    name_tok, rest, quote_err)`` where ``rest`` is everything after the
    name token (the ``<source_file>`` field).  ``name_tok`` is ``None``
    when the quoted-name form is malformed — ``quote_err`` then
    distinguishes the two C++ pre-validation failures: ``"unterminated"``
    (no closing quote) or ``"malformed"`` (junk fused after the closing
    quote, e.g. ``"a"b``).  A ``type``-only line yields ``("", "")`` for
    name/rest so the caller can apply the C++ "name must not be empty"
    diagnosis.
    """
    trimmed = line.strip(_MANIFEST_WS)
    if not trimmed or trimmed.startswith("#"):
        return None
    sp1 = _manifest_first_ws(trimmed)
    if sp1 == -1:
        return trimmed, "", "", None
    type_tok = trimmed[:sp1]
    rest1 = trimmed[sp1 + 1:].strip(_MANIFEST_WS)
    if rest1.startswith('"'):
        close = rest1.find('"', 1)
        if close == -1:
            return type_tok, None, "", "unterminated"
        if close + 1 < len(rest1) and rest1[close + 1] not in _MANIFEST_WS:
            return type_tok, None, "", "malformed"
        return type_tok, rest1[1:close], rest1[close + 1:].strip(_MANIFEST_WS), None
    sp2 = _manifest_first_ws(rest1)
    if sp2 == -1:
        return type_tok, rest1, "", None
    return type_tok, rest1[:sp2], rest1[sp2 + 1:].strip(_MANIFEST_WS), None


def _manifest_source_paths(manifest_path: str) -> set[str] | None:
    """Source paths a retained resume manifest references.

    Reproduces the C++ ``parseManifestLines`` tokenization EXACTLY (same
    ``_MANIFEST_WS`` class, same quoted-name rule, lines split on ``'\\n'``
    only — NOT ``str.splitlines``, which also splits on '\\v'/'\\f') and
    returns the ``<source_file>`` token (the rest of the line after
    ``<type> <name>``, the name possibly double-quoted) for every item
    line; lines the C++ parser would mark pre-validation failures carry no
    source.  Returns ``None`` when the manifest cannot be read — the
    caller must then spare every temp source, because the referenced
    set cannot be enumerated.
    """
    try:
        with open(manifest_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    sources: set[str] = set()
    for line in text.split("\n"):
        toks = _manifest_line_tokens(line)
        if toks is None:
            continue
        _, name_tok, rest, _quote_err = toks
        if name_tok is None:
            continue  # malformed quoted name -> pre-validation failure
        if rest:
            sources.add(rest)
    return sources


def _manifest_name_token(name: str) -> str:
    """Render ``<name>`` in the manifest line schema — the same form the
    CLI's resume-manifest writer emits and ``parseManifestLines``
    re-reads.

    A name containing whitespace is double-quoted; a bare ``"`` in a
    whitespace-free name stays unquoted (it re-parses as a plain token).
    Names that can NEVER round-trip — a line break, a leading ``"``, or
    ``"`` combined with whitespace — are refused with ``ValueError``
    (fail-closed): an unquoted spaced name would re-parse to a DIFFERENT
    component, so e.g. a delete must never silently retarget.
    """
    if (
        "\r" in name
        or "\n" in name
        or name.startswith('"')
        or ('"' in name and any(c in _MANIFEST_WS for c in name))
    ):
        raise ValueError(
            f"component name {name!r} cannot be represented in the "
            "manifest line schema (a line break, a leading quote, or a "
            "quote combined with whitespace) -- such a name can still be "
            "used with the single-item operations (insert/modify/delete/"
            "upsert/exists), which pass it via argv or the serve JSON "
            "protocol instead of a manifest line"
        )
    if any(c in _MANIFEST_WS for c in name):
        return f'"{name}"'
    return name


def _recycle_bound_marker_message(max_recycles: int) -> str:
    """Not-run marker message when the recycle resubmit bound is hit."""
    return (
        f"Batch stopped after {max_recycles} WHServer recycle(s) "
        "before this item was attempted"
    )


# Exit codes a direct batch invocation can return when the cache phase
# (backup/restore) failed BEFORE the batch ran: 5 = no backup to restore,
# 16 = cache I/O error, 17 = backup cancelled.  In that case stdout carries
# a single result object — no results array exists.
_CACHE_PHASE_EXIT_CODES: frozenset[int] = frozenset({5, 16, 17})


def _raise_cache_phase_error(stdout: str, stderr: str, returncode: int) -> None:
    """Raise the typed exception when a cache-phase failure preempted a batch.

    With cache flags, the CLI runs backup/restore before the batch; a failure
    there exits with a ``_CACHE_PHASE_EXIT_CODES`` code and emits a SINGLE
    failed result object on stdout (the batch never ran, so no results
    array).  When the exit code is in that set and stdout parses as one
    failed result object, convert it through the same
    :func:`exception_for` path single ops use
    (:class:`TsDevCacheError` / :class:`TsDevCacheCancelledError` /
    :class:`NotFoundError`).  Anything else — including genuinely malformed
    output — is a no-op so the caller's normal protocol/array validation
    reports it.
    """
    if returncode not in _CACHE_PHASE_EXIT_CODES:
        return
    try:
        obj = json.loads(stdout)
    except (json.JSONDecodeError, ValueError):
        return
    if not isinstance(obj, dict):
        return
    try:
        result = TsDevResult.from_json(obj, stdout=stdout, stderr=stderr)
    except (TypeError, ValueError, AttributeError):
        return
    if result.success or result.error is None:
        return
    raise exception_for(result)


def _cancelled_result(
    op: str,
    type_: str,
    name: str,
    message: str = "Batch cancelled before this item was attempted",
) -> TsDevResult:
    """Synthesize the not-run marker for an item the batch never attempted."""
    return TsDevResult(
        op=op,
        type=type_,
        name=name,
        success=False,
        exit_code=12,
        error=TsDevErrorInfo(
            code="cancelled",
            message=message,
        ),
    )


def _invoke_progress(
    on_progress: BatchProgressCallback | None,
    index: int,
    total: int,
    result: TsDevResult,
) -> bool:
    """Invoke the batch ``on_progress`` callback for one actual result.

    Returns ``True`` when the callback returned ``False`` before the final
    item — the only cancellation signal.  Callback exceptions propagate
    unchanged to the caller (no markers, no further requests).
    """
    if on_progress is None:
        return False
    decision = on_progress(BatchProgress(index=index, total=total, result=result))
    return decision is False and index < total


def _resolve_cli_path(cli_path: str | os.PathLike | None) -> Path:
    """Resolve the path to ``tsdev_cli.exe``.

    Priority:
    1. explicit ``cli_path`` argument
    2. ``TSDEV_CLI_PATH`` env var
    3. bundle layout: ``<package_parent>/tsdev_cli.exe`` — the exe sits as a
       sibling of the ``tsdev_automation/`` package directory (e.g.
       ``dist/tsdev_cli.exe`` next to ``dist/tsdev_automation/``)
    4. source-tree layout: walk up from this file until a ``CMakeLists.txt`` is
       found, then return ``<repo>/build/Release/tsdev_cli.exe``
    5. fallback: ``here.parents[1] / "build/Release/tsdev_cli.exe"``
    """
    if cli_path is not None:
        return Path(cli_path)
    env = os.environ.get("TSDEV_CLI_PATH")
    if env:
        return Path(env)
    here = Path(__file__).resolve().parent
    # Bundle layout: the exe sits as a sibling of the tsdev_automation/ package
    # directory (e.g. dist/tsdev_cli.exe next to dist/tsdev_automation/). In the
    # source tree (src/tsdev_automation/client.py) there is no src/tsdev_cli.exe,
    # so this falls through to the CMakeLists.txt walk-up below.
    bundle_exe = here.parent / "tsdev_cli.exe"
    if bundle_exe.is_file():
        return bundle_exe
    # Walk up from the package directory looking for CMakeLists.txt (repo root).
    for parent in [here, *here.parents]:
        if (parent / "CMakeLists.txt").is_file():
            return parent / "build" / "Release" / "tsdev_cli.exe"
    # Fallback: relative to this file (src/tsdev_automation -> repo root).
    return here.parents[1] / "build" / "Release" / "tsdev_cli.exe"


# --- Elevation detection (Windows) ----------------------------------------

_TOKEN_QUERY = 0x0008  # TOKEN_QUERY from winnt.h (was 0x0020 = TOKEN_ADJUST_PRIVILEGES)
_TokenElevation = 20

_advapi32 = ctypes.windll.advapi32
_kernel32 = ctypes.windll.kernel32
_kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
_kernel32.TerminateProcess.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.restype = wintypes.BOOL
_kernel32.GetCurrentProcess.restype = wintypes.HANDLE
_kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
_kernel32.GetExitCodeProcess.restype = wintypes.BOOL
_advapi32.OpenProcessToken.argtypes = [
    wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE),
]
_advapi32.OpenProcessToken.restype = wintypes.BOOL
_advapi32.GetTokenInformation.argtypes = [
    wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
]
_advapi32.GetTokenInformation.restype = wintypes.BOOL


def _detect_elevation() -> bool:
    """Return ``True`` if the current process is running elevated (admin).

    Uses ``OpenProcessToken`` + ``GetTokenInformation(TokenElevation)`` — the
    same check the C++ CLI performs.  On non-Windows platforms (or any
    unexpected failure) returns ``False`` so the caller falls back to the
    non-elevated path.
    """
    try:
        token = wintypes.HANDLE()
        if not _advapi32.OpenProcessToken(
            _kernel32.GetCurrentProcess(),
            _TOKEN_QUERY,
            ctypes.byref(token),
        ):
            return False
        try:
            elevated = wintypes.DWORD()
            ret_len = wintypes.DWORD()
            if not _advapi32.GetTokenInformation(
                token,
                _TokenElevation,
                ctypes.byref(elevated),
                ctypes.sizeof(elevated),
                ctypes.byref(ret_len),
            ):
                return False
            return bool(elevated.value)
        finally:
            _kernel32.CloseHandle(token)
    except OSError:
        return False


# --- Serve broker lifecycle (ShellExecuteExW runas) -----------------------

# ShellExecuteExW constants.
_SEE_MASK_NOCLOSEPROCESS = 0x00000040
# SEE_MASK_NO_CONSOLE: the elevated update child gets NO console at all --
# belt-and-suspenders with the CLI's visible-console interactivity
# predicate (a hidden console would make a confirmation need exit 28
# instead of hanging on ReadConsoleW).
_SEE_MASK_NO_CONSOLE = 0x00008000
_SW_HIDE = 0

# Bounded wait for the elevated `update` child (runas path): a slow-link
# download plus the staged swap can take minutes; the floor is 10 min per
# the update design (docs/UPDATE_DESIGN.md Section 10).
_UPDATE_WAIT_TIMEOUT_MS = 15 * 60 * 1000
# Grace period for the post-timeout terminate+reap: long enough for the
# OS to tear the child down, short enough to fail fast when it refuses.
_UPDATE_KILL_WAIT_MS = 10 * 1000
_INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value


class _SHELLEXECUTEINFOW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("fMask", wintypes.ULONG),
        ("hwnd", wintypes.HWND),
        ("lpVerb", wintypes.LPCWSTR),
        ("lpFile", wintypes.LPCWSTR),
        ("lpParameters", wintypes.LPCWSTR),
        ("lpDirectory", wintypes.LPCWSTR),
        ("nShow", ctypes.c_int),
        ("hInstApp", wintypes.HINSTANCE),
        ("lpIDList", ctypes.c_void_p),
        ("lpClass", wintypes.LPCWSTR),
        ("hkeyClass", wintypes.HKEY),
        ("dwHotKey", wintypes.DWORD),
        ("hIcon", wintypes.HANDLE),
        ("hProcess", wintypes.HANDLE),
    ]


_shell32 = ctypes.windll.shell32
_shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(_SHELLEXECUTEINFOW)]
_shell32.ShellExecuteExW.restype = wintypes.BOOL


def _build_serve_parameters(ts_path: str | None, handshake_path: str, verbose: int = 0,
                            recycle_whserver: bool | None = None) -> str:
    """Build the broker command line using Windows-compatible argv quoting.

    ``verbose`` (the client's already-clamped level) appends that many
    ``-v`` flags; the CLI pre-scans them position-free before subcommand
    parsing, so placement is arbitrary (they go last, matching _base_cmd).

    ``recycle_whserver`` is the tri-state WHServer-recycle authorization
    (design tmp/RECYCLE_ENGINE_DESIGN.md Section 6.1/4.3 + the tri-state
    addendum): ``True`` forwards the explicit ``--recycle-whserver`` flag
    (Full mode), ``False`` forwards the explicit ``--no-recycle-whserver``
    opt-out (Off mode), and ``None`` (the default) forwards nothing -- the
    broker then follows the CLI's own default (RescueOnly) plus whatever
    ``TSDEV_RECYCLE_WHSERVER`` / ``TSDEV_NO_RECYCLE_WHSERVER`` env vars
    reach it.  The env forwarding below mirrors the CLI's precedence: an
    env-vs-env conflict resolves to the negative arm, and an explicit
    kwarg beats the opposite-polarity env var (runas does not inherit the
    caller's environment block, so the env vars would never reach the
    broker without this forwarding).

    WHServer VAS-headroom guard: ``TSDEV_WHSERVER_*`` env overrides do NOT
    reach the elevated broker — ``ShellExecuteExW`` ``runas`` spawns the
    child with the user's default environment, not the caller's environment
    block — so they are forwarded here as explicit CLI flags (which the
    pre-scan consumes before ``cmdServe`` argument validation).  Non-numeric
    or out-of-range MB values are dropped client-side, mirroring the C++
    ``parseMbThreshold`` env rule (garbage keeps the default) instead of
    becoming a hard flag error that would fail the whole broker launch.
    """
    args: list[str] = []
    if ts_path:
        args.extend(("--ts-path", ts_path))
    args.extend(("serve", "--serve-pipe-file", handshake_path))
    for env_name, flag in (
        ("TSDEV_WHSERVER_FREE_MB", "--whserver-free-mb"),
        ("TSDEV_WHSERVER_LARGEST_FREE_MB", "--whserver-largest-free-mb"),
        ("TSDEV_WHSERVER_ABORT_FREE_MB", "--whserver-abort-free-mb"),
        ("TSDEV_WHSERVER_ABORT_LARGEST_FREE_MB",
         "--whserver-abort-largest-free-mb"),
    ):
        v = os.environ.get(env_name)
        # isascii() guards isdigit(): Unicode digits (e.g. "²") pass
        # isdigit() but crash int(), breaking the drop-invalid contract.
        if v and v.isascii() and v.isdigit() and int(v) <= 1048575:
            args.extend((flag, v))
    # Any value other than "0" disables the guard (C++ env semantics).
    if os.environ.get("TSDEV_NO_WHSERVER_VAS_GUARD") not in (None, "", "0"):
        args.append("--no-whserver-vas-guard")
    # Same env->flag forwarding for the dispatch-watchdog bound:
    # TSDEV_DISPATCH_TIMEOUT_SECS does not reach the elevated broker either.
    # Valid range mirrors the C++ parseTimeoutSecs rule: strict
    # decimal "0".."86400" (0 disables); anything else is dropped
    # client-side so the broker keeps its default bound.
    v = os.environ.get("TSDEV_DISPATCH_TIMEOUT_SECS")
    if v and v.isascii() and v.isdigit() and int(v) <= 86400:
        args.extend(("--dispatch-timeout-secs", v))
    # Same forwarding for the strategy-root traversal switch: any value
    # other than "0" restores the legacy traversal in the broker.
    if os.environ.get("TSDEV_TRAVERSE_STRATEGY_DEPENDENTS") not in (None, "", "0"):
        args.append("--traverse-strategy-dependents")
    # Tri-state WHServer-recycle forwarding (runas drops the caller's env,
    # so the env arms must ride the command line as explicit flags).
    # Precedence mirrors the CLI's resolveRecycleMode: an explicit kwarg
    # beats the opposite-polarity env var; with kwarg None an env-vs-env
    # conflict resolves to the negative arm; an env value of "0"
    # neutralizes only its own arm.
    if recycle_whserver is True:
        args.append("--recycle-whserver")
    elif recycle_whserver is False:
        args.append("--no-recycle-whserver")
    else:
        recycle_env = os.environ.get("TSDEV_RECYCLE_WHSERVER") not in (None, "", "0")
        no_recycle_env = (
            os.environ.get("TSDEV_NO_RECYCLE_WHSERVER") not in (None, "", "0")
        )
        if recycle_env and no_recycle_env:
            args.append("--no-recycle-whserver")
        elif recycle_env:
            args.append("--recycle-whserver")
        elif no_recycle_env:
            args.append("--no-recycle-whserver")
    # Same env->flag forwarding for the debug log file: TSDEV_LOG_FILE does
    # not reach the elevated broker (runas default env), so it is forwarded
    # as an explicit --log-file flag. Any non-empty value forwards verbatim
    # (path validation is the CLI's; an open failure only warns and never
    # aborts the broker). The broker records the full diagnostic stream at
    # every level -- its stderr is discarded anyway, so the file is the only
    # record. Direct subprocess calls need no forwarding: they inherit the
    # caller's environment block and the CLI reads TSDEV_LOG_FILE itself.
    v = os.environ.get("TSDEV_LOG_FILE")
    if v:
        args.extend(("--log-file", v))
    args.extend(["-v"] * verbose)
    # ShellExecuteExW receives a command-line string, not an argv list.
    # list2cmdline uses the quoting rules expected by the Windows C runtime.
    return subprocess.list2cmdline(args)


# Brief client-side stand-in for the broker's §6.3 stderr warning — the
# broker is spawned hidden and its stderr is discarded, so serve-mode users
# would never see the full text. Emitted via warnings.warn whenever the
# effective authorization is on (direct path included, where the child's
# captured stderr is likewise not surfaced by default).
_RECYCLE_WHSERVER_WARNING = (
    "recycle_whserver authorizes restarting TradeStation's background "
    "library server (WHServer.exe) when its memory runs low. The batch "
    "may still stop safely -- for example when another TradeStation "
    "program is using the server -- and writes a resume file for the "
    "remaining items. See docs/BUNDLE_README.md."
)


def _warn_recycle_whserver() -> None:
    """Emit the client-side §6.3 stand-in warning (see the constant)."""
    warnings.warn(_RECYCLE_WHSERVER_WARNING, UserWarning, stacklevel=3)


class _ServeBroker:
    """Manages the elevated ``tsdev_cli serve`` broker process.

    The broker is started once (via ``ShellExecuteExW`` with ``lpVerb="runas"``,
    triggering a single UAC prompt) and held open for the lifetime of the
    owning :class:`TsDevClient` instance.  Operations are sent as framed JSON
    over a named pipe.

    The broker is **lazy-started** on the first operation (not in the
    constructor) and shut down via :meth:`shutdown` (registered with
    :mod:`atexit` as a safety net).
    """

    def __init__(self, cli_path: Path, ts_path: str | None = None, verbose: int = 0,
                 recycle_whserver: bool | None = None) -> None:
        self._cli_path = cli_path
        self._ts_path = ts_path
        self._verbose = verbose
        # Tri-state WHServer-recycle authorization (design Section 6.1/4.3 +
        # the tri-state addendum): True forwards --recycle-whserver (Full),
        # False forwards --no-recycle-whserver (Off), None forwards nothing
        # (the broker follows the CLI default + its own env).
        self._recycle_whserver = recycle_whserver
        # True when the ctor kwarg EXPLICITLY opted out (False): the
        # forwarded --no-recycle-whserver beats the opposite-polarity
        # TSDEV_RECYCLE_WHSERVER env, so a later env var is shadowed by
        # design and must not trigger the env-after-launch warning.
        self._recycle_explicit_optout = recycle_whserver is False
        # The launch-time effective Full authorization (kwarg True, or the
        # env read at spawn when the kwarg is None): the flag lives on the
        # broker's command line and runas does not inherit the caller's
        # env, so TSDEV_RECYCLE_WHSERVER set after the broker started is
        # silently inert -- the batch paths warn on it.
        self._recycle_authorized_at_launch = False
        self._process_handle: int | None = None
        self._pipe_handle: int | None = None
        self._pid: int | None = None
        self._started = False
        self._shutdown_registered = False

    def start(self) -> None:
        """Start the broker via ``ShellExecuteExW`` (one UAC prompt).

        Steps:
        1. Create a temp file path for the pipe-name handshake.
        2. ``ShellExecuteExW`` with ``SEE_MASK_NOCLOSEPROCESS`` + ``runas`` +
           ``serve --serve-pipe-file <tmp>`` + ``SW_HIDE``.
        3. Poll the temp file (up to 30 s) for the pipe name line.
        4. Connect to the pipe via :func:`_pipe.connect`.

        Raises :class:`PermissionError` if the user declines the UAC prompt,
        :class:`TimeoutError` if the broker does not write the pipe file in
        time, or :class:`TsDevError` on other failures.
        """
        if self._started:
            return
        # A failed start may leave a live process handle when Windows refuses
        # TerminateProcess or the post-termination wait is inconclusive.  Do
        # not replace that handle with a new broker; clean it up first.
        if self._process_handle is not None or self._pipe_handle is not None:
            self._reset_pipe()
        # Validate the CLI path before invoking ShellExecuteExW.  Without this
        # check, a missing CLI binary would surface as a Windows "cannot find
        # file" dialog rather than a clean Python FileNotFoundError — the same
        # validation the subprocess.run path applies in _run_cli/_run_batch.
        if not self._cli_path.is_file():
            raise FileNotFoundError(f"tsdev_cli.exe not found at {self._cli_path!s}")
        # 1. Temp file path for the pipe-name handshake.  We create the file
        #    so the path is valid on disk, then close it; the broker will
        #    overwrite it atomically (write to <path>.tmp then MoveFileExW).
        fd, tmp_path = tempfile.mkstemp(suffix=".pipe", prefix="tsdev_serve_")
        try:
            os.close(fd)
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

            # 2. ShellExecuteExW with runas.  lpParameters is one command-line
            #    string, so quote path arguments using the Windows C-runtime
            #    rules rather than interpolating them into the string.
            params = _build_serve_parameters(self._ts_path, tmp_path, self._verbose,
                                             recycle_whserver=self._recycle_whserver)
            # Pin the launch-time effective Full authorization BEFORE the
            # spawn: this is the same precedence _build_serve_parameters
            # just applied -- afterwards the broker's command line is
            # immutable, so an env var appearing later cannot reach it.
            # An explicit ctor opt-out (False) shadows the opposite-polarity
            # env by design (the forwarded --no-recycle-whserver beats it).
            if self._recycle_whserver is True:
                self._recycle_authorized_at_launch = True
            elif self._recycle_whserver is False:
                self._recycle_authorized_at_launch = False
            else:
                self._recycle_authorized_at_launch = (
                    os.environ.get("TSDEV_RECYCLE_WHSERVER")
                    not in (None, "", "0")
                )
            sei = _SHELLEXECUTEINFOW()
            sei.cbSize = ctypes.sizeof(sei)
            sei.fMask = _SEE_MASK_NOCLOSEPROCESS
            sei.hwnd = None
            sei.lpVerb = "runas"
            sei.lpFile = str(self._cli_path)
            sei.lpParameters = params
            sei.lpDirectory = None
            sei.nShow = _SW_HIDE

            try:
                ok = _shell32.ShellExecuteExW(ctypes.byref(sei))
                if not ok:
                    err = ctypes.windll.kernel32.GetLastError()
                    raise PermissionError(f"UAC declined or ShellExecuteExW failed (err={err})")
                if (
                    sei.hProcess is None
                    or int(sei.hProcess) == 0
                    or int(sei.hProcess) == _INVALID_HANDLE_VALUE
                ):
                    raise PermissionError("UAC declined: no process handle returned")
                self._process_handle = int(sei.hProcess)

                # 3. Poll the temp file (up to 30 s) for the pipe name, then
                #    connect.  The handshake file is removed by the outer
                #    finally block on every success and failure path.
                pipe_name: str | None = None
                deadline = time.monotonic() + 30.0
                while time.monotonic() < deadline:
                    if self._process_exited():
                        raise TsDevError(message="serve broker exited before writing the pipe file")
                    if os.path.isfile(tmp_path) and os.path.getsize(tmp_path) > 0:
                        try:
                            with open(tmp_path, "r", encoding="utf-8") as f:
                                pipe_name = f.readline().strip()
                        except OSError:
                            pipe_name = None
                        if pipe_name:
                            break
                    time.sleep(0.1)
                if not pipe_name:
                    raise TimeoutError("serve broker did not write the pipe file within 30s")

                # 4. Connect to the pipe (retry for ~5 s — broker may not have
                #    called ConnectNamedPipe yet).
                self._pipe_handle = _pipe.connect(pipe_name, timeout_ms=5000)
                self._started = True
                # Safety net: ensure the broker is killed if the Python process
                # exits without an explicit shutdown().
                atexit.register(self.shutdown)
            except Exception:
                # Close any connected pipe before cleaning up the process.
                if self._pipe_handle is not None:
                    pipe_handle = self._pipe_handle
                    self._pipe_handle = None
                    try:
                        _pipe.close(pipe_handle)
                    except Exception:
                        pass
                self._started = False
                # Terminate the orphaned broker before re-raising.  If this
                # cannot be confirmed, retain the process handle so a later
                # explicit shutdown can retry; never claim it was terminated.
                if self._process_handle is not None:
                    process_handle = self._process_handle
                    try:
                        exited = _wait_for_exit(process_handle, 5000)
                    except Exception:
                        exited = False
                    if exited:
                        try:
                            _kernel32.CloseHandle(wintypes.HANDLE(process_handle))
                        except Exception:
                            pass
                        self._process_handle = None
                raise
        finally:
            # This is deliberately scoped to the exact path returned by
            # mkstemp; cleanup must not touch any unrelated path.
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    def call(self, request: dict) -> dict:
        """Send a request and receive the response.

        If the pipe handle breaks mid-session (broker restarted), the broker
        is re-started and the request retried once — unless the broker has
        crashed (detected via its NT-status exit code), in which case a
        :class:`TsDevCrashError` is raised immediately, or the broker's
        dispatch watchdog fired (exit code 15), in which case a
        :class:`TsDevWatchdogAbortError` is raised and the request is NOT
        retried.
        """
        if not self._started:
            self.start()
        try:
            assert self._pipe_handle is not None
            _pipe.send_request(self._pipe_handle, request)
            return _pipe.recv_response(self._pipe_handle)
        except (_pipe.PipeError, OSError):
            # Check how the broker died (crash -> NT status; watchdog abort
            # -> 15) before considering a reconnect.
            if self._process_handle:
                exit_code = self._get_exit_code()
                if exit_code == _WATCHDOG_ABORT_EXIT_CODE:
                    # The broker's dispatch watchdog fired
                    # TerminateProcess(15): a warehouse dispatch exceeded its
                    # bound or WHServer.exe crashed/vanished mid-flight.  Do
                    # NOT reconnect/retry — a fresh elevated broker would
                    # spawn a new UAC prompt and retry the same request into
                    # the same wedged WHServer.  The abort detail (the
                    # "WATCHDOG ABORT: ..." line) goes to the broker's own
                    # stderr, which is not captured over the pipe; the exit
                    # code is the only machine-readable signal here.
                    self._reset_pipe()
                    raise TsDevWatchdogAbortError(
                        message=(
                            "serve broker aborted via dispatch watchdog "
                            "(exit 15): a warehouse dispatch exceeded its "
                            "bound or WHServer.exe crashed/vanished "
                            "mid-flight; the request's outcome is uncertain"
                        ),
                        exit_code=exit_code,
                    )
                if exit_code is not None:
                    is_crash, crash_type = _is_crash_exit(exit_code)
                    if is_crash:
                        self._reset_pipe()  # terminates and closes the old broker
                        raise TsDevCrashError(
                            message=f"serve broker crashed ({crash_type}, exit code 0x{exit_code & 0xFFFFFFFF:08X}). "
                            "This indicates a bug in the orlib20 integration.",
                            exit_code=exit_code,
                        )
            # Non-crash broken pipe — reconnect once.
            self._reset_pipe()
            self.start()
            assert self._pipe_handle is not None
            _pipe.send_request(self._pipe_handle, request)
            return _pipe.recv_response(self._pipe_handle)

    def shutdown(self) -> None:
        """Send ``{"op":"quit"}``, wait up to 5 s, terminate if still alive."""
        if not self._started:
            self._close_handles()
            return
        # Best-effort quit over the pipe.
        if self._pipe_handle is not None:
            try:
                _pipe.send_request(self._pipe_handle, {"op": "quit"})
            except (_pipe.PipeError, OSError):
                pass
        # Close the pipe handle.
        if self._pipe_handle is not None:
            _pipe.close(self._pipe_handle)
            self._pipe_handle = None
        # Wait for the process to exit, then terminate if still alive.  A
        # failed termination or an inconclusive wait is an error, and the
        # process handle is retained for a later retry.
        self._wait_and_close_process()
        self._started = False

    def __del__(self) -> None:
        try:
            self.shutdown()
        except Exception:
            pass

    # --- internals ---------------------------------------------------------

    def _reset_pipe(self, timeout_ms: int = 5000, *, terminate: bool = True) -> None:
        """Close the broken pipe and reap its broker.

        With the defaults this is the existing contract: wait up to
        ``timeout_ms`` (5 s) for the broker to exit, then
        ``TerminateProcess`` and confirm — the bound for hung/dead
        brokers.  ``terminate=False`` makes the reap WAIT-ONLY (no
        TerminateProcess ever); used ONLY by the exit-18 recycle-respawn
        path via :meth:`_reset_pipe_for_recycle_respawn`, where the
        broker is mid-graceful-teardown releasing its WHServer session
        refs and a kill would orphan them.
        """
        self._started = False
        # Close pipe handle.
        if self._pipe_handle is not None:
            pipe_handle = self._pipe_handle
            self._pipe_handle = None
            _pipe.close(pipe_handle)
        # Reap the old broker process so start() doesn't leak it.
        # _wait_and_close_process raises if a terminal state cannot be
        # confirmed; in that case the live process handle remains
        # available for retry.
        self._wait_and_close_process(timeout_ms, terminate=terminate)

    def _reset_pipe_for_recycle_respawn(self) -> None:
        """Reset the pipe for an exit-18 respawn, reaping the broker WAIT-ONLY.

        The exit-18 broker is mid-GRACEFUL-teardown — the boundary frame
        was written+flushed, then ``delete lib`` -> ``OrlibExit`` releases
        its +7 WHServer ``m_nLock`` session refs before worker return /
        ExitProcess.  ``TerminateProcess`` here would orphan those refs
        for minutes: the respawned broker's census sees lock>0 ->
        census_lock -> exit 19 — the deadlock the recycle design exists
        to prevent.  So this reap only WAITS, bounded generously by
        :func:`_recycle_respawn_reap_timeout_ms` (the broker's own
        dispatch-watchdog window + margin — even a wedged teardown
        self-kills inside it).  On bound expiry a ``TimeoutError``
        propagates — loud, never a silent kill.
        """
        self._reset_pipe(
            timeout_ms=_recycle_respawn_reap_timeout_ms(),
            terminate=False,
        )

    def _close_handles(self) -> None:
        if self._pipe_handle is not None:
            pipe_handle = self._pipe_handle
            self._pipe_handle = None
            _pipe.close(pipe_handle)
        # Even an unsuccessful start can leave a process handle.  Do not
        # discard it without first confirming that the process is terminal.
        self._wait_and_close_process()

    def _wait_and_close_process(
        self, timeout_ms: int = 5000, *, terminate: bool = True
    ) -> None:
        """Wait for and close the broker handle, or raise without closing it.

        ``terminate=False`` makes the wait wait-only (passed through to
        :func:`_wait_for_exit`) — on expiry the broker is left RUNNING,
        not killed, and the raised ``TimeoutError`` says why (the broker
        was mid-graceful-teardown releasing the WHServer session).
        """
        if self._process_handle is None:
            return
        process_handle = self._process_handle
        if not _wait_for_exit(process_handle, timeout_ms, terminate=terminate):
            if terminate:
                raise TimeoutError("serve broker process did not terminate")
            raise TimeoutError(
                "serve broker did not exit within the wait-only "
                "recycle-respawn reap bound while in graceful teardown "
                "releasing its WHServer session (m_nLock) refs -- left "
                "running, NOT terminated, so the teardown can finish "
                "without orphaning the lock"
            )
        _kernel32.CloseHandle(wintypes.HANDLE(process_handle))
        self._process_handle = None

    def _process_exited(self) -> bool:
        if self._process_handle is None:
            return True
        return _wait_for_exit(self._process_handle, timeout_ms=0)

    def _get_exit_code(self) -> int | None:
        """Get the broker process exit code, or ``None`` if still running.

        Uses ``GetExitCodeProcess``: returns ``None`` when the process is
        still alive (exit code == ``STILL_ACTIVE`` == 259) or when the handle
        is missing / the call fails.
        """
        if not self._process_handle:
            return None
        exit_code = wintypes.DWORD()
        ok = _kernel32.GetExitCodeProcess(wintypes.HANDLE(self._process_handle), ctypes.byref(exit_code))
        if not ok:
            return None
        if exit_code.value == 259:  # STILL_ACTIVE
            return None
        return exit_code.value


def _wait_for_exit(
    process_handle: int, timeout_ms: int, *, terminate: bool = True
) -> bool:
    """Wait for a process handle to exit, terminating it after a timeout.

    Returns ``True`` only when a terminal process state is confirmed by
    ``WaitForSingleObject``.  If the initial wait times out and
    ``timeout_ms > 0`` and ``terminate`` is true, ``TerminateProcess`` is
    attempted and a second wait confirms whether termination completed.  A
    failed termination call or a second wait timeout is reported as
    ``False``.  ``terminate=False`` makes the wait WAIT-ONLY: a
    ``WAIT_TIMEOUT`` result is reported as ``False`` and the process is
    left running — used by the exit-18 recycle-respawn reap, where the
    broker is mid-graceful-teardown and a kill would orphan its WHServer
    session refs.
    """
    WAIT_TIMEOUT = 0x00000102
    WAIT_OBJECT_0 = 0x00000000
    handle = wintypes.HANDLE(process_handle)
    result = _kernel32.WaitForSingleObject(handle, wintypes.DWORD(timeout_ms))
    if result == WAIT_OBJECT_0:
        return True
    if result == WAIT_TIMEOUT and timeout_ms > 0 and terminate:
        if not _kernel32.TerminateProcess(handle, wintypes.UINT(1)):
            return False
        result = _kernel32.WaitForSingleObject(handle, wintypes.DWORD(1000))
        return result == WAIT_OBJECT_0
    return False


class TsDevClient:
    """High-level Python client driving ``tsdev_cli.exe``.

    Parameters
    ----------
    cli_path:
        Path to ``tsdev_cli.exe``.  If ``None``, the ``TSDEV_CLI_PATH`` env
        var is consulted, then ``<repo>/build/Release/tsdev_cli.exe`` is used.
        When auto-discovered and the binary is missing, ``FileNotFoundError``
        is raised immediately.  When an explicit path is given, the existence
        check is deferred to the first operation.
    ts_path:
        Optional TradeStation install directory forwarded as ``--ts-path``.
        ``None`` lets the CLI auto-discover the install.
    timeout:
        Subprocess timeout in seconds.  ``None`` means no timeout.
    use_serve:
        When ``True`` (the default) and the Python session is NOT elevated,
        single-item operations are routed through an elevated ``tsdev_cli
        serve`` broker (started once via a single UAC prompt) instead of
        spawning a fresh elevated child per op — so a non-elevated session
        pays one UAC prompt for the whole process lifetime rather than one
        per operation.  When already elevated, or when ``use_serve=False`` is
        passed explicitly from an already elevated process, the existing
        ``subprocess.run`` path is used directly (zero overhead, no broker, no
        extra UAC).  Direct ``tsdev_cli`` subprocess mode requires an elevated
        administrator process.  A non-elevated Python caller with
        ``use_serve=False`` raises
        :class:`~tsdev_automation.exceptions.ElevationRequiredError` before
        launch; a non-elevated direct ``tsdev_cli`` invocation returns a
        not-elevated error (see AGENTS.md and docs/BUILD.md for the
        authoritative elevation contract, including the exit code).  Batch ops are
        routed through the broker by looping over individual
        items — the broker's long-lived TsDevLib (flushPerItem=false) makes
        this as efficient as the C++ batch path (no per-op process spawn, no
        per-op warehouse flush).  Per-item failures are collected into the
        returned list (NOT raised), matching the non-serve batch semantics.
    verbose:
        Log verbosity forwarded to the CLI as repeatable ``-v`` flags:
        0 = default (errors+warnings, no flags), 1 = info (``-v``),
        2 = debug (``-vv``).  Values outside ``[0, 2]`` are clamped,
        mirroring the CLI's repeatable ``-v``/``--verbose`` global flag.
        In serve mode the level is applied to the broker process at launch;
        broker stderr is not captured (the broker is launched hidden) and
        the serve protocol has no per-request verbosity control.
    recycle_whserver:
        Tri-state WHServer-recycle authorization (design
        tmp/RECYCLE_ENGINE_DESIGN.md Section 6.1/4.3 + the tri-state
        addendum).
        ``None`` (the default) forwards nothing: every batch command line
        and the serve broker follow the CLI's own default (RescueOnly --
        the recycle is reserved for the abort-floor rescue) plus whatever
        ``TSDEV_RECYCLE_WHSERVER`` / ``TSDEV_NO_RECYCLE_WHSERVER`` env
        vars are visible.  ``True`` forwards the explicit
        ``--recycle-whserver`` flag (Full mode: the recycle may also fire
        on a trigger-band breach) on batch-* command lines and to the
        serve broker at launch; ``False`` forwards the explicit
        ``--no-recycle-whserver`` opt-out (Off mode: a below-floor breach
        hard-aborts with exit 14) the same way -- the explicit flag is
        forwarded even on the broker launch because ``runas`` does not
        inherit the caller's environment block, so the negative
        ``TSDEV_NO_RECYCLE_WHSERVER`` env var could not reach the broker
        otherwise, and an explicit kwarg beats the opposite-polarity env
        var (the CLI's pinned precedence).  The flag AUTHORIZES the
        recycle -- the zero-client census gate still applies.  On the
        DIRECT path an explicit authorization (``True`` here or the
        ``TSDEV_RECYCLE_WHSERVER`` env var) combined with a batch
        ``on_progress`` callback raises ``ValueError`` before spawn (the
        ``--batch-progress-control`` channel belongs to the dying
        process); in SERVE mode authorization + ``on_progress`` is
        allowed — progress is tagged per-response and a recycle resumes
        the batch on a respawned broker (design §4.3): on an exit-18
        boundary frame the client respawns the broker and resubmits from
        the refused index — the refused item itself is retried — bounded
        at 3 recycles per batch call; a refusal (exit 19) aborts the
        batch with not-run markers, no respawn.  Serve-mode broker
        authorization is FIXED AT BROKER LAUNCH (the flag lives on the
        broker's launch command line): the per-call batch
        ``recycle_whserver=True`` kwarg therefore raises ``ValueError``
        on the serve path — use this ctor kwarg or the env var before the
        broker starts.  The broker's own §6.3 warning is written to broker
        stderr, which is discarded (the broker is spawned hidden), so
        serve-mode users never see it — the client surfaces a brief
        equivalent via :mod:`warnings` ONLY when authorization is
        explicit (this kwarg ``True`` or the ``TSDEV_RECYCLE_WHSERVER``
        env var), never for the RescueOnly default.  A machine/user-level
        ``TSDEV_RECYCLE_WHSERVER`` set outside this process reaches the
        broker through the broker's own environment block -- the client
        cannot veto it (an explicit ``False`` kwarg does shadow it on the
        broker's command line, by the CLI's flag-beats-env precedence).
        In serve mode each recycle respawns the elevated broker -- a
        non-elevated client faces one UAC prompt per respawn (unattended
        non-elevated runs must not set this).
    """

    def __init__(
        self,
        cli_path: str | os.PathLike | None = None,
        ts_path: str | None = None,
        timeout: float | None = None,
        use_serve: bool = True,
        verbose: int = 0,
        recycle_whserver: bool | None = None,
    ) -> None:
        resolved = _resolve_cli_path(cli_path)
        # Fast-fail only for the auto-discovered default; explicit paths are
        # checked lazily so test doubles can point at a stub.
        if cli_path is None and os.environ.get("TSDEV_CLI_PATH") is None and not resolved.is_file():
            raise FileNotFoundError(
                f"tsdev_cli.exe not found at {resolved!s}. "
                "Build it first (cmake --build build --config Release) or set "
                "TSDEV_CLI_PATH / pass cli_path=."
            )
        self.cli_path: Path = resolved
        self.ts_path: str | None = ts_path
        self.timeout: float | None = timeout
        self.verbose = max(0, min(int(verbose), 2))
        # Serve-broker integration (TVS-K.5).  When ``use_serve`` is True and
        # the Python session is NOT elevated, single-item ops are routed
        # through an elevated serve broker (one UAC prompt) instead of
        # spawning a fresh elevated child per op.  When already elevated, the
        # existing ``subprocess.run`` path is used directly.  Direct mode is
        # valid only for an elevated administrator process; with
        # ``use_serve=False`` and a non-elevated session, _run_cli/_run_batch
        # raise ElevationRequiredError before launching the CLI.
        self._use_serve = use_serve
        self._is_elevated = _detect_elevation()
        # Tri-state gated WHServer-recycle authorization (design
        # Section 6.1/4.3 + the tri-state addendum). None forwards nothing
        # (the CLI's RescueOnly default + its own env apply); True forwards
        # the explicit --recycle-whserver flag (Full); False forwards the
        # explicit --no-recycle-whserver opt-out (Off) -- on batch-*
        # command lines and on the serve broker launch (runas drops env,
        # so the negative env cannot reach the broker otherwise). The flag
        # authorizes the recycle -- the zero-client census gate still
        # applies.
        self._recycle_whserver = recycle_whserver
        self._broker: _ServeBroker | None = None  # lazy-init on first op

    # -- public API --------------------------------------------------------

    def backup_cache(self, on_existing: str = "abort") -> TsDevResult:
        """Back up the TradeStation cache dir (``<TS>\\Program\\Cache``).

        Standalone ``--backup-cache`` invocation.  ``on_existing`` selects the
        existing-backup policy: ``"abort"`` (the Python default — fail-safe,
        an existing backup is never silently overwritten or skipped),
        ``"prompt"``, ``"overwrite"``, or ``"skip"`` — any other value raises
        ``ValueError`` client-side.

        Note: ``"prompt"`` can never actually prompt under a piped subprocess
        (the child's stderr is not a console); the CLI degrades it to
        keep-existing + warning.  On the serve path there is no console at
        all, so ``"prompt"`` raises ``ValueError`` client-side rather than
        being silently remapped.

        The result's ``.cache`` dict carries the payload (``path``,
        ``files``/``bytes``/``elapsed_secs``/``created``, optional
        ``live_source``/``used_vss``/``skipped`` keys).  Raises
        :class:`~tsdev_automation.exceptions.TsDevCacheError` on cache I/O
        failure (exit 16) and
        :class:`~tsdev_automation.exceptions.TsDevCacheCancelledError` when
        the backup is cancelled (exit 17).
        """
        policy = _validate_cache_policy(on_existing)
        if self._serve_active():
            return self._call_broker_cache("backup-cache", _serve_cache_policy(policy))
        cmd = self._base_cmd() + ["--json", "--backup-cache", f"--cache-backup-policy={policy}"]
        result = self._run_cli(cmd)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def restore_cache(self) -> TsDevResult:
        """Restore the cache backup into ``<TS>\\Program\\Cache``.

        Standalone ``--restore-cache`` invocation.  The result's ``.cache``
        dict carries the payload (``path``, ``files``/``bytes``/
        ``elapsed_secs``).  Raises
        :class:`~tsdev_automation.exceptions.TsDevCacheError` on cache I/O
        failure (exit 16) and
        :class:`~tsdev_automation.exceptions.NotFoundError` when no backup
        exists (exit 5).
        """
        if self._serve_active():
            return self._call_broker_cache("restore-cache", None)
        cmd = self._base_cmd() + ["--json", "--restore-cache"]
        result = self._run_cli(cmd)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def cache_maintenance(
        self,
        *,
        backup: bool = False,
        restore: bool = False,
        on_existing: str = "abort",
    ) -> list[TsDevResult]:
        """Run standalone cache maintenance (backup then restore) in one call.

        Direct mode spawns ONE ``tsdev_cli`` process carrying both flags —
        identical to ``tsdev_cli --json --backup-cache --restore-cache`` —
        so the CLI runs its cache phase in the fixed order
        backup -> restore in a single process/invocation.  Serve mode sends
        the ``backup-cache`` / ``restore-cache`` broker requests
        sequentially in the same order.  Returns one ``TsDevResult`` per
        requested phase, in run order; each carries that phase's own
        ``.cache`` payload (the raw CLI's two-flag
        ``{"backup": ..., "restore": ...}`` shape is split per phase and
        never surfaces through this API).  A phase failure raises the typed
        exception (:class:`TsDevCacheError`, exit 16;
        :class:`TsDevCacheCancelledError`, exit 17;
        :class:`NotFoundError`, exit 5 when no backup exists to restore)
        and suppresses the later phase.

        ``on_existing`` is the existing-backup policy for the backup phase
        (Python-side default ``"abort"`` — see :meth:`backup_cache`); it is
        validated only when ``backup`` is requested.  ``"prompt"`` raises
        :class:`ValueError` on the serve path (the broker has no console).
        """
        if not backup and not restore:
            raise ValueError("cache_maintenance requires backup and/or restore")
        if self._serve_active():
            # Sequential broker requests in the fixed order; a failed
            # pre-request raises and suppresses the later phase.
            results: list[TsDevResult] = []
            if backup:
                results.append(self.backup_cache(on_existing=on_existing))
            if restore:
                results.append(self.restore_cache())
            return results
        cmd = (
            self._base_cmd()
            + ["--json"]
            + self._cache_flag_args(backup, restore, on_existing if backup else None)
        )
        result = self._run_cli(cmd)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        # The single emitted object nests per-phase payloads under
        # "backup"/"restore" when both flags ran, or carries one "cache"
        # payload for a single-phase run (from_json already mapped that to
        # result.cache).  Split into one result per phase, in run order.
        cache = result.cache if isinstance(result.cache, dict) else None
        two_phase = cache is not None and ("backup" in cache or "restore" in cache)
        results = []
        if backup:
            results.append(TsDevResult(
                op="backup-cache", type=None, name=None,
                success=True, exit_code=0,
                cache=cache.get("backup") if two_phase else cache,
            ))
        if restore:
            results.append(TsDevResult(
                op="restore-cache", type=None, name=None,
                success=True, exit_code=0,
                cache=cache.get("restore") if two_phase else cache,
            ))
        return results

    def insert(
        self,
        type: str,
        name: str,
        source: str | os.PathLike,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        """Insert a new component and compile/verify it.

        Raises :class:`~tsdev_automation.exceptions.AlreadyExistsError` if the
        component exists, :class:`~tsdev_automation.exceptions.CompileError` on
        compilation failure, etc.

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the CLI's
        cache phase before the operation (see :meth:`_cache_flag_args`).
        """
        return self._single("insert", type, name, source,
                            backup_cache=backup_cache, restore_cache=restore_cache,
                            cache_backup_policy=cache_backup_policy)

    def modify(
        self,
        type: str,
        name: str,
        source: str | os.PathLike,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        """Replace an existing component's source and recompile/verify it.

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the CLI's
        cache phase before the operation (see :meth:`_cache_flag_args`).
        """
        return self._single("modify", type, name, source,
                            backup_cache=backup_cache, restore_cache=restore_cache,
                            cache_backup_policy=cache_backup_policy)

    def upsert(
        self,
        type: str,
        name: str,
        source: str | os.PathLike,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        """Insert a new component, or modify the existing one if it already exists.

        Materializes `source` the same way insert/modify do (string -> temp .el file;
        existing file path -> used directly). Returns TsDevResult on success.
        Raises CompileError on compile failure, SourceFileError if the source file
        cannot be read, ValueError for an unknown component type. Never raises
        NotFoundError/AlreadyExistsError (upsert branches on existence by construction).

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the CLI's
        cache phase before the operation (see :meth:`_cache_flag_args`).
        """
        return self._single("upsert", type, name, source,
                            backup_cache=backup_cache, restore_cache=restore_cache,
                            cache_backup_policy=cache_backup_policy)

    def delete(
        self,
        type: str,
        name: str,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        """Delete an existing component.

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the CLI's
        cache phase before the operation (see :meth:`_cache_flag_args`).
        """
        return self._single("delete", type, name, None,
                            backup_cache=backup_cache, restore_cache=restore_cache,
                            cache_backup_policy=cache_backup_policy)

    def exists(
        self,
        type: str,
        name: str,
        *,
        case_insensitive: bool = False,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        """Query whether a component named `name` of `type` exists in the TSDev Library.

        Returns a TsDevResult with ``.exists`` (bool), ``.prog_id`` (int; 0 if not
        found) and ``.stored_name`` (the verbatim stored name when found, empty
        string when not found).

        When ``case_insensitive`` is True, the CLI is invoked with
        ``--case-insensitive`` so the name matches regardless of casing, and the
        verbatim stored name is reported via ``.stored_name``.  When False (the
        default), the name must match exactly (case-sensitive).

        Does NOT raise on not-found (not-found is a valid query result, success=True).
        Raises typed exceptions on real errors (unknown_type, usage_error, etc.).

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the CLI's
        cache phase before the operation (see :meth:`_cache_flag_args`).
        """
        t = _validate_type(type)
        # Serve-broker routing for exists.
        if self._serve_active():
            self._serve_cache_phase(backup_cache, restore_cache, cache_backup_policy)
            return self._call_broker_exists(t, name, case_insensitive)
        sub = _SINGLE_SUBCMD["exists"]
        cmd = (
            self._base_cmd()
            + ["--json"]
            + self._cache_flag_args(backup_cache, restore_cache, cache_backup_policy)
            + [sub, t, name]
        )
        if case_insensitive:
            cmd.append("--case-insensitive")
        result = self._run_cli(cmd)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def get_source(
        self,
        type: str,
        name: str,
        output_file: str | os.PathLike | None = None,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        """Retrieve the EasyLanguage source of an existing component.

        Returns a TsDevResult with ``.source`` (the EL source text).  Always uses
        ``--json``.  If ``output_file`` is given, the CLI also writes the source
        to that file.

        Raises :class:`~tsdev_automation.exceptions.NotFoundError` if the item is
        not found.
        Raises :class:`~tsdev_automation.exceptions.OutputWriteError` if writing
        to ``output_file`` fails.
        Raises :class:`ValueError` for an unknown component type (client-side
        validation).

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the CLI's
        cache phase before the operation (see :meth:`_cache_flag_args`).
        """
        t = _validate_type(type)
        # Serve-broker routing for get-source.
        if self._serve_active():
            self._serve_cache_phase(backup_cache, restore_cache, cache_backup_policy)
            return self._call_broker_get_source(t, name, output_file)
        sub = _SINGLE_SUBCMD["get-source"]
        cmd = (
            self._base_cmd()
            + ["--json"]
            + self._cache_flag_args(backup_cache, restore_cache, cache_backup_policy)
            + [sub, t, name]
        )
        if output_file is not None:
            cmd += ["--output", str(output_file)]
        result = self._run_cli(cmd)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def batch_insert(
        self,
        items: Sequence[BatchItem],
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-insert many items. Returns one result per item (in order).

        Per-item failures are reported in the returned list (NOT raised); only
        a manifest-level failure (exit 1) raises :class:`TsDevError`.  An
        item carrying :attr:`BatchItem.prevalid_failure` (the console
        manifest reader's mark for a line ``parseManifestLines`` would
        reject) is never dispatched — it appears as a per-item failure
        result in position, matching the C++ ``preValid=false`` shape.

        ``on_progress`` is invoked synchronously with a
        :class:`~tsdev_automation.results.BatchProgress` after each attempted
        item's actual result, success or failure, in input order.  Only a
        return value of ``False`` cancels — before the next item — and the
        returned list stays full length: unattempted items become exit-12
        ``cancelled`` markers that never invoke the callback.  A callback
        exception aborts the batch and propagates unchanged.  The semantics
        are identical on the serve path (tagged broker requests) and on the
        direct elevated path (``--batch-progress-control`` cooperative
        stdin control); a non-elevated ``use_serve=False`` session still
        raises ``ElevationRequiredError`` before the CLI is launched.

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the
        CLI's cache phase once before the batch (see :meth:`_cache_flag_args`).
        """
        return self._batch("insert", items, on_progress,
                           backup_cache=backup_cache, restore_cache=restore_cache,
                           cache_backup_policy=cache_backup_policy)

    def batch_modify(
        self,
        items: Sequence[BatchItem],
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-modify many items. See :meth:`batch_insert`."""
        return self._batch("modify", items, on_progress,
                           backup_cache=backup_cache, restore_cache=restore_cache,
                           cache_backup_policy=cache_backup_policy)

    def batch_delete(
        self,
        items: Sequence[BatchItem],
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-delete many items. See :meth:`batch_insert`."""
        return self._batch("delete", items, on_progress,
                           backup_cache=backup_cache, restore_cache=restore_cache,
                           cache_backup_policy=cache_backup_policy)

    def batch_upsert(
        self,
        items: Sequence[BatchItem],
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-upsert many items (insert-or-modify). See :meth:`batch_insert`."""
        return self._batch("upsert", items, on_progress,
                           backup_cache=backup_cache, restore_cache=restore_cache,
                           cache_backup_policy=cache_backup_policy)

    def batch_insert_dir(
        self,
        type: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None = None,
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-insert all .el files in a directory. File stem = item name, file content = EL source.

        ``extensions`` selects which file extensions to include. ``None`` (default)
        or an empty list uses the CLI defaults (``.el`` + ``.txt``). A non-empty
        list like ``["el"]`` selects only ``.el`` files; each element is passed to
        the CLI as a ``-t <ext>`` flag. Per-item failures are reported in the
        returned list (NOT raised); a directory-level failure (exit 1) raises
        :class:`TsDevError`. ``on_progress`` behaves exactly as in
        :meth:`batch_insert`; positions are sorted-entry positions, and a local
        source-read failure is an actual result that invokes the callback.

        ``backup_cache``/``restore_cache``/``cache_backup_policy`` run the
        CLI's cache phase once before the batch (see :meth:`_cache_flag_args`).
        """
        return self._batch_dir("insert", type, dir_path, extensions, on_progress,
                               backup_cache=backup_cache, restore_cache=restore_cache,
                               cache_backup_policy=cache_backup_policy)

    def batch_modify_dir(
        self,
        type: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None = None,
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-modify all .el files in a directory. See :meth:`batch_insert_dir`."""
        return self._batch_dir("modify", type, dir_path, extensions, on_progress,
                               backup_cache=backup_cache, restore_cache=restore_cache,
                               cache_backup_policy=cache_backup_policy)

    def batch_delete_dir(
        self,
        type: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None = None,
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-delete all .el files in a directory (file stem = item name; content ignored). See :meth:`batch_insert_dir`."""
        return self._batch_dir("delete", type, dir_path, extensions, on_progress,
                               backup_cache=backup_cache, restore_cache=restore_cache,
                               cache_backup_policy=cache_backup_policy)

    def batch_upsert_dir(
        self,
        type: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None = None,
        *,
        on_progress: BatchProgressCallback | None = None,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> list[TsDevResult]:
        """Batch-upsert all .el files in a directory. See :meth:`batch_insert_dir`."""
        return self._batch_dir("upsert", type, dir_path, extensions, on_progress,
                               backup_cache=backup_cache, restore_cache=restore_cache,
                               cache_backup_policy=cache_backup_policy)

    # -- self-update (non-TS operation; update design Section 10) ----------

    def check_update(self) -> TsDevResult:
        """Check whether a newer release is available (read-only).

        Runs ``tsdev_cli --json update check``.  Never raises for the
        "up to date"/"ahead"/"dev build" verdicts -- those are results,
        not failures (the ``exists`` not-found precedent); inspect
        ``result.update_available``/``result.latest_version``/
        ``result.dev_build``.  Feed failures raise
        :class:`~tsdev_automation.exceptions.UpdateCheckError` (exit 25).

        NEVER routed through the serve broker -- the broker IS the binary
        being replaced.  A non-elevated session launches the CLI through
        the ``ShellExecuteExW`` runas path (one UAC prompt per call)
        regardless of ``use_serve``.
        """
        return self._update_op(["update", "check"])

    def update(
        self,
        version: str | None = None,
        *,
        yes: bool = False,
        force: bool = False,
    ) -> TsDevResult:
        """Self-update (or downgrade) to a release.

        ``version=None`` (or ``"latest"``) installs the latest published
        release; ``"v0.1.5"``/``"0.1.5"`` install exactly that version
        (the CLI validates the spelling).  ``yes=True`` forwards
        ``--yes`` (pre-answers the capability-loss confirmation);
        ``force=True`` forwards ``--force`` (dev-build / non-bundle /
        same-version overrides).

        NEVER routed through the serve broker -- the broker IS the binary
        being replaced.  A non-elevated session launches the CLI through
        the ``ShellExecuteExW`` runas path (one UAC prompt per call)
        regardless of ``use_serve``; a declined prompt raises
        ``CancelledError`` (exit 12) and a non-interactive refusal raises
        :class:`~tsdev_automation.exceptions.UpdateRefusedError` (exit
        28, whose message carries the ``yes=True`` remedy).

        On success, when this client owns a LIVE serve broker the broker
        is left running (it keeps serving the old version) and
        ``"broker_restart_required"`` is added to ``result.notes`` --
        shut the client down (or start a new ``TsDevClient``) to pick up
        the new binary.
        """
        args = ["update"]
        if version is not None:
            args.append(version)
        if yes:
            args.append("--yes")
        if force:
            args.append("--force")
        result = self._update_op(args)
        if (
            result.success
            and self._broker is not None
            and self._broker._started
        ):
            notes = list(result.notes or [])
            if "broker_restart_required" not in notes:
                notes.append("broker_restart_required")
            result.notes = notes
        return result

    def _update_op(self, update_args: list[str]) -> TsDevResult:
        """Dispatch an ``update`` subcommand, never through the broker.

        ``update_args`` is the update-local argv tail (``["check"]`` or
        ``["update", <version>?, --yes?, --force?]``).  Elevated session:
        plain ``_run_cli``.  Non-elevated session (regardless of
        ``use_serve``): elevated runas launch + ``--update-result-file``
        capture (design Section 10).
        """
        if self._is_elevated:
            cmd = self._base_cmd() + ["--json"] + update_args
            result = self._run_cli(cmd)
        else:
            result = self._run_update_elevated(update_args)
        if not result.success:
            assert result.error is not None
            self._raise_update_error(result)
        return result

    @staticmethod
    def _raise_update_error(result: TsDevResult) -> None:
        """Raise the mapped exception for a failed update result.

        Exit-28 refinement (design Section 10): on a missed capability-
        loss confirmation the CLI's stderr guidance is invisible through
        the runas launch, so the raised UpdateRefusedError carries the
        actionable ``yes=True`` remedy.
        """
        assert result.error is not None
        if result.error.code == "update_refused" and result.requires_confirmation:
            raise UpdateRefusedError(
                result,
                message=(
                    f"{result.error.message} "
                    "Re-run client.update(..., yes=True) to accept."
                ),
            )
        raise exception_for(result)

    def _build_update_parameters(self, update_args: list[str], result_path: str) -> str:
        """Build the elevated-update command line for the runas launch.

        ``runas`` does not inherit the caller's environment block, so
        ``TSDEV_UPDATE_URL`` (when set) is forwarded explicitly as
        ``--update-url`` -- the same env->flag convention
        :func:`_build_serve_parameters` applies.  ``--update-url`` is an
        update-local flag (the C++ parses it only inside
        ``parseUpdateArgs``), so it is emitted AFTER the update args;
        placed before the ``update`` token it would become the
        subcommand and the launch would fail as a usage error.
        """
        args: list[str] = []
        if self.ts_path:
            args.extend(("--ts-path", self.ts_path))
        args.extend(("--json", "--update-result-file", result_path))
        args.extend(update_args)
        update_url = os.environ.get("TSDEV_UPDATE_URL")
        if update_url:
            args.extend(("--update-url", update_url))
        args.extend(["-v"] * self.verbose)
        return subprocess.list2cmdline(args)

    def _run_update_elevated(self, update_args: list[str]) -> TsDevResult:
        """Launch ``update`` elevated via ``ShellExecuteExW`` runas.

        Copies the :meth:`_ServeBroker.start` launch shape and adds
        ``SEE_MASK_NO_CONSOLE`` so the console-subsystem child gets no
        console at all (a hidden console's confirmation prompt would
        hang forever on ``ReadConsoleW``; with no console the CLI's
        interactivity predicate exits 28 instead).  stdout cannot be
        captured from a ``runas`` child, so the result JSON is read from
        the global ``--update-result-file`` the CLI writes at every
        JSON-frame emission point.  The wait is bounded at 15 minutes;
        on timeout the child is terminated (safe -- the apply phase's
        rollback + ``*.old`` recovery points bound the damage) and
        ``TsDevError`` is raised.
        """
        if not self.cli_path.is_file():
            raise FileNotFoundError(f"tsdev_cli.exe not found at {self.cli_path!s}")
        # Temp path for the result file: create it so the path is valid,
        # then remove the placeholder -- the CLI writes it atomically
        # (<path>.tmp + rename, the --serve-pipe-file precedent).
        fd, tmp_path = tempfile.mkstemp(suffix=".json", prefix="tsdev_update_")
        try:
            os.close(fd)
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

            params = self._build_update_parameters(update_args, tmp_path)
            sei = _SHELLEXECUTEINFOW()
            sei.cbSize = ctypes.sizeof(sei)
            sei.fMask = _SEE_MASK_NOCLOSEPROCESS | _SEE_MASK_NO_CONSOLE
            sei.hwnd = None
            sei.lpVerb = "runas"
            sei.lpFile = str(self.cli_path)
            sei.lpParameters = params
            sei.lpDirectory = None
            sei.nShow = _SW_HIDE

            ok = _shell32.ShellExecuteExW(ctypes.byref(sei))
            if not ok:
                err = ctypes.windll.kernel32.GetLastError()
                raise PermissionError(f"UAC declined or ShellExecuteExW failed (err={err})")
            if (
                sei.hProcess is None
                or int(sei.hProcess) == 0
                or int(sei.hProcess) == _INVALID_HANDLE_VALUE
            ):
                raise PermissionError("UAC declined: no process handle returned")
            process_handle = int(sei.hProcess)
            try:
                # Long wait is WAIT-ONLY: a False here means "still
                # running after 15 min", NOT "termination failed" --
                # _wait_for_exit(terminate=True) reports True on a
                # confirmed kill, which would silently swallow the
                # timeout.  Terminate explicitly, then always raise.
                if not _wait_for_exit(
                    process_handle, _UPDATE_WAIT_TIMEOUT_MS, terminate=False
                ):
                    # Mid-swap termination is permitted because the apply
                    # phase's rollback + *.old recovery points bound the
                    # damage (design Section 10).
                    _wait_for_exit(
                        process_handle, _UPDATE_KILL_WAIT_MS, terminate=True
                    )
                    raise TsDevError(message="update timed out")
                exit_dword = wintypes.DWORD()
                ok2 = _kernel32.GetExitCodeProcess(
                    wintypes.HANDLE(process_handle), ctypes.byref(exit_dword)
                )
                exit_code = int(exit_dword.value) if ok2 else None
            finally:
                _kernel32.CloseHandle(wintypes.HANDLE(process_handle))
            return self._read_update_result(tmp_path, exit_code)
        finally:
            # Best-effort cleanup of the result file and a lingering
            # atomic-write sibling (<path>.tmp) if the child died
            # mid-rename.
            for p in (tmp_path, tmp_path + ".tmp"):
                try:
                    os.unlink(p)
                except OSError:
                    pass

    def _read_update_result(self, result_path: str, exit_code: int | None) -> TsDevResult:
        """Map the elevated update child's outcome over the four
        exit/file combinations (design Section 10).

        The result file is written at every point a ``--json`` frame
        would be emitted, so "no file" means the child died before
        result emission.
        """
        text: str | None = None
        try:
            with open(result_path, "r", encoding="utf-8") as f:
                text = f.read()
        except OSError:
            text = None
        if exit_code == 0:
            if text is None or not text.strip():
                raise TsDevError(
                    message=(
                        "update result capture failed: the CLI exited 0 "
                        "but wrote no result file -- the update itself may "
                        "have succeeded; check the install dir"
                    )
                )
        elif text is None or not text.strip():
            # The child died before result emission (watchdog kill and
            # SEH crashes emit no frame).  runas captures no stderr.
            if exit_code is not None:
                is_crash, crash_type = _is_crash_exit(exit_code)
                if is_crash:
                    raise TsDevCrashError(
                        message=(
                            f"tsdev_cli.exe crashed during update "
                            f"({crash_type}, exit code "
                            f"0x{exit_code & 0xFFFFFFFF:08X})."
                        ),
                        exit_code=exit_code,
                    )
                raise TsDevError(
                    message=(
                        f"elevated update exited {exit_code} before "
                        "producing a result (stderr is not capturable "
                        "on the elevated launch)"
                    )
                )
            raise TsDevError(
                message=(
                    "elevated update produced no result and its exit "
                    "code is unavailable (stderr is not capturable on "
                    "the elevated launch)"
                )
            )
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            raise TsDevError(
                message=f"update result file is not valid JSON: {text!r}"
            )
        result = TsDevResult.from_json(obj, stderr="")
        # The result file is authoritative for its contents in both
        # off-spec combinations: exit 0 + a failure frame still maps the
        # frame through the error table (raise here), and a nonzero exit
        # carrying a success frame returns the success result as
        # recorded (the caller's not-success check handles nonzero +
        # failure frames the same way).
        if exit_code == 0 and not result.success:
            self._raise_update_error(result)
        return result

    # -- internals ---------------------------------------------------------

    def _base_cmd(self) -> list[str]:
        cmd = [str(self.cli_path)]
        if self.ts_path:
            cmd += ["--ts-path", self.ts_path]
        cmd += ["-v"] * self.verbose
        return cmd

    @staticmethod
    def _cache_flag_args(
        backup_cache: bool,
        restore_cache: bool,
        cache_backup_policy: str | None,
    ) -> list[str]:
        """Build the direct-mode cache-phase argv fragment.

        ``--backup-cache``/``--restore-cache``/``--cache-backup-policy=...``
        are position-free presence flags; the CLI runs them once before any
        subcommand in the fixed order backup -> restore -> op.  A non-None
        ``cache_backup_policy`` implies ``--backup-cache`` — even when
        ``backup_cache=False`` is passed explicitly (the policy is
        meaningless without it); an invalid policy raises ``ValueError``
        before any subprocess is spawned.

        The Python-side default policy is ``abort`` on EVERY path (direct
        and serve): ``--cache-backup-policy=abort`` is emitted whenever
        ``backup_cache`` is set without an explicit policy, because the
        bare flag's own default ("prompt") degrades to keep-existing on a
        piped subprocess.

        Cache-phase payloads are not attached to the operation result: a
        combined-mode op result's ``.cache`` is ``None``.  Only standalone
        :meth:`backup_cache`/:meth:`restore_cache`/:meth:`cache_maintenance`
        results carry ``.cache``.
        """
        policy: str | None = None
        if cache_backup_policy is not None:
            backup_cache = True
            policy = _validate_cache_policy(cache_backup_policy)
        args: list[str] = []
        if backup_cache:
            args.append("--backup-cache")
            # Always pin the policy: the Python-side default is "abort" on
            # every path (see docstring), so the bare flag's CLI default of
            # "prompt" must never leak through.
            args.append(
                f"--cache-backup-policy={policy if policy is not None else 'abort'}"
            )
        if restore_cache:
            args.append("--restore-cache")
        return args

    def _serve_cache_phase(
        self,
        backup_cache: bool,
        restore_cache: bool,
        cache_backup_policy: str | None,
    ) -> None:
        """Send cache-phase broker requests ahead of the op request.

        Mirrors the CLI's fixed order (backup -> restore -> op): each
        pre-request runs before the op, and a failed pre-request (success
        false -> typed exception, or a broker exception) propagates so the
        op request is never sent.
        """
        if cache_backup_policy is not None:
            backup_cache = True
        if not backup_cache and not restore_cache:
            return
        if backup_cache:
            # The serve default is "abort" (fail-safe). "prompt" is
            # unavailable under serve (the broker has no console) and raises
            # ValueError in _serve_cache_policy; other policies pass through.
            policy = _validate_cache_policy(cache_backup_policy or "abort")
            self._call_broker_cache("backup-cache", _serve_cache_policy(policy))
        if restore_cache:
            self._call_broker_cache("restore-cache", None)

    # -- serve-broker routing ----------------------------------------------

    def _serve_active(self) -> bool:
        """Return ``True`` when single-item ops should route through the broker.

        The broker path is used ONLY when the Python session is NOT elevated
        AND ``use_serve`` is True.  When already elevated, the existing
        ``subprocess.run`` path is used directly (zero overhead, no extra UAC).
        Direct subprocess mode is valid only when the Python process is
        elevated; with ``use_serve=False`` in a non-elevated session,
        _run_cli/_run_batch raise ElevationRequiredError before launch.
        """
        return self._use_serve and not self._is_elevated

    def broker_process_handle(self) -> int | None:
        """Return the broker process handle, or None if no broker is running.

        For serve mode, this is the elevated tsdev_cli.exe broker's process
        handle. For direct mode, returns None. Used by tests to verify the
        broker doesn't restart between operations.
        """
        if self._broker is None:
            return None
        return self._broker._process_handle

    def _ensure_broker(self) -> _ServeBroker:
        """Lazy-start the serve broker on first use."""
        if self._broker is None:
            # §6.1 conflict, refused client-side BEFORE the spawn: the
            # broker would exit 1 before writing the pipe file and surface
            # only as a generic "exited before writing the pipe file".
            # Mirror the on_progress fail-fast convention (ValueError).
            # Scoped to EXPLICIT Full authorization (ctor kwarg True or
            # the TSDEV_RECYCLE_WHSERVER env var): the RescueOnly default
            # and an explicit ctor opt-out are never refused (an explicit
            # opt-out also beats the opposite-polarity env var, so it
            # cannot create the conflict).
            recycle_from_env = (
                os.environ.get("TSDEV_RECYCLE_WHSERVER") not in (None, "", "0")
            )
            noguard_from_env = (
                os.environ.get("TSDEV_NO_WHSERVER_VAS_GUARD") not in (None, "", "0")
            )
            if noguard_from_env and (
                self._recycle_whserver is True
                or (self._recycle_whserver is None and recycle_from_env)
            ):
                sources = []
                if self._recycle_whserver is True:
                    sources.append("TsDevClient(recycle_whserver=True)")
                if self._recycle_whserver is None and recycle_from_env:
                    sources.append("TSDEV_RECYCLE_WHSERVER env")
                raise ValueError(
                    "recycle_whserver (" + " + ".join(sources) + ") cannot "
                    "be combined with TSDEV_NO_WHSERVER_VAS_GUARD env: the "
                    "VAS guard is the recycle trigger -- the broker would "
                    "exit 1 before writing the pipe file. Unset "
                    "TSDEV_NO_WHSERVER_VAS_GUARD or drop the recycle "
                    "authorization."
                )
            if self._recycle_whserver is True or (
                self._recycle_whserver is None and recycle_from_env
            ):
                # The broker's §6.3 stderr warning is invisible here (hidden
                # spawn, stderr discarded) -- surface the client-side brief.
                # Explicit authorization only: the RescueOnly default is
                # silent (the CLI prints its own one-line NOTE to the
                # broker's discarded stderr).
                _warn_recycle_whserver()
            self._broker = _ServeBroker(self.cli_path, self.ts_path, verbose=self.verbose,
                                        recycle_whserver=self._recycle_whserver)
        return self._broker

    def _call_broker_single(self, op: str, type_: str, name: str, source: str | os.PathLike | None) -> TsDevResult:
        """Route a single-item op (insert/modify/delete/upsert) through the broker.

        The source is read client-side (from a file path or a source string)
        and sent inline in the request JSON as ``"source"``.
        """
        request: dict = {"op": op, "type": type_, "name": name}
        if source is not None:
            request["source"] = self._read_source(source)
        broker = self._ensure_broker()
        obj = broker.call(request)
        result = TsDevResult.from_json(obj)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def _call_broker_exists(self, type_: str, name: str, case_insensitive: bool) -> TsDevResult:
        """Route an ``exists`` query through the broker."""
        request: dict = {"op": "exists", "type": type_, "name": name}
        if case_insensitive:
            request["case_insensitive"] = True
        broker = self._ensure_broker()
        obj = broker.call(request)
        result = TsDevResult.from_json(obj)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def _call_broker_get_source(self, type_: str, name: str, output_file: str | os.PathLike | None) -> TsDevResult:
        """Route a ``get-source`` request through the broker.

        When ``output_file`` is provided, it is forwarded as
        ``"output_path"`` so the (elevated) broker writes the source to that
        file directly.
        """
        request: dict = {"op": "get-source", "type": type_, "name": name}
        if output_file is not None:
            request["output_path"] = str(output_file)
        broker = self._ensure_broker()
        obj = broker.call(request)
        result = TsDevResult.from_json(obj)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def _call_broker_cache(self, op: str, policy: str | None) -> TsDevResult:
        """Route a ``backup-cache``/``restore-cache`` request through the broker.

        ``policy`` is the serve-mode existing-backup policy for
        ``backup-cache`` (``"prompt"`` must already be rejected via
        :func:`_serve_cache_policy`) and is omitted for ``restore-cache``.
        """
        request: dict = {"op": op}
        if policy is not None:
            request["policy"] = policy
        broker = self._ensure_broker()
        obj = broker.call(request)
        result = TsDevResult.from_json(obj)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def _read_source(self, source: str | os.PathLike) -> str:
        """Read EL source from a file path or pass through a source string.

        Mirrors :meth:`_materialize_source`'s path-vs-string detection, but
        returns the source text directly (for inline transmission over the
        pipe) instead of writing it to a temp file.
        """
        p = Path(source)
        if p.is_file():
            return p.read_text(encoding="utf-8", errors="replace")
        return source if isinstance(source, str) else str(source)

    def _single(
        self,
        op: str,
        type_: str,
        name: str,
        source: str | os.PathLike | None,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
    ) -> TsDevResult:
        t = _validate_type(type_)
        # Serve-broker routing: when not elevated and use_serve is active,
        # route single-item ops through the elevated broker instead of
        # spawning a fresh child per op.  Otherwise _run_cli requires an
        # elevated process for direct subprocess mode.
        if self._serve_active():
            self._serve_cache_phase(backup_cache, restore_cache, cache_backup_policy)
            return self._call_broker_single(op, t, name, source)
        sub = _SINGLE_SUBCMD[op]
        cmd = (
            self._base_cmd()
            + ["--json"]
            + self._cache_flag_args(backup_cache, restore_cache, cache_backup_policy)
            + [sub, t, name]
        )
        cleanup: list[Path] = []
        try:
            if source is not None:
                src_path = self._materialize_source(source, cleanup)
                cmd.append(str(src_path))
            result = self._run_cli(cmd)
        finally:
            self._cleanup(cleanup)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def _recycle_whserver_requested(self, kwarg: bool = False) -> bool:
        """EXPLICIT Full ``--recycle-whserver`` authorization.

        Pinned precedence (design tmp/RECYCLE_ENGINE_DESIGN.md
        Section 6.1/4.3 + the tri-state addendum): the per-call kwarg, the
        ``TsDevClient(recycle_whserver=...)`` ctor kwarg, and the
        ``TSDEV_RECYCLE_WHSERVER`` env var (set to any value other than
        ``"0"``).  An explicit ctor opt-out (``False``) beats the
        opposite-polarity env var, so it is never requested; the
        RescueOnly default (ctor ``None``, no env) is never requested
        either -- the default's below-floor rescue needs no per-call
        authorization.
        """
        if kwarg or self._recycle_whserver is True:
            return True
        if self._recycle_whserver is False:
            return False
        return os.environ.get("TSDEV_RECYCLE_WHSERVER") not in (None, "", "0")

    def _recycle_flag_args(self, kwarg: bool = False) -> list[str]:
        """Tri-state recycle flag emission for a direct batch command line.

        An explicit ctor opt-out (``False``) forwards
        ``--no-recycle-whserver``: the flag makes the opt-out explicit and
        beats the opposite-polarity ``TSDEV_RECYCLE_WHSERVER`` env var
        (the CLI's pinned precedence).  The literal
        ``--recycle-whserver`` goes out only for the explicit arms
        (per-call kwarg / ctor kwarg) -- env-only authorization rides the
        inherited environment block so the CLI's ``authorizedViaEnv``
        attribution stays truthful.  The RescueOnly default forwards
        nothing.
        """
        if self._recycle_whserver is False:
            return ["--no-recycle-whserver"]
        if kwarg or self._recycle_whserver is True:
            return ["--recycle-whserver"]
        return []

    def _check_recycle_polarity_conflict(self, kwarg: bool) -> None:
        """Refuse a per-call ``recycle_whserver=True`` on a client whose
        ctor kwarg EXPLICITLY opted out (``False``): the emitted command
        line would carry both polarity flags, which the CLI refuses with
        a usage error -- fail here, before any spawn."""
        if kwarg and self._recycle_whserver is False:
            raise ValueError(
                "recycle_whserver=True cannot be combined with the "
                "explicit TsDevClient(recycle_whserver=False) opt-out: "
                "the CLI refuses both polarity flags"
            )

    def _check_recycle_whserver_noguard(self, kwarg: bool = False) -> None:
        """§6.1 recycle+``--no-whserver-vas-guard`` conflict, refused
        client-side BEFORE spawn — the direct-path mirror of the
        ``_ensure_broker`` check.  The CLI's own exit-1 refusal is the
        backstop; refusing here keeps the client-side §6.3 warning
        honest (it must not fire for an invocation the CLI will refuse —
        the C++ side emits it post-validation, ``tsdev_cli.cpp``
        ~:6680-6687)."""
        if os.environ.get("TSDEV_NO_WHSERVER_VAS_GUARD") in (None, "", "0"):
            return
        sources = []
        if kwarg:
            sources.append("recycle_whserver=True")
        if self._recycle_whserver is True:
            sources.append("TsDevClient(recycle_whserver=True)")
        if self._recycle_whserver is None and (
            os.environ.get("TSDEV_RECYCLE_WHSERVER") not in (None, "", "0")
        ):
            sources.append("TSDEV_RECYCLE_WHSERVER env")
        if not sources:
            return
        raise ValueError(
            "recycle_whserver (" + " + ".join(sources) + ") cannot "
            "be combined with TSDEV_NO_WHSERVER_VAS_GUARD env: the "
            "VAS guard is the recycle trigger -- the child exits 1 "
            "before dispatching any item. Unset "
            "TSDEV_NO_WHSERVER_VAS_GUARD or drop the recycle "
            "authorization."
        )

    def _warn_serve_recycle_env_after_launch(self, broker: _ServeBroker) -> None:
        """Fail-loud when ``TSDEV_RECYCLE_WHSERVER`` is set NOW but the
        running broker was launched without recycle authorization.

        Broker authorization is fixed on the launch command line and
        ``runas`` does not inherit the caller's environment block, so an
        env var set after the broker started can never reach it — the
        batch would run unrecycled and silently so.  Warn (do not fail):
        the batch still ran is the honest outcome.  An explicit ctor
        opt-out is exempt: the forwarded ``--no-recycle-whserver`` beats
        the opposite-polarity env by the CLI's pinned precedence, so the
        later env var is shadowed by design, not lost."""
        if (
            getattr(broker, "_started", False)
            and not getattr(broker, "_recycle_authorized_at_launch", True)
            and not getattr(broker, "_recycle_explicit_optout", False)
            and os.environ.get("TSDEV_RECYCLE_WHSERVER") not in (None, "", "0")
        ):
            warnings.warn(
                "TSDEV_RECYCLE_WHSERVER is set but the running serve "
                "broker was launched WITHOUT --recycle-whserver: broker "
                "recycle authorization is fixed at broker launch (runas "
                "does not inherit the caller's environment), so this "
                "batch runs in the RescueOnly default (floor rescue "
                "only) and the later-set Full authorization never "
                "reaches the broker. Set the env "
                "var or TsDevClient(recycle_whserver=True) before the "
                "broker starts, or call client._broker.shutdown() -- the "
                "next operation auto-restarts the broker (_ServeBroker."
                "call() re-runs start() when not _started).",
                UserWarning,
                stacklevel=4,
            )

    def _batch(
        self,
        op: str,
        items: Sequence[BatchItem],
        on_progress: BatchProgressCallback | None = None,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
        recycle_whserver: bool = False,
    ) -> list[TsDevResult]:
        # Manifest pre-validation failures (BatchItem.prevalid_failure —
        # the C++ BatchItem::preValid=false shape, produced by the console
        # manifest reader): each becomes a per-item failure result in
        # position instead of a batch-aborting error, matching the C++
        # ordering (the item is never dispatched; the failure result is
        # what parseManifestLines pre-populated). Dispatchable items run
        # as a contiguous sub-batch and the results merge back in input
        # order. Pre-validated items do not invoke on_progress (the
        # callback contract is attempted items only) and the sub-batch's
        # index/total counts dispatchable items.
        prevalid: dict[int, TsDevResult] = {}
        dispatchable: list[BatchItem] = []
        for i, it in enumerate(items):
            if it.prevalid_failure is not None:
                ec, msg = it.prevalid_failure
                prevalid[i] = TsDevResult(
                    op=op, type=it.type, name=it.name, success=False,
                    exit_code=ec,
                    error=TsDevErrorInfo(
                        code=_PREVALID_ERROR_CODE.get(ec, "unknown_error"),
                        message=msg))
            else:
                dispatchable.append(it)
        if prevalid:
            if not dispatchable:
                # Every item failed manifest pre-validation: there is no
                # sub-batch to dispatch — spawning the CLI with an empty
                # stdin manifest would exit 1 ("manifest from stdin is
                # empty") and raise, losing the per-item failure results.
                # Return them in position, matching the serve path and
                # the C++ front-end (which reports the parseManifestLines
                # preValid=false results without a run).
                return [prevalid[i] for i in range(len(items))]
            sub = self._batch(
                op, dispatchable, on_progress,
                backup_cache=backup_cache, restore_cache=restore_cache,
                cache_backup_policy=cache_backup_policy,
                recycle_whserver=recycle_whserver)
            merged: list[TsDevResult] = []
            sub_iter = iter(sub)
            for i in range(len(items)):
                merged.append(
                    prevalid[i] if i in prevalid else next(sub_iter))
            return merged
        if self._serve_active():
            # The per-call recycle_whserver kwarg is silently inert on the
            # serve path: broker authorization is fixed at broker launch
            # (the flag lives on the broker's launch command line). Refuse
            # rather than drop it — the ctor kwarg and
            # TSDEV_RECYCLE_WHSERVER env are the serve-mode levers.
            # (recycle + on_progress IS allowed in serve mode — design
            # §4.3 tagged-progress resume; only this kwarg is refused.)
            self._check_recycle_polarity_conflict(recycle_whserver)
            if recycle_whserver:
                raise ValueError(
                    "per-call recycle_whserver=True is not supported on the "
                    "serve path: broker recycle authorization is fixed at "
                    "broker launch -- set TsDevClient(recycle_whserver=True) "
                    "or TSDEV_RECYCLE_WHSERVER before the broker starts"
                )
            return self._batch_serve(op, items, on_progress,
                                     backup_cache=backup_cache,
                                     restore_cache=restore_cache,
                                     cache_backup_policy=cache_backup_policy)
        self._check_recycle_polarity_conflict(recycle_whserver)
        recycle = self._recycle_whserver_requested(recycle_whserver)
        if recycle and on_progress is not None:
            # §6.1: the on_progress path runs --batch-progress-control whose
            # stdin control channel belongs to the DYING process — a
            # mid-batch recycle exit would strand it. Refuse before spawn;
            # the CLI's usage error is the backstop.
            raise ValueError(
                "recycle_whserver cannot be combined with on_progress "
                "(the batch progress-control channel belongs to the dying process)"
            )
        if recycle:
            # Post-validation ordering (mirrors the C++ §6.3 emission at
            # tsdev_cli.cpp ~:6680-6687): refuse the noguard conflict
            # BEFORE warning, and warn only when a child will actually be
            # spawned — a non-elevated use_serve=False client raises
            # ElevationRequiredError inside _run_batch before any child
            # exists, so it must not see a warning about a recycle that
            # cannot run.
            self._check_recycle_whserver_noguard(recycle_whserver)
            if self._is_elevated or self._use_serve:
                # The CLI's §6.3 warning goes to captured child stderr
                # (not surfaced by default) — emit the client-side brief.
                _warn_recycle_whserver()
        # Tri-state flag emission.  An explicit ctor opt-out (False)
        # forwards --no-recycle-whserver: on the direct path the flag
        # makes the opt-out explicit and beats the opposite-polarity
        # TSDEV_RECYCLE_WHSERVER env (the CLI's pinned precedence).
        # The literal --recycle-whserver goes out ONLY for the explicit
        # arms (per-call kwarg / ctor kwarg): env-only authorization
        # reaches the direct child through the inherited environment
        # block (subprocess.run passes no env=) -- appending the flag
        # would flip the CLI's authorizedViaEnv attribution off, so a
        # refusal would misname the env var's source as an explicit
        # --recycle-whserver.  The RescueOnly default forwards nothing.
        recycle_args = self._recycle_flag_args(recycle_whserver)
        needs_source = op in ("insert", "modify", "upsert")
        # Reset the retained-manifest record BEFORE this call's cleanup
        # runs — the controlled path never reaches _run_batch, so a stale
        # path from a previous call must not spare this call's temps.
        self._retained_resume_manifest = None
        cleanup: list[Path] = []
        try:
            manifest = self._build_manifest_string(items, needs_source, cleanup)
            sub = _BATCH_SUBCMD[op]
            cache_args = self._cache_flag_args(
                backup_cache, restore_cache, cache_backup_policy
            )
            # --json selects NDJSON progress on stderr AND the stdout
            # results array (batch stdout is empty without it), so
            # captured stderr is machine-readable.
            if on_progress is None:
                cmd = (self._base_cmd() + ["--json"] + cache_args
                       + recycle_args + [sub, "--batch-stdin"])
                results = self._run_batch(cmd, input=manifest)
            else:
                # --batch-progress-control: the manifest goes to stdin as a
                # framed TSDEV_BATCH_CONTROL/1 block and stdin stays open
                # for per-item continue/cancel decisions. recycle_args is
                # [] or the explicit --no-recycle-whserver opt-out here
                # (explicit authorization + on_progress is refused above).
                cmd = self._base_cmd() + ["--json"] + cache_args + recycle_args + [
                    sub, "--batch-stdin", "--batch-progress-control"
                ]
                results = self._run_batch_controlled(
                    cmd, manifest.encode("utf-8"), op, on_progress
                )
        finally:
            # A resume manifest retained by _run_batch (the resumed run
            # raised, the recycle bound was hit, or exit 19) references
            # the temp .el sources this call materialized — deleting
            # them would make the retained manifest unusable for manual
            # resubmission.  Carve the manifest-referenced paths out of
            # cleanup; an UNREADABLE manifest spares every temp source
            # (fail-safe: the referenced set cannot be enumerated).
            retained = getattr(self, "_retained_resume_manifest", None)
            if retained is _RETAIN_ALL_TEMP_SOURCES:
                # A manifest was written but its path could not be
                # recovered — the referenced set cannot be enumerated,
                # so every temp source is spared (fail-safe).
                cleanup = []
            elif retained is not None:
                keep = _manifest_source_paths(retained)
                cleanup = (
                    [p for p in cleanup if str(p) not in keep]
                    if keep is not None
                    else []
                )
            self._cleanup(cleanup)
        return results

    def _batch_serve(
        self,
        op: str,
        items: Sequence[BatchItem],
        on_progress: BatchProgressCallback | None = None,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
        max_recycles: int = _MAX_RECYCLES,
    ) -> list[TsDevResult]:
        """Route a batch op through the serve broker by looping over items.

        The broker's TsDevLib is long-lived (flushPerItem=false), so this is
        as efficient as the C++ batch path — no per-op process spawn, no
        per-op warehouse flush.  Per-item failures are collected into the
        returned list (NOT raised), matching non-serve batch semantics.
        Broker-level failures (crash, broken pipe) propagate as exceptions.

        Every request is tagged with ``batch`` metadata (one ``uuid4().hex``
        id per call, 1-based index, total); each response's ``progress``
        member is validated before result parsing.  ``on_progress`` is
        invoked after each actual result; ``False`` cancels before the next
        item and the unattempted remainder becomes exit-12 ``cancelled``
        markers (no callback, no broker call, no source read for markers).

        Recycle boundary frames (design §4.3): an exit-18
        (``whserver_recycled``) response means the broker recycled
        WHServer and exited — the broker is respawned and the batch
        resubmits from the refused index (the refused item is retried,
        not marked not-run), bounded by ``max_recycles`` per call; when
        the bound is hit the boundary result stands and the remainder
        becomes bound-exhaustion markers.  An exit-19
        (``whserver_recycle_refused``) response aborts the batch — the
        refusal result stands, the remainder becomes refusal markers,
        and NO respawn is attempted.  A respawn failure (declined/ignored
        UAC, handshake timeout) propagates and ends the batch.
        """
        needs_source = op in ("insert", "modify", "upsert")
        # Pre-validate all items upfront (matches _build_manifest_string).
        validated: list[tuple[str, BatchItem]] = []
        for it in items:
            t = _validate_type(it.type)
            if needs_source and it.source is None:
                raise ValueError(
                    f"batch {it.type}/{it.name}: source is required for {op}"
                )
            validated.append((t, it))
        # Cache-phase pre-requests run once before the batch, in the CLI's
        # fixed order; a failed pre-request propagates and no item runs.
        # This precedes even the empty-batch return so the ordering matches
        # the direct path (where the cache phase runs before any batch
        # processing, including an empty manifest).
        self._serve_cache_phase(backup_cache, restore_cache, cache_backup_policy)
        if not validated:
            return []
        broker = self._ensure_broker()
        # Launch-time-fixed env check: TSDEV_RECYCLE_WHSERVER set after
        # the broker was launched without authorization is silently
        # inert — warn so the batch does not run unrecycled unnoticed
        # (fail-loud, not fail-silent).
        self._warn_serve_recycle_env_after_launch(broker)
        batch_id = uuid.uuid4().hex
        total = len(validated)
        results: list[TsDevResult] = []
        recycles_used = 0
        i = 0
        while i < len(validated):
            t, it = validated[i]
            index = i + 1
            request: dict = {
                "op": op,
                "type": t,
                "name": it.name,
                "batch": {"id": batch_id, "index": index, "total": total},
            }
            if needs_source:
                request["source"] = self._read_source(it.source)
            obj = broker.call(request)
            _validate_serve_progress(obj, batch_id, index, total)
            result = TsDevResult.from_json(obj)
            if result.exit_code == _WHSERVER_RECYCLED_EXIT_CODE:
                # The broker recycled WHServer, wrote this boundary frame
                # and exited — the refused item was NEVER dispatched.
                # Resubmit from THIS index on a respawned broker (the
                # retried item's fresh result replaces the boundary
                # frame), bounded by max_recycles per batch call (design
                # §4.3).  Each respawn is another runas launch — one UAC
                # prompt for a non-elevated client; a declined/ignored
                # prompt propagates from start() and kills the batch
                # (fail-closed, never a hang).
                if recycles_used >= max_recycles:
                    results.append(result)
                    results.extend(
                        _cancelled_result(
                            op, t2, it2.name,
                            _recycle_bound_marker_message(max_recycles))
                        for t2, it2 in validated[index:]
                    )
                    break
                recycles_used += 1
                # WAIT-ONLY reap: the exit-18 broker is mid-graceful-
                # teardown releasing its WHServer m_nLock session refs —
                # killing it here would orphan them and deadlock the
                # respawned broker's census (see the method docstring).
                broker._reset_pipe_for_recycle_respawn()
                broker.start()
                continue
            results.append(result)
            if result.exit_code == _WHSERVER_RECYCLE_REFUSED_EXIT_CODE:
                # Recycle authorized but the gate refused — the broker
                # exited.  STOP without respawning: a refused server is
                # still breached, so a respawn-per-item loop would pay a
                # spawn + census + refusal for every remaining item.
                # The refused item keeps its exit-19 result; the
                # remainder gets not-run markers.
                results.extend(
                    _cancelled_result(op, t2, it2.name,
                                      _RECYCLE_REFUSED_MARKER_MESSAGE)
                    for t2, it2 in validated[index:]
                )
                break
            if (
                result.exit_code == _VAS_ABORT_EXIT_CODE
                and result.error is not None
                and result.error.code == _VAS_ABORT_ERROR_CODE
            ):
                # The broker's WHServer VAS-headroom guard refused to
                # dispatch this item. Record the refusal as this item's
                # result, then mark the unattempted remainder as not-run
                # (exit-12 markers carrying the abort message) and STOP —
                # no further requests are sent, no callback invoked for the
                # refused item (it was never attempted, same as the C++
                # batch markers).
                results.extend(
                    _cancelled_result(op, t2, it2.name,
                                      _VAS_ABORT_MARKER_MESSAGE)
                    for t2, it2 in validated[index:]
                )
                break
            if _invoke_progress(on_progress, index, total, result):
                results.extend(
                    _cancelled_result(op, t2, it2.name)
                    for t2, it2 in validated[index:]
                )
                break
            i += 1
        return results

    def _batch_dir(
        self,
        op: str,
        type_: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None,
        on_progress: BatchProgressCallback | None = None,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
        recycle_whserver: bool = False,
    ) -> list[TsDevResult]:
        if self._serve_active():
            # Same drop-guard as _batch: the per-call kwarg cannot reach an
            # already-defined broker command line — authorization is fixed
            # at broker launch (ctor kwarg / env are the serve-mode levers).
            self._check_recycle_polarity_conflict(recycle_whserver)
            if recycle_whserver:
                raise ValueError(
                    "per-call recycle_whserver=True is not supported on the "
                    "serve path: broker recycle authorization is fixed at "
                    "broker launch -- set TsDevClient(recycle_whserver=True) "
                    "or TSDEV_RECYCLE_WHSERVER before the broker starts"
                )
            return self._batch_dir_serve(op, type_, dir_path, extensions, on_progress,
                                         backup_cache=backup_cache,
                                         restore_cache=restore_cache,
                                         cache_backup_policy=cache_backup_policy)
        self._check_recycle_polarity_conflict(recycle_whserver)
        recycle = self._recycle_whserver_requested(recycle_whserver)
        if recycle and on_progress is not None:
            # Same §6.1 restriction as _batch: on_progress implies
            # --batch-progress-control, refused before spawn.
            raise ValueError(
                "recycle_whserver cannot be combined with on_progress "
                "(the batch progress-control channel belongs to the dying process)"
            )
        if recycle:
            # Same post-validation ordering as _batch (noguard refusal
            # before warning; no warning when the elevation guard will
            # raise before any child exists).
            self._check_recycle_whserver_noguard(recycle_whserver)
            if self._is_elevated or self._use_serve:
                _warn_recycle_whserver()
        t = _validate_type(type_)
        exts = list(extensions) if extensions else []
        for e in exts:
            if not isinstance(e, str) or not e:
                raise ValueError(f"invalid extension {e!r}: must be a non-empty string")
        # --json: see _batch — keeps captured stderr progress machine-readable.
        # Same tri-state emission rule as _batch (see _recycle_flag_args):
        # the explicit ctor opt-out forwards --no-recycle-whserver, the
        # explicit arms forward --recycle-whserver, env-only authorization
        # rides the inherited env block, the default forwards nothing.
        cmd = (
            self._base_cmd()
            + ["--json"]
            + self._cache_flag_args(backup_cache, restore_cache, cache_backup_policy)
            + self._recycle_flag_args(recycle_whserver)
            + [f"batch-{op}-dir", t, str(dir_path)]
        )
        for e in exts:
            cmd += ["-t", e]
        if on_progress is None:
            return self._run_batch(cmd, input="")
        # Native directory enumeration; stdin carries decision lines only.
        cmd.append("--batch-progress-control")
        return self._run_batch_controlled(cmd, None, op, on_progress)

    def _batch_dir_serve(
        self,
        op: str,
        type_: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None,
        on_progress: BatchProgressCallback | None = None,
        *,
        backup_cache: bool = False,
        restore_cache: bool = False,
        cache_backup_policy: str | None = None,
        max_recycles: int = _MAX_RECYCLES,
    ) -> list[TsDevResult]:
        """Route a directory batch op through the serve broker.

        Replicates the C++ CLI's ``parseDirectory`` logic client-side:
        enumerates regular files (no recursion), filters by extension
        (default ``.el`` + ``.txt``; dot-prefix optional; case-insensitive;
        note: empty-string extensions are rejected, diverging from C++ which
        uses them to match extensionless files — this matches the non-serve
        ``_batch_dir`` path's existing validation),
        uses the file stem as the item name, and reads file content as EL
        source for insert/modify/upsert.  Items are sorted by name for
        deterministic ordering (matching the C++ path).

        Non-existent directory and empty/no-matching-files both raise
        :class:`TsDevError` (matching the C++ exit-1 behavior).  Per-item
        source-read failures are collected as failure results with
        ``source_file_error`` (exit 3), matching the C++ path.  Per-item
        operation failures are collected (NOT raised).  Broker-level
        failures (crash, broken pipe) propagate as exceptions.

        Requests are tagged and responses validated exactly as in
        :meth:`_batch_serve`; the 1-based position is the sorted entry-list
        position (a local read failure still consumes an index).  Sources
        are read lazily per entry so cancellation and callback exceptions
        never touch files the batch never reached.  ``False`` from
        ``on_progress`` cancels before the next entry: the remainder become
        exit-12 ``cancelled`` markers (no callback, no broker call, no
        source read for markers).

        Recycle boundary frames are handled exactly as in
        :meth:`_batch_serve` (exit 18 = respawn + resubmit from the
        refused index bounded by ``max_recycles``; exit 19 = abort with
        refusal markers, no respawn).
        """
        t = _validate_type(type_)
        # Normalize extensions (matches C++ parseDirectory).
        if extensions:
            exts: list[str] = []
            for e in extensions:
                if not isinstance(e, str) or not e:
                    raise ValueError(
                        f"invalid extension {e!r}: must be a non-empty string"
                    )
                ext = e.lower()
                if ext and not ext.startswith("."):
                    ext = "." + ext
                exts.append(ext)
        else:
            exts = [".el", ".txt"]

        # Cache-phase pre-requests run once before the batch (fixed order,
        # failure suppresses the op — same rule as _batch_serve).  This
        # precedes directory validation/enumeration so the ordering matches
        # the direct path (where the CLI runs the cache phase before opening
        # the directory).
        self._serve_cache_phase(backup_cache, restore_cache, cache_backup_policy)

        dir_p = Path(dir_path)
        if not dir_p.is_dir():
            raise TsDevError(
                message=f'failed to open directory "{dir_path}"'
            )

        needs_source = op in ("insert", "modify", "upsert")
        # (name, file_path) — source is read lazily inside the loop.
        entries: list[tuple[str, Path]] = []
        for entry in dir_p.iterdir():
            if not entry.is_file():
                continue
            ext = entry.suffix.lower()
            if ext not in exts:
                continue
            entries.append((entry.stem, entry))

        if not entries:
            ext_desc = ", ".join(exts)
            raise TsDevError(
                message=f"no files matching {ext_desc} found in {dir_path}"
            )

        # Sort by name for deterministic ordering (matches C++ parseDirectory).
        entries.sort(key=lambda x: x[0])

        broker = self._ensure_broker()
        # Same launch-time-fixed env check as _batch_serve.
        self._warn_serve_recycle_env_after_launch(broker)
        batch_id = uuid.uuid4().hex
        total = len(entries)
        results: list[TsDevResult] = []
        recycles_used = 0
        i = 0
        while i < len(entries):
            name, file_path = entries[i]
            index = i + 1
            if needs_source:
                try:
                    source = file_path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    # A local read failure is an actual completed result at
                    # this position: it invokes the callback and can cancel.
                    result = TsDevResult(
                        op=op,
                        type=t,
                        name=name,
                        success=False,
                        exit_code=3,
                        error=TsDevErrorInfo(
                            code="source_file_error",
                            message=f'failed to read source file "{file_path}"',
                        ),
                    )
                    results.append(result)
                    if _invoke_progress(on_progress, index, total, result):
                        results.extend(
                            _cancelled_result(op, t, n)
                            for n, _p in entries[index:]
                        )
                        break
                    i += 1
                    continue
            else:
                source = None
            request: dict = {
                "op": op,
                "type": t,
                "name": name,
                "batch": {"id": batch_id, "index": index, "total": total},
            }
            if needs_source:
                request["source"] = source
            obj = broker.call(request)
            _validate_serve_progress(obj, batch_id, index, total)
            result = TsDevResult.from_json(obj)
            if result.exit_code == _WHSERVER_RECYCLED_EXIT_CODE:
                # Same bounded respawn+resubmit rule as _batch_serve: the
                # broker recycled WHServer and exited — resubmit from THIS
                # index on a respawned broker, bounded by max_recycles.
                if recycles_used >= max_recycles:
                    results.append(result)
                    results.extend(
                        _cancelled_result(
                            op, t, n,
                            _recycle_bound_marker_message(max_recycles))
                        for n, _p in entries[index:]
                    )
                    break
                recycles_used += 1
                # WAIT-ONLY reap, same as _batch_serve: the exit-18
                # broker is mid-graceful-teardown releasing its WHServer
                # m_nLock session refs — a kill would orphan them and
                # deadlock the respawned broker's census.
                broker._reset_pipe_for_recycle_respawn()
                broker.start()
                continue
            results.append(result)
            if result.exit_code == _WHSERVER_RECYCLE_REFUSED_EXIT_CODE:
                # Same abort rule as _batch_serve: gate refused, broker
                # exited — refusal result stands, remainder gets not-run
                # markers, NO respawn.
                results.extend(
                    _cancelled_result(op, t, n,
                                      _RECYCLE_REFUSED_MARKER_MESSAGE)
                    for n, _p in entries[index:]
                )
                break
            if (
                result.exit_code == _VAS_ABORT_EXIT_CODE
                and result.error is not None
                and result.error.code == _VAS_ABORT_ERROR_CODE
            ):
                # Same broker VAS-guard abort rule as _batch_serve.
                results.extend(
                    _cancelled_result(op, t, n, _VAS_ABORT_MARKER_MESSAGE)
                    for n, _p in entries[index:]
                )
                break
            if _invoke_progress(on_progress, index, total, result):
                results.extend(
                    _cancelled_result(op, t, n) for n, _p in entries[index:]
                )
                break
            i += 1
        return results

    def _materialize_source(self, source: str | os.PathLike, cleanup: list[Path]) -> Path:
        """Return a path to a source file.

        If ``source`` is an existing file, it is used directly.  Otherwise it
        is treated as EasyLanguage source text and written to a temporary
        ``.el`` file (scheduled for cleanup).
        """
        p = Path(source)
        if p.is_file():
            return p
        # Treat as source string.  Write in binary mode to match the CLI's
        # binary read (avoids any newline translation surprises).
        text = source if isinstance(source, str) else str(source)
        fd, tmp = tempfile.mkstemp(suffix=".el", prefix="tsdev_src_")
        f = os.fdopen(fd, "wb")
        try:
            f.write(text.encode("utf-8"))
        finally:
            f.close()
        path = Path(tmp)
        cleanup.append(path)
        return path

    def _build_manifest_string(self, items: Sequence[BatchItem], needs_source: bool, cleanup: list[Path]) -> str:
        """Build the batch manifest as an in-memory string.

        String sources are still materialized to temporary ``.el`` files (via
        :meth:`_materialize_source`) and their paths referenced in the manifest
        lines; only the manifest itself is kept in memory (piped to the CLI via
        stdin) instead of being written to a ``.manifest`` file.

        The line format is byte-identical to what the previous file-writing
        implementation produced: one ``<type> <name> [<source_file>]`` line per
        item, joined with ``\\n`` and terminated with a trailing ``\\n`` when
        non-empty.  ``<name>`` goes through :func:`_manifest_name_token` —
        the quoted form the CLI's manifest parser accepts — so a spaced
        name (the TradeStation norm) re-parses to the SAME component; an
        unquoted spaced name would silently retarget a delete.
        """
        lines: list[str] = []
        for it in items:
            t = _validate_type(it.type)
            name_tok = _manifest_name_token(it.name)
            if needs_source:
                if it.source is None:
                    raise ValueError(f"batch {it.type}/{it.name}: source is required for insert/modify")
                src_path = self._materialize_source(it.source, cleanup)
                # source_file is the rest of the line after the 2nd token, so
                # a path with spaces is fine.
                lines.append(f"{t} {name_tok} {src_path}")
            else:
                lines.append(f"{t} {name_tok}")
        text = "\n".join(lines)
        if lines:
            text += "\n"
        return text

    def _run_cli(self, cmd: list[str]) -> TsDevResult:
        # Direct tsdev_cli subprocess mode requires an elevated administrator
        # process.  When use_serve=False and the Python session is NOT
        # elevated, raise ElevationRequiredError BEFORE attempting the
        # subprocess call.  A direct tsdev_cli invocation in that state
        # returns a not-elevated error; the Python guard provides the
        # client-side exception instead.  The authoritative elevation
        # contract (direct-CLI / serve broker / use_serve / exit code) lives
        # in AGENTS.md and docs/BUILD.md; see those instead of restating the
        # exit-code detail here.  When the session IS elevated this guard is
        # a no-op.  When use_serve=True and not elevated, this method is
        # never reached (the serve-broker path is taken instead).
        if not self._use_serve and not self._is_elevated:
            raise ElevationRequiredError()
        if not self.cli_path.is_file():
            raise FileNotFoundError(f"tsdev_cli.exe not found at {self.cli_path!s}")
        try:
            cp = subprocess.run(
                cmd,
                capture_output=True,
                timeout=self.timeout,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
        except subprocess.TimeoutExpired as e:
            raise TimeoutError(f"tsdev_cli timed out after {self.timeout}s") from e
        # Check for crash exit codes (VEH catches fatal exceptions and exits
        # with the NT status code as the process exit code).
        is_crash, crash_type = _is_crash_exit(cp.returncode)
        if is_crash:
            raise TsDevCrashError(
                message=f"tsdev_cli.exe crashed ({crash_type}, exit code 0x{cp.returncode & 0xFFFFFFFF:08X}). "
                "This indicates a bug in the orlib20 integration.",
                exit_code=cp.returncode,
            )
        stdout = cp.stdout or ""
        stderr = cp.stderr or ""
        if cp.returncode == _WATCHDOG_ABORT_EXIT_CODE:
            # Watchdog abort emits no JSON frame — the item's outcome is
            # uncertain; the WATCHDOG ABORT line is on the child's stderr.
            raise TsDevWatchdogAbortError(
                message=(
                    "tsdev_cli aborted via dispatch watchdog (exit 15). "
                    f"stderr={stderr!r}"
                ),
                exit_code=cp.returncode,
            )
        try:
            obj = json.loads(stdout)
        except json.JSONDecodeError:
            raise TsDevError(
                message=(
                    f"tsdev_cli returned non-JSON output (exit {cp.returncode}). "
                    f"stdout={stdout!r} stderr={stderr!r}"
                )
            )
        return TsDevResult.from_json(obj, stdout=stdout, stderr=stderr)

    def _run_batch(
        self,
        cmd: list[str],
        input: str,
        *,
        max_recycles: int = _MAX_RECYCLES,
    ) -> list[TsDevResult]:
        """Run one direct batch; on exit 18 resubmit the CLI's resume
        manifest to fresh ``batch-<op>`` invocations (design §4.1/§4.2).

        An exit-18 run's results array already carries the boundary:
        attempted results followed by exit-12 not-run markers.  The
        markers pin the resume point — the attempted head is kept and the
        marker tail is replaced by the resumed batch's results, so no
        item is ever dropped or duplicated.  Bounded by ``max_recycles``;
        bound exhaustion (or a missing/unusable manifest reference)
        returns the boundary array with the markers re-worded to state
        WHY the tail was never re-attempted (the generic "resubmit
        remaining items" text would advise an action that cannot
        happen).  Exit 19 returns the array with its refusal-marker
        tail re-worded to name the retained manifest path (or the
        stderr diagnostic when the refused event names none).  The
        resume manifest is
        caller-owned: deleted once the resumed batch has RETURNED —
        terminal outcomes included; when the resumed run raises instead
        the file stays on disk for manual resubmission/forensics and
        the completed-so-far results ride the raised exception as
        ``batch_partial_results`` so the attempted head is not lost.
        """
        completed: list[TsDevResult] = []
        pending_manifest: str | None = None
        recycles_used = 0
        # Path of a resume manifest left on disk for the caller — set on
        # every path that retains one (raise, bound exhaustion, exit 19)
        # so the caller's temp-source cleanup spares the source files it
        # still references.  The _RETAIN_ALL_TEMP_SOURCES sentinel marks
        # a manifest that was written but whose path could not be
        # recovered (non-ASCII path dropped from the JSON event) — it
        # still references this batch's temps, so all are spared.
        self._retained_resume_manifest = None
        while True:
            try:
                rc, results, stderr = self._run_batch_once(cmd, input)
            except BaseException as e:
                # A raise leaves pending_manifest on disk for manual
                # resubmission — record it for the caller's cleanup.
                # The attempted head is NOT lost with the raise: the
                # completed-so-far results ride the exception so the
                # caller can recover items 1..N outcomes.
                self._retained_resume_manifest = pending_manifest
                e.batch_partial_results = completed
                raise
            # The just-returned child consumed the previous resume
            # manifest — delete it now (the resumed batch HAS returned,
            # terminal outcomes included).  When _run_batch_once RAISES
            # this statement is skipped and the file is retained for
            # manual resubmission/forensics — deliberately no finally.
            if pending_manifest is not None:
                _unlink_quiet(pending_manifest)
                pending_manifest = None
            if rc != _WHSERVER_RECYCLED_EXIT_CODE:
                # Exit 19 (recycle refused): the CLI appended refusal
                # markers AND may have written a resume manifest — its
                # path rides the whserver_recycle_refused event but is
                # never surfaced unless the marker names it.  Re-word
                # the marker tail to carry the retained path.
                if rc == _WHSERVER_RECYCLE_REFUSED_EXIT_CODE:
                    refused_path = _resume_manifest_path(stderr)
                    if refused_path is not None:
                        self._retained_resume_manifest = refused_path
                        results = [
                            _rewrite_marker_message(
                                r,
                                _RECYCLE_REFUSED_MARKER_MESSAGE
                                + _manifest_path_suffix(refused_path),
                            )
                            for r in results
                        ]
                    else:
                        # No manifest path on the refused event — name
                        # WHY via the stderr diagnostic (a writer
                        # refusal names the refusing item and states
                        # that no manifest exists; a non-ASCII path was
                        # dropped from the event). When the event says
                        # a manifest WAS written but its path is
                        # unusable, the file still references this
                        # batch's temp sources — spare them all.
                        diag = _resume_manifest_diagnostic(stderr)
                        message = _RECYCLE_REFUSED_MARKER_MESSAGE + (
                            f" (resume manifest unavailable -- {diag})"
                            if diag else "")
                        results = [
                            _rewrite_marker_message(r, message)
                            for r in results
                        ]
                        if _resume_manifest_path_dropped(stderr):
                            self._retained_resume_manifest = (
                                _RETAIN_ALL_TEMP_SOURCES)
                completed.extend(results)
                return completed
            # Exit 18: the FIRST exit-12 result pins the boundary —
            # keep the attempted head, resubmit the not-run tail.
            # INVARIANT: exit code 12 is marker-exclusive — no real
            # dispatch produces it (attempted items return their own
            # codes), so the first 12 cannot be a genuine item result
            # and everything before it is a real attempt (failures
            # included).  The whserver_recycled event's index member is
            # informational only: this marker scan is the authoritative
            # boundary (it tracks the array the caller actually sees).
            boundary = next(
                (i for i, r in enumerate(results) if r.exit_code == 12),
                len(results),
            )
            completed.extend(results[:boundary])
            markers = results[boundary:]
            manifest_path = _resume_manifest_path(stderr)
            resume_cmd = (
                _resume_batch_cmd(cmd, manifest_path)
                if manifest_path is not None
                else None
            )
            if resume_cmd is None or recycles_used >= max_recycles:
                # No resumable manifest or the bound is hit: the
                # boundary array stands, but the marker text must say
                # WHY the tail was never re-attempted — a missing
                # manifest has nothing to resubmit, and bound
                # exhaustion deliberately stops.  A bound-exhausted
                # run's own manifest is retained on disk (its tail was
                # never consumed): the marker names its path — the
                # event only reaches the child's stderr — and the path
                # is recorded so temp-source cleanup spares the files
                # it references.
                if recycles_used >= max_recycles:
                    message = _recycle_bound_marker_message(max_recycles)
                    if manifest_path is not None:
                        message += _manifest_path_suffix(manifest_path)
                        self._retained_resume_manifest = manifest_path
                else:
                    # No resumable manifest. Distinguish the causes via
                    # the stderr diagnostic: a writer refusal means NO
                    # file exists; a written manifest whose non-ASCII
                    # path was dropped from the event still exists on
                    # disk and references this batch's temp sources —
                    # spare them all so manual resubmission works.
                    message = _RESUME_MANIFEST_UNAVAILABLE_MARKER_MESSAGE
                    diag = _resume_manifest_diagnostic(stderr)
                    if diag is not None:
                        message += f" -- {diag}"
                    if _resume_manifest_path_dropped(stderr):
                        self._retained_resume_manifest = (
                            _RETAIN_ALL_TEMP_SOURCES)
                completed.extend(
                    _rewrite_marker_message(r, message) for r in markers
                )
                return completed
            recycles_used += 1
            pending_manifest = manifest_path
            cmd = resume_cmd
            input = ""  # manifest-file form: no stdin manifest

    def _run_batch_once(
        self, cmd: list[str], input: str
    ) -> tuple[int, list[TsDevResult], str]:
        """One direct ``tsdev_cli`` batch invocation.

        Returns ``(returncode, results, stderr)`` so the caller can apply
        the exit-18 resume contract; every pre-array failure raises
        exactly as ``_run_batch`` did historically.
        """
        # Elevation guard: direct tsdev_cli subprocess mode requires an
        # elevated administrator process.  When use_serve=False and the
        # session is not elevated, raise ElevationRequiredError BEFORE
        # attempting the subprocess call; a direct CLI invocation in that
        # state returns a not-elevated error.  The authoritative elevation
        # contract (direct-CLI / serve broker / use_serve / exit code) lives
        # in AGENTS.md and docs/BUILD.md; see those instead of restating the
        # exit-code detail here.  When use_serve=True and not elevated, batch
        # ops are routed through the broker in _batch_serve/_batch_dir_serve
        # before reaching here.
        if not self._use_serve and not self._is_elevated:
            raise ElevationRequiredError()
        if not self.cli_path.is_file():
            raise FileNotFoundError(f"tsdev_cli.exe not found at {self.cli_path!s}")
        try:
            cp = subprocess.run(
                cmd,
                capture_output=True,
                timeout=self.timeout,
                text=True,
                encoding="utf-8",
                errors="replace",
                input=input,
            )
        except subprocess.TimeoutExpired as e:
            raise TimeoutError(f"tsdev_cli timed out after {self.timeout}s") from e
        # Check for crash exit codes (VEH catches fatal exceptions and exits
        # with the NT status code as the process exit code).
        is_crash, crash_type = _is_crash_exit(cp.returncode)
        if is_crash:
            raise TsDevCrashError(
                message=f"tsdev_cli.exe crashed ({crash_type}, exit code 0x{cp.returncode & 0xFFFFFFFF:08X}). "
                "This indicates a bug in the orlib20 integration.",
                exit_code=cp.returncode,
            )
        stdout = cp.stdout or ""
        stderr = cp.stderr or ""
        if cp.returncode == _WATCHDOG_ABORT_EXIT_CODE:
            # Watchdog abort mid-batch: no final JSON array for the
            # in-flight/remaining items; completed items' results are lost
            # with the process. The WATCHDOG ABORT line is on stderr.
            raise TsDevWatchdogAbortError(
                message=(
                    "tsdev_cli batch aborted via dispatch watchdog "
                    f"(exit 15). stderr={stderr!r}"
                ),
                exit_code=cp.returncode,
            )
        # Exit 1 => manifest/usage error: the batch did not run.
        if cp.returncode == 1:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch failed (exit 1, manifest/usage error). "
                    f"stdout={stdout!r} stderr={stderr!r}"
                )
            )
        # A cache-phase failure (exit 5/16/17) preempts the batch: stdout is
        # a single error object, not a results array.
        _raise_cache_phase_error(stdout, stderr, cp.returncode)
        try:
            arr = json.loads(stdout)
        except json.JSONDecodeError:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch returned non-JSON output (exit {cp.returncode}). "
                    f"stdout={stdout!r} stderr={stderr!r}"
                )
            )
        if not isinstance(arr, list):
            raise TsDevError(message=f"tsdev_cli batch output is not a JSON array: {stdout!r}")
        results = [TsDevResult.from_json(obj, stdout=stdout, stderr=stderr) for obj in arr]
        return cp.returncode, results, stderr

    def _run_batch_controlled(
        self,
        cmd: list[str],
        manifest_bytes: bytes | None,
        op: str,
        on_progress: BatchProgressCallback,
    ) -> list[TsDevResult]:
        """Drive a direct ``tsdev_cli`` batch over the
        ``--batch-progress-control`` channel.

        The CLI runs on binary ``Popen`` pipes with ``--json`` +
        ``--batch-progress-control``.  For manifest batches
        ``manifest_bytes`` is written first as a ``TSDEV_BATCH_CONTROL/1``
        framed block and stdin stays open for per-item decisions; for
        directory batches stdin carries decision lines only.  stderr NDJSON
        events drive synchronous ``on_progress`` callbacks on this (caller)
        thread; after each non-final ``batch_item_completed`` a
        ``continue``/``cancel`` decision line is written back.  The final
        stdout JSON array is authoritative — close-failure rewrites and
        exit-12 cancellation markers included.
        """
        # Same elevation preflight as _run_cli/_run_batch — before Popen.
        if not self._use_serve and not self._is_elevated:
            raise ElevationRequiredError()
        if not self.cli_path.is_file():
            raise FileNotFoundError(f"tsdev_cli.exe not found at {self.cli_path!s}")

        deadline = (
            None if self.timeout is None else time.monotonic() + self.timeout
        )

        def _remaining() -> float | None:
            return (
                None
                if deadline is None
                else max(0.0, deadline - time.monotonic())
            )

        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        # Drain both child pipes concurrently so a large final array (or a
        # chatty stderr) can never deadlock the run; callbacks still execute
        # synchronously on this caller thread.
        out_buf = bytearray()
        line_q: queue.Queue = queue.Queue()

        def _drain_stdout() -> None:
            try:
                while True:
                    chunk = proc.stdout.read(65536)
                    if not chunk:
                        return
                    out_buf.extend(chunk)
            except Exception:
                return

        def _drain_stderr() -> None:
            try:
                while True:
                    line = proc.stderr.readline()
                    if not line:
                        return
                    line_q.put(line)
            except Exception:
                return
            finally:
                line_q.put(None)  # EOF sentinel

        threads = (
            threading.Thread(target=_drain_stdout, daemon=True),
            threading.Thread(target=_drain_stderr, daemon=True),
        )
        for t in threads:
            t.start()

        def _write_stdin(data: bytes) -> None:
            try:
                proc.stdin.write(data)
                proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                # A dead channel defers to the final exit/crash
                # classification below.
                pass

        def _close_pipes() -> None:
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                try:
                    if stream is not None:
                        stream.close()
                except (BrokenPipeError, OSError, ValueError):
                    pass

        def _reap_after_timeout() -> None:
            try:
                proc.terminate()
            except Exception:
                pass
            try:
                proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
                try:
                    proc.wait(timeout=5)
                except Exception:
                    pass
            for t in threads:
                t.join(timeout=5)
            _close_pipes()

        # Manifest batches open the control channel with the framed
        # TSDEV_BATCH_CONTROL/1 header + exact byte payload; stdin then
        # stays open for decisions. Directory batches skip the frame —
        # stdin carries decision lines only.
        if manifest_bytes is not None:
            _write_stdin(
                b"TSDEV_BATCH_CONTROL/1 "
                + str(len(manifest_bytes)).encode("ascii")
                + b"\n"
                + manifest_bytes
            )

        expected_index = 1
        stable_total: int | None = None
        open_started: tuple[str, str] | None = None  # (type, name) in flight
        summary_seen = False
        first_error: str | None = None
        callback_exc: BaseException | None = None
        stop_events = False
        stderr_parts: list[bytes] = []

        def _int_field(obj: dict, key: str) -> int | None:
            v = obj.get(key)
            if isinstance(v, bool) or not isinstance(v, int):
                return None
            return v

        def _cancel_if_pending(event_obj) -> None:
            # The child awaits a decision exactly after emitting a
            # batch_item_completed for a non-final item position. The
            # position is deterministic (expected_index), even when the
            # event's own fields are malformed; fall back to the event's
            # claimed total when the stable total is not yet known.
            tot = stable_total
            if tot is None and isinstance(event_obj, dict):
                tot = _int_field(event_obj, "total")
            if tot is not None and expected_index < tot:
                _write_stdin(b"cancel\n")

        def _note_error(msg: str, event_obj=None) -> None:
            nonlocal first_error
            if first_error is None:
                first_error = msg
            _cancel_if_pending(event_obj)

        def _handle_started(obj: dict) -> None:
            nonlocal stable_total, open_started
            idx = _int_field(obj, "index")
            tot = _int_field(obj, "total")
            typ, nam = obj.get("type"), obj.get("name")
            if (
                obj.get("batch_operation") != op
                or idx is None
                or idx != expected_index
                or tot is None
                or tot < 1
                or not isinstance(typ, str)
                or not isinstance(nam, str)
            ):
                _note_error(f"malformed batch_item_started event: {obj!r}", obj)
                return
            if stable_total is None:
                stable_total = tot
            elif tot != stable_total:
                _note_error(
                    f"batch_item_started total changed: {obj!r}", obj
                )
                return
            if open_started is not None:
                _note_error(
                    "batch_item_started before the previous item completed",
                    obj,
                )
                return
            open_started = (typ, nam)

        def _handle_completed(obj: dict) -> None:
            nonlocal expected_index, open_started
            idx = _int_field(obj, "index")
            tot = _int_field(obj, "total")
            if (
                open_started is None
                or obj.get("batch_operation") != op
                or idx is None
                or idx != expected_index
                or tot is None
                or tot != stable_total
                or obj.get("type") != open_started[0]
                or obj.get("name") != open_started[1]
            ):
                _note_error(
                    f"malformed batch_item_completed event: {obj!r}", obj
                )
                return
            open_started = None
            result_obj = obj.get("result")
            if not isinstance(result_obj, dict):
                _note_error(
                    "batch_item_completed event has no result object", obj
                )
                return
            # The nested result must agree with the event envelope.
            if (
                result_obj.get("op") != op
                or result_obj.get("type") != obj["type"]
                or result_obj.get("name") != obj["name"]
                or ("success" in obj
                    and obj["success"] != result_obj.get("success"))
                or ("exit_code" in obj
                    and obj["exit_code"] != result_obj.get("exit_code"))
            ):
                _note_error(
                    "batch_item_completed event disagrees with its result "
                    "object",
                    obj,
                )
                return
            try:
                result = TsDevResult.from_json(result_obj)
            except Exception as e:
                _note_error(
                    f"batch_item_completed result could not be parsed: {e!r}",
                    obj,
                )
                return
            # The callback runs synchronously on this caller thread; only a
            # `False` return before the final item cancels. A callback
            # exception propagates to the loop, which still sends cancel
            # and drains the child to a safe close before re-raising.
            cancel = _invoke_progress(on_progress, idx, stable_total, result)
            expected_index = idx + 1
            if idx < stable_total:
                _write_stdin(b"cancel\n" if cancel else b"continue\n")

        def _handle_summary(obj: dict) -> None:
            nonlocal summary_seen
            if summary_seen:
                _note_error("duplicate batch_completed event", obj)
                return
            summary_seen = True
            comp = _int_field(obj, "completed")
            tot = _int_field(obj, "total")
            not_run = _int_field(obj, "not_run")
            succ = _int_field(obj, "successes")
            fail = _int_field(obj, "failures")
            if (
                obj.get("batch_operation") != op
                or tot is None
                or tot != stable_total
                or comp is None
                or comp != expected_index - 1
                or open_started is not None
                or not_run is None
                or comp + not_run != tot
                or succ is None
                or fail is None
                or succ + fail != comp
                or not isinstance(obj.get("cancelled"), bool)
                or not isinstance(obj.get("close_failed"), bool)
                or _int_field(obj, "exit_code") is None
            ):
                _note_error(
                    f"malformed batch_completed summary: {obj!r}", obj
                )

        timed_out = False
        while True:
            try:
                line = line_q.get(timeout=_remaining())
            except queue.Empty:
                timed_out = True
                break
            if line is None:
                break  # stderr EOF
            stderr_parts.append(line)
            if stop_events:
                continue
            try:
                obj = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
                continue  # non-JSON line -> retained diagnostic
            if not isinstance(obj, dict):
                continue  # JSON scalar/array -> retained diagnostic
            ev = obj.get("event")
            if ev == "batch_item_started":
                _handle_started(obj)
            elif ev == "batch_item_completed":
                try:
                    _handle_completed(obj)
                except BaseException as e:
                    # Progress-callback exception: send cancel when a
                    # decision may be pending, keep draining so the CLI
                    # closes the warehouse and finalizes, then re-raise
                    # the original exception unchanged.
                    callback_exc = e
                    _cancel_if_pending(obj)
            elif ev == "batch_completed":
                _handle_summary(obj)
            # batch_control_error and unrecognized JSON objects are
            # diagnostics, retained in stderr_parts.
            if first_error is not None or callback_exc is not None:
                stop_events = True

        if timed_out:
            _reap_after_timeout()
            raise TimeoutError(f"tsdev_cli timed out after {self.timeout}s")

        try:
            proc.wait(timeout=_remaining())
        except subprocess.TimeoutExpired:
            _reap_after_timeout()
            raise TimeoutError(f"tsdev_cli timed out after {self.timeout}s")
        for t in threads:
            t.join(timeout=10)
        _close_pipes()

        rc = proc.returncode
        is_crash, crash_type = _is_crash_exit(rc)
        if is_crash:
            raise TsDevCrashError(
                message=f"tsdev_cli.exe crashed ({crash_type}, exit code "
                        f"0x{rc & 0xFFFFFFFF:08X}). "
                        "This indicates a bug in the orlib20 integration.",
                exit_code=rc,
            )
        if rc == _WATCHDOG_ABORT_EXIT_CODE:
            # Watchdog abort mid-batch: no JSON frame and no
            # batch_completed summary — the in-flight item's outcome is
            # uncertain; the WATCHDOG ABORT line is on stderr.
            stderr_text = b"".join(stderr_parts).decode("utf-8", "replace")
            raise TsDevWatchdogAbortError(
                message=(
                    "tsdev_cli batch aborted via dispatch watchdog "
                    f"(exit 15). stderr={stderr_text!r}"
                ),
                exit_code=rc,
            )
        if callback_exc is not None:
            raise callback_exc
        stdout_text = bytes(out_buf).decode("utf-8", "replace")
        stderr_text = b"".join(stderr_parts).decode("utf-8", "replace")
        if rc == 1:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch failed (exit 1, manifest/usage error). "
                    f"stdout={stdout_text!r} stderr={stderr_text!r}"
                )
            )
        # Same cache-phase preemption as _run_batch: a single error object
        # on stdout (exit 5/16/17); no batch_completed summary is emitted.
        _raise_cache_phase_error(stdout_text, stderr_text, rc)
        if first_error is not None:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch progress protocol violation "
                    f"(exit {rc}): {first_error}. stderr={stderr_text!r}"
                )
            )
        if not summary_seen:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch ended without a batch_completed "
                    f"summary (exit {rc}). stderr={stderr_text!r}"
                )
            )
        if rc not in (0, 2, 3):
            raise TsDevError(
                message=(
                    f"tsdev_cli batch returned unexpected exit {rc}. "
                    f"stdout={stdout_text!r} stderr={stderr_text!r}"
                )
            )
        try:
            arr = json.loads(stdout_text)
        except json.JSONDecodeError:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch returned non-JSON output (exit {rc}). "
                    f"stdout={stdout_text!r} stderr={stderr_text!r}"
                )
            )
        if not isinstance(arr, list):
            raise TsDevError(
                message=f"tsdev_cli batch output is not a JSON array: {stdout_text!r}"
            )
        return [
            TsDevResult.from_json(obj, stdout=stdout_text, stderr=stderr_text)
            for obj in arr
        ]

    @staticmethod
    def _cleanup(paths: list[Path]) -> None:
        for p in paths:
            try:
                p.unlink(missing_ok=True)
            except OSError:
                pass
