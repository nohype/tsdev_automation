# SPDX-License-Identifier: MIT
"""Exception hierarchy for :class:`tsdev_automation.TsDevClient`.

Each exception maps 1:1 to a ``tsdev_cli`` ``error.code`` value and carries the
originating :class:`~tsdev_automation.results.TsDevResult` so callers can
inspect ``prog_id``, ``compile_errors``, ``existing_prog_id``, etc.

Mapping (see ``src/tsdev_cli.cpp`` ``errorCodeString``):

    usage_error         -> UsageError
    unknown_type        -> UnknownTypeError
    source_file_error   -> SourceFileError
    output_write_error  -> OutputWriteError
    init_failure        -> InitFailure
    not_found           -> NotFoundError
    already_exists      -> AlreadyExistsError
    entry_op_failure    -> EntryOpError
    save_source_failure -> SaveSourceError
    compile_error       -> CompileError
    artifact_missing    -> ArtifactMissingError
    ell_missing         -> EllMissingError (legacy compatibility)
    thread_failure      -> ThreadFailure
    whserver_dispatch_watchdog_abort -> TsDevWatchdogAbortError
                                  (exit 15; no JSON frame is emitted on a
                                  watchdog abort — mapping is for symmetry
                                  only)
"""

from __future__ import annotations

from .results import TsDevResult


class TsDevError(Exception):
    """Base exception for all CLI-reported failures.

    Attributes
    ----------
    result:
        The :class:`~tsdev_automation.results.TsDevResult` describing the
        failed operation (never ``None`` for mapped subclasses).
    """

    def __init__(self, result: TsDevResult | None = None, message: str | None = None) -> None:
        self.result = result
        super().__init__(message or (result.error.message if result and result.error else "tsdev_cli operation failed"))


class UsageError(TsDevError):
    """CLI usage error (exit 1)."""


class UnknownTypeError(TsDevError):
    """Unknown component type (exit 2)."""


class SourceFileError(TsDevError):
    """Source file could not be read (exit 3)."""


class OutputWriteError(TsDevError):
    """Failed to write retrieved source to the output file (exit 3)."""


class InitFailure(TsDevError):
    """orlib20 initialization failure (exit 4)."""


class NotFoundError(TsDevError):
    """Component not found for modify/delete (exit 5)."""


class AlreadyExistsError(TsDevError):
    """Component already exists for insert (exit 5)."""


class EntryOpError(TsDevError):
    """Warehouse entry operation failure (code: entry_op_failure)."""


class SaveSourceError(TsDevError):
    """saveSource failure (exit 7)."""


class CompileError(TsDevError):
    """EasyLanguage compilation failure (exit 8).

    Inspect ``self.result.error.compile_errors`` for the raw compiler text and
    ``self.result.prog_id`` for the (cleaned-up) ProgId.
    """


class ArtifactMissingError(TsDevError):
    """Persistence artifact missing or inconsistent after compile (exit 9)."""


class EllMissingError(TsDevError):
    """.ell file not produced after compile (exit 9)."""


class ThreadFailure(TsDevError):
    """Worker-thread creation failure (exit 10)."""


class TsDevCrashError(TsDevError):
    """tsdev_cli.exe crashed (access violation or other fatal exception).

    This indicates a bug in the orlib20 integration, not a user error.
    The VEH (Vectored Exception Handler) in the CLI catches the crash and
    terminates the process cleanly with the NT status code as the exit code.
    No crash dialog (TSCrashReport.exe) is spawned.
    """

    def __init__(self, message: str | None = None, exit_code: int | None = None) -> None:
        self.exit_code = exit_code
        if message is None:
            if exit_code is not None:
                message = f"tsdev_cli.exe crashed (exit code 0x{exit_code & 0xFFFFFFFF:08X})"
            else:
                message = "tsdev_cli.exe crashed unexpectedly"
        super().__init__(message=message)


class TsDevWatchdogAbortError(TsDevError):
    """``tsdev_cli`` was terminated by its dispatch watchdog (exit 15).

    The watchdog fires ``TerminateProcess(15)`` when a warehouse dispatch
    exceeds ``--dispatch-timeout-secs`` or WHServer.exe crashed/vanished
    mid-flight (see ``src/dispatch_watchdog.cpp``).  The abort writes a
    ``tsdev_cli: WATCHDOG ABORT: ... [reason: ...]; exiting with code 15``
    line to the *CLI's own stderr* and produces NO JSON frame — the
    in-flight item's outcome is uncertain and no ``TsDevResult`` exists for
    it.  On the serve-broker path the broker's stderr is not captured by
    the client, so the exit code is the only machine-readable signal; in
    direct mode the line is visible on the child's stderr.

    Never retry the aborted request into the same server: the wedged
    WHServer state that provoked the abort persists.

    Mapped under ``error.code == "whserver_dispatch_watchdog_abort"`` for
    symmetry with the C++ ``errorCodeString`` table, even though a watchdog
    abort never emits a JSON frame carrying that code.
    """

    code = "whserver_dispatch_watchdog_abort"

    def __init__(self, result: TsDevResult | None = None, message: str | None = None, exit_code: int | None = None) -> None:
        self.exit_code = exit_code if exit_code is not None else (result.exit_code if result else None)
        if message is None:
            message = (
                "tsdev_cli was terminated by the dispatch watchdog (exit 15): "
                "a warehouse dispatch exceeded its bound or WHServer.exe "
                "crashed/vanished mid-flight. The in-flight item's outcome is "
                "uncertain; see the process's stderr for the WATCHDOG ABORT line."
            )
        super().__init__(result=result, message=message)


class ElevationRequiredError(TsDevError):
    """Non-elevated Python caller requested direct mode (client exit 12).

    Direct ``tsdev_cli`` subprocess mode requires an elevated administrator
    process; a non-elevated direct CLI invocation returns a not-elevated
    error.  The authoritative elevation contract (direct-CLI / serve broker /
    use_serve / exit code) lives in AGENTS.md and docs/BUILD.md; see those
    instead of restating the exit-code detail here.  For ``use_serve=False``,
    the Python client raises this error *before* launching the subprocess
    instead, so the caller gets a clear, actionable message.  Direct mode is
    valid when the Python process is already elevated; non-elevated callers
    must use the serve-broker path.

    This is a client-side error (no ``TsDevResult``); it is NOT produced by
    the C++ CLI and is therefore intentionally absent from
    :data:`ERROR_CODE_MAP`.  ``error.code`` is exposed as the class attribute
    :attr:`code` for symmetry with the CLI error-code strings.
    """

    code = "elevation_required"

    def __init__(self, message: str | None = None) -> None:
        self.exit_code = 12
        if message is None:
            message = (
                "Process is not elevated. Either run Python as administrator, "
                "or enable use_serve=True (the default) to use the serve-mode "
                "broker which handles elevation via a single UAC prompt."
            )
        super().__init__(message=message)


# error.code -> exception class
ERROR_CODE_MAP: dict[str, type[TsDevError]] = {
    "usage_error": UsageError,
    "unknown_type": UnknownTypeError,
    "source_file_error": SourceFileError,
    "output_write_error": OutputWriteError,
    "init_failure": InitFailure,
    "not_found": NotFoundError,
    "already_exists": AlreadyExistsError,
    "entry_op_failure": EntryOpError,
    "save_source_failure": SaveSourceError,
    "compile_error": CompileError,
    "artifact_missing": ArtifactMissingError,
    "ell_missing": EllMissingError,
    "thread_failure": ThreadFailure,
    "whserver_dispatch_watchdog_abort": TsDevWatchdogAbortError,
}


def exception_for(result: TsDevResult) -> TsDevError:
    """Return the exception class matching ``result.error.code`` (or base)."""
    assert result.error is not None
    cls = ERROR_CODE_MAP.get(result.error.code, TsDevError)
    return cls(result)


__all__ = [
    "TsDevError",
    "UsageError",
    "UnknownTypeError",
    "SourceFileError",
    "OutputWriteError",
    "InitFailure",
    "NotFoundError",
    "AlreadyExistsError",
    "EntryOpError",
    "SaveSourceError",
    "CompileError",
    "ArtifactMissingError",
    "EllMissingError",
    "ThreadFailure",
    "TsDevCrashError",
    "TsDevWatchdogAbortError",
    "ElevationRequiredError",
    "ERROR_CODE_MAP",
    "exception_for",
]
