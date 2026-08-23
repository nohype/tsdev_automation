# SPDX-License-Identifier: MIT
"""The :class:`TsDevClient` wrapper around the ``tsdev_cli`` executable.

``tsdev_cli.exe`` is a 32-bit C++ binary that loads ``orlib20.dll`` from the
TradeStation 9.5 install.  It cannot be loaded in-process from 64-bit Python,
so this wrapper drives it as a subprocess using the ``--json`` structured
output flag (single-item) and the batch subcommands (JSON array output).

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
import subprocess
import tempfile
import time
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from . import _pipe
from .exceptions import ElevationRequiredError, TsDevCrashError, TsDevError, exception_for
from .results import TsDevErrorInfo, TsDevResult

# The component types confirmed in tsdev_cli.cpp (parseType).
_VALID_TYPES: frozenset[str] = frozenset({"strategy", "function", "indicator", "tradingapp", "showme"})

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
    """

    type: str
    name: str
    source: str | os.PathLike | None = None


def _validate_type(type_: str) -> str:
    """Lower-case and validate a component type; raise ``ValueError`` if unknown."""
    t = type_.strip().lower()
    if t not in _VALID_TYPES:
        raise ValueError(
            f"unknown component type {type_!r} (expected one of {sorted(_VALID_TYPES)})"
        )
    return t


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
_SW_HIDE = 0
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

    def __init__(self, cli_path: Path, ts_path: str | None = None) -> None:
        self._cli_path = cli_path
        self._ts_path = ts_path
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
        os.close(fd)
        try:
            os.unlink(tmp_path)
        except OSError:
            pass

        # 2. ShellExecuteExW with runas.
        params = f"serve --serve-pipe-file {tmp_path}"
        if self._ts_path:
            params = f"--ts-path {self._ts_path} " + params
        sei = _SHELLEXECUTEINFOW()
        sei.cbSize = ctypes.sizeof(sei)
        sei.fMask = _SEE_MASK_NOCLOSEPROCESS
        sei.hwnd = None
        sei.lpVerb = "runas"
        sei.lpFile = str(self._cli_path)
        sei.lpParameters = params
        sei.lpDirectory = None
        sei.nShow = _SW_HIDE
        ok = _shell32.ShellExecuteExW(ctypes.byref(sei))
        if not ok:
            err = ctypes.windll.kernel32.GetLastError()
            raise PermissionError(f"UAC declined or ShellExecuteExW failed (err={err})")
        if sei.hProcess is None or int(sei.hProcess) == 0 or int(sei.hProcess) == _INVALID_HANDLE_VALUE:
            raise PermissionError("UAC declined: no process handle returned")
        self._process_handle = int(sei.hProcess)

        # 3. Poll the temp file (up to 30 s) for the pipe name, then connect.
        #    If anything below fails, terminate the orphaned broker process
        #    before re-raising so we don't leak an elevated child.
        try:
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
            # Best-effort cleanup of the handshake file.
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
            if not pipe_name:
                raise TimeoutError("serve broker did not write the pipe file within 30s")

            # 4. Connect to the pipe (retry for ~5 s — broker may not have called
            #    ConnectNamedPipe yet).
            self._pipe_handle = _pipe.connect(pipe_name, timeout_ms=5000)
        except Exception:
            # Terminate the orphaned broker process before re-raising.
            if self._process_handle:
                try:
                    _wait_for_exit(self._process_handle, 5000)
                except Exception:
                    pass
                try:
                    _kernel32.CloseHandle(self._process_handle)
                except Exception:
                    pass
                self._process_handle = None
            raise
        self._started = True
        # Safety net: ensure the broker is killed if the Python process exits
        # without an explicit shutdown().
        atexit.register(self.shutdown)

    def call(self, request: dict) -> dict:
        """Send a request and receive the response.

        If the pipe handle breaks mid-session (broker restarted), the broker
        is re-started and the request retried once — unless the broker has
        crashed (detected via its NT-status exit code), in which case a
        :class:`TsDevCrashError` is raised immediately.
        """
        if not self._started:
            self.start()
        try:
            assert self._pipe_handle is not None
            _pipe.send_request(self._pipe_handle, request)
            return _pipe.recv_response(self._pipe_handle)
        except (_pipe.PipeError, OSError):
            # Check if the broker crashed (VEH exits with the NT status code).
            if self._process_handle:
                exit_code = self._get_exit_code()
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
        # Wait for the process to exit, then terminate if needed.
        if self._process_handle is not None:
            _wait_for_exit(self._process_handle, timeout_ms=5000)
            _kernel32.CloseHandle(wintypes.HANDLE(self._process_handle))
            self._process_handle = None
        self._started = False

    def __del__(self) -> None:
        try:
            self.shutdown()
        except Exception:
            pass

    # --- internals ---------------------------------------------------------

    def _reset_pipe(self) -> None:
        # Close pipe handle.
        if self._pipe_handle is not None:
            _pipe.close(self._pipe_handle)
            self._pipe_handle = None
        # Terminate and close old broker process so start() doesn't leak it.
        if self._process_handle is not None:
            try:
                _wait_for_exit(self._process_handle, 5000)
            except Exception:
                pass
            try:
                _kernel32.CloseHandle(self._process_handle)
            except Exception:
                pass
            self._process_handle = None
        self._started = False

    def _close_handles(self) -> None:
        if self._pipe_handle is not None:
            _pipe.close(self._pipe_handle)
            self._pipe_handle = None
        if self._process_handle is not None:
            _kernel32.CloseHandle(wintypes.HANDLE(self._process_handle))
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


def _wait_for_exit(process_handle: int, timeout_ms: int) -> bool:
    """Wait for a process handle to exit.

    Returns ``True`` if the process exited within ``timeout_ms`` (or was
    already dead), ``False`` on timeout.  If the wait times out and
    ``timeout_ms > 0``, the process is terminated via ``TerminateProcess``.
    """
    WAIT_TIMEOUT = 0x00000102
    WAIT_OBJECT_0 = 0x00000000
    result = _kernel32.WaitForSingleObject(wintypes.HANDLE(process_handle), wintypes.DWORD(timeout_ms))
    if result == WAIT_OBJECT_0:
        return True
    if result == WAIT_TIMEOUT and timeout_ms > 0:
        _kernel32.TerminateProcess(wintypes.HANDLE(process_handle), wintypes.UINT(1))
        _kernel32.WaitForSingleObject(wintypes.HANDLE(process_handle), wintypes.DWORD(1000))
        return True
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
        per operation.  When already elevated, or when ``use_serve=False``
        is passed explicitly, the existing ``subprocess.run`` path is used
        directly (zero overhead, no broker, no extra UAC).  Batch ops are
        routed through the broker by looping over individual
        items — the broker's long-lived TsDevLib (flushPerItem=false) makes
        this as efficient as the C++ batch path (no per-op process spawn, no
        per-op warehouse flush).  Per-item failures are collected into the
        returned list (NOT raised), matching the non-serve batch semantics.
    """

    def __init__(
        self,
        cli_path: str | os.PathLike | None = None,
        ts_path: str | None = None,
        timeout: float | None = None,
        use_serve: bool = True,
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
        # Serve-broker integration (TVS-K.5).  When ``use_serve`` is True and
        # the Python session is NOT elevated, single-item ops are routed
        # through an elevated serve broker (one UAC prompt) instead of
        # spawning a fresh elevated child per op.  When already elevated (or
        # ``use_serve`` is False) the existing ``subprocess.run`` path is used.
        self._use_serve = use_serve
        self._is_elevated = _detect_elevation()
        self._broker: _ServeBroker | None = None  # lazy-init on first op

    # -- public API --------------------------------------------------------

    def insert(self, type: str, name: str, source: str | os.PathLike) -> TsDevResult:
        """Insert a new component and compile/verify it.

        Raises :class:`~tsdev_automation.exceptions.AlreadyExistsError` if the
        component exists, :class:`~tsdev_automation.exceptions.CompileError` on
        compilation failure, etc.
        """
        return self._single("insert", type, name, source)

    def modify(self, type: str, name: str, source: str | os.PathLike) -> TsDevResult:
        """Replace an existing component's source and recompile/verify it."""
        return self._single("modify", type, name, source)

    def upsert(self, type: str, name: str, source: str | os.PathLike) -> TsDevResult:
        """Insert a new component, or modify the existing one if it already exists.

        Materializes `source` the same way insert/modify do (string -> temp .el file;
        existing file path -> used directly). Returns TsDevResult on success.
        Raises CompileError on compile failure, SourceFileError if the source file
        cannot be read, ValueError for an unknown component type. Never raises
        NotFoundError/AlreadyExistsError (upsert branches on existence by construction).
        """
        return self._single("upsert", type, name, source)

    def delete(self, type: str, name: str) -> TsDevResult:
        """Delete an existing component."""
        return self._single("delete", type, name, None)

    def exists(self, type: str, name: str, *, case_insensitive: bool = False) -> TsDevResult:
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
        """
        t = _validate_type(type)
        # Serve-broker routing for exists.
        if self._serve_active():
            return self._call_broker_exists(t, name, case_insensitive)
        sub = _SINGLE_SUBCMD["exists"]
        cmd = self._base_cmd() + ["--json", sub, t, name]
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
        """
        t = _validate_type(type)
        # Serve-broker routing for get-source.
        if self._serve_active():
            return self._call_broker_get_source(t, name, output_file)
        sub = _SINGLE_SUBCMD["get-source"]
        cmd = self._base_cmd() + ["--json", sub, t, name]
        if output_file is not None:
            cmd += ["--output", str(output_file)]
        result = self._run_cli(cmd)
        if not result.success:
            assert result.error is not None
            raise exception_for(result)
        return result

    def batch_insert(self, items: Sequence[BatchItem]) -> list[TsDevResult]:
        """Batch-insert many items. Returns one result per item (in order).

        Per-item failures are reported in the returned list (NOT raised); only
        a manifest-level failure (exit 1) raises :class:`TsDevError`.
        """
        return self._batch("insert", items)

    def batch_modify(self, items: Sequence[BatchItem]) -> list[TsDevResult]:
        """Batch-modify many items. See :meth:`batch_insert`."""
        return self._batch("modify", items)

    def batch_delete(self, items: Sequence[BatchItem]) -> list[TsDevResult]:
        """Batch-delete many items. See :meth:`batch_insert`."""
        return self._batch("delete", items)

    def batch_upsert(self, items: Sequence[BatchItem]) -> list[TsDevResult]:
        """Batch-upsert many items (insert-or-modify). See :meth:`batch_insert`."""
        return self._batch("upsert", items)

    def batch_insert_dir(self, type: str, dir_path: str | os.PathLike, extensions: Sequence[str] | None = None) -> list[TsDevResult]:
        """Batch-insert all .el files in a directory. File stem = item name, file content = EL source.

        ``extensions`` selects which file extensions to include. ``None`` (default)
        or an empty list uses the CLI defaults (``.el`` + ``.txt``). A non-empty
        list like ``["el"]`` selects only ``.el`` files; each element is passed to
        the CLI as a ``-t <ext>`` flag. Per-item failures are reported in the
        returned list (NOT raised); a directory-level failure (exit 1) raises
        :class:`TsDevError`.
        """
        return self._batch_dir("insert", type, dir_path, extensions)

    def batch_modify_dir(self, type: str, dir_path: str | os.PathLike, extensions: Sequence[str] | None = None) -> list[TsDevResult]:
        """Batch-modify all .el files in a directory. See :meth:`batch_insert_dir`."""
        return self._batch_dir("modify", type, dir_path, extensions)

    def batch_delete_dir(self, type: str, dir_path: str | os.PathLike, extensions: Sequence[str] | None = None) -> list[TsDevResult]:
        """Batch-delete all .el files in a directory (file stem = item name; content ignored). See :meth:`batch_insert_dir`."""
        return self._batch_dir("delete", type, dir_path, extensions)

    def batch_upsert_dir(self, type: str, dir_path: str | os.PathLike, extensions: Sequence[str] | None = None) -> list[TsDevResult]:
        """Batch-upsert all .el files in a directory. See :meth:`batch_insert_dir`."""
        return self._batch_dir("upsert", type, dir_path, extensions)

    # -- internals ---------------------------------------------------------

    def _base_cmd(self) -> list[str]:
        cmd = [str(self.cli_path)]
        if self.ts_path:
            cmd += ["--ts-path", self.ts_path]
        return cmd

    # -- serve-broker routing ----------------------------------------------

    def _serve_active(self) -> bool:
        """Return ``True`` when single-item ops should route through the broker.

        The broker path is used ONLY when the Python session is NOT elevated
        AND ``use_serve`` is True.  When already elevated (the common
        elevated-session case) the existing ``subprocess.run`` path is used
        directly (zero overhead, no extra UAC).
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
            self._broker = _ServeBroker(self.cli_path, self.ts_path)
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

    def _single(self, op: str, type_: str, name: str, source: str | os.PathLike | None) -> TsDevResult:
        t = _validate_type(type_)
        # Serve-broker routing: when not elevated and use_serve is active,
        # route single-item ops through the elevated broker instead of
        # spawning a fresh child per op.
        if self._serve_active():
            return self._call_broker_single(op, t, name, source)
        sub = _SINGLE_SUBCMD[op]
        cmd = self._base_cmd() + ["--json", sub, t, name]
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

    def _batch(self, op: str, items: Sequence[BatchItem]) -> list[TsDevResult]:
        if self._serve_active():
            return self._batch_serve(op, items)
        needs_source = op in ("insert", "modify", "upsert")
        cleanup: list[Path] = []
        try:
            manifest = self._build_manifest_string(items, needs_source, cleanup)
            sub = _BATCH_SUBCMD[op]
            cmd = self._base_cmd() + [sub, "--batch-stdin"]
            results = self._run_batch(cmd, input=manifest)
        finally:
            self._cleanup(cleanup)
        return results

    def _batch_serve(self, op: str, items: Sequence[BatchItem]) -> list[TsDevResult]:
        """Route a batch op through the serve broker by looping over items.

        The broker's TsDevLib is long-lived (flushPerItem=false), so this is
        as efficient as the C++ batch path — no per-op process spawn, no
        per-op warehouse flush.  Per-item failures are collected into the
        returned list (NOT raised), matching non-serve batch semantics.
        Broker-level failures (crash, broken pipe) propagate as exceptions.
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
        if not validated:
            return []
        broker = self._ensure_broker()
        results: list[TsDevResult] = []
        for t, it in validated:
            request: dict = {"op": op, "type": t, "name": it.name}
            if needs_source:
                request["source"] = self._read_source(it.source)
            obj = broker.call(request)
            results.append(TsDevResult.from_json(obj))
        return results

    def _batch_dir(self, op: str, type_: str, dir_path: str | os.PathLike, extensions: Sequence[str] | None) -> list[TsDevResult]:
        if self._serve_active():
            return self._batch_dir_serve(op, type_, dir_path, extensions)
        t = _validate_type(type_)
        exts = list(extensions) if extensions else []
        for e in exts:
            if not isinstance(e, str) or not e:
                raise ValueError(f"invalid extension {e!r}: must be a non-empty string")
        cmd = self._base_cmd() + [f"batch-{op}-dir", t, str(dir_path)]
        for e in exts:
            cmd += ["-t", e]
        return self._run_batch(cmd, input="")

    def _batch_dir_serve(
        self,
        op: str,
        type_: str,
        dir_path: str | os.PathLike,
        extensions: Sequence[str] | None,
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

        dir_p = Path(dir_path)
        if not dir_p.is_dir():
            raise TsDevError(
                message=f'failed to open directory "{dir_path}"'
            )

        needs_source = op in ("insert", "modify", "upsert")
        # (name, file_path, source_or_None) — source is None when read failed.
        entries: list[tuple[str, str, str | None]] = []
        for entry in dir_p.iterdir():
            if not entry.is_file():
                continue
            ext = entry.suffix.lower()
            if ext not in exts:
                continue
            name = entry.stem
            source: str | None = None
            if needs_source:
                try:
                    source = entry.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    source = None  # will become a source_file_error result
            entries.append((name, str(entry), source))

        if not entries:
            ext_desc = ", ".join(exts)
            raise TsDevError(
                message=f"no files matching {ext_desc} found in {dir_path}"
            )

        # Sort by name for deterministic ordering (matches C++ parseDirectory).
        entries.sort(key=lambda x: x[0])

        broker = self._ensure_broker()
        results: list[TsDevResult] = []
        for name, file_path, source in entries:
            if needs_source and source is None:
                results.append(
                    TsDevResult(
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
                )
                continue
            request: dict = {"op": op, "type": t, "name": name}
            if needs_source:
                request["source"] = source
            obj = broker.call(request)
            results.append(TsDevResult.from_json(obj))
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
        non-empty.
        """
        lines: list[str] = []
        for it in items:
            t = _validate_type(it.type)
            if needs_source:
                if it.source is None:
                    raise ValueError(f"batch {it.type}/{it.name}: source is required for insert/modify")
                src_path = self._materialize_source(it.source, cleanup)
                # source_file is the rest of the line after the 2nd token, so
                # a path with spaces is fine.
                lines.append(f"{t} {it.name} {src_path}")
            else:
                lines.append(f"{t} {it.name}")
        text = "\n".join(lines)
        if lines:
            text += "\n"
        return text

    def _run_cli(self, cmd: list[str]) -> TsDevResult:
        # Elevation guard: when use_serve=False and the Python session is NOT
        # elevated, the CLI (which has a requireAdministrator manifest) cannot
        # be launched via subprocess.run — it fails with
        # ERROR_ELEVATION_REQUIRED (Windows error 740).  Raise a clear,
        # actionable error BEFORE attempting the subprocess call.  When the
        # session IS elevated this guard is a no-op (direct subprocess calls
        # work fine).  When use_serve=True and not elevated, this method is
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

    def _run_batch(self, cmd: list[str], input: str) -> list[TsDevResult]:
        # Elevation guard: same rationale as _run_cli — when use_serve=False
        # and the session is not elevated, the batch CLI (requireAdministrator
        # manifest) cannot be launched via subprocess.run.  Raise a clear,
        # actionable error BEFORE attempting the subprocess call.  When
        # use_serve=True and not elevated, batch ops are routed
        # through the broker in _batch_serve/_batch_dir_serve before
        # reaching here.
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
        # Exit 1 => manifest/usage error: the batch did not run.
        if cp.returncode == 1:
            raise TsDevError(
                message=(
                    f"tsdev_cli batch failed (exit 1, manifest/usage error). "
                    f"stdout={stdout!r} stderr={stderr!r}"
                )
            )
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
        return [TsDevResult.from_json(obj, stdout=stdout, stderr=stderr) for obj in arr]

    @staticmethod
    def _cleanup(paths: list[Path]) -> None:
        for p in paths:
            try:
                p.unlink(missing_ok=True)
            except OSError:
                pass
