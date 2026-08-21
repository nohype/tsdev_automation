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
    ell_missing         -> EllMissingError
    thread_failure      -> ThreadFailure
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
    """Warehouse entry operation failure (exit 6)."""


class SaveSourceError(TsDevError):
    """saveSource failure (exit 7)."""


class CompileError(TsDevError):
    """EasyLanguage compilation failure (exit 8).

    Inspect ``self.result.error.compile_errors`` for the raw compiler text and
    ``self.result.prog_id`` for the (cleaned-up) ProgId.
    """


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
    "ell_missing": EllMissingError,
    "thread_failure": ThreadFailure,
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
    "EllMissingError",
    "ThreadFailure",
    "TsDevCrashError",
    "ERROR_CODE_MAP",
    "exception_for",
]
