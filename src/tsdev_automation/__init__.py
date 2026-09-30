# SPDX-License-Identifier: MIT
"""tsdev_automation -- Python wrapper driving the ``tsdev_cli`` executable.

This package provides a high-level, stdlib-only Python API
(:class:`TsDevClient`) for six single-item operations:
:meth:`TsDevClient.insert`, :meth:`TsDevClient.modify`,
:meth:`TsDevClient.delete`, :meth:`TsDevClient.exists`,
:meth:`TsDevClient.get_source`, and :meth:`TsDevClient.upsert`.
It also provides batch methods :meth:`TsDevClient.batch_insert`,
:meth:`TsDevClient.batch_modify`, :meth:`TsDevClient.batch_delete`, and
:meth:`TsDevClient.batch_upsert`, plus directory-batch methods
:meth:`TsDevClient.batch_insert_dir`, :meth:`TsDevClient.batch_modify_dir`,
:meth:`TsDevClient.batch_delete_dir`, and
:meth:`TsDevClient.batch_upsert_dir`, against the TradeStation Development
Environment warehouse.  It shells out to the 32-bit ``tsdev_cli.exe`` (which
loads ``orlib20.dll``) using its ``--json`` structured output and batch
subcommands, so it works from 64-bit Python.

Package layout
--------------
The importable package lives at ``src/tsdev_automation/`` (uv_build src-layout,
auto-discovered from ``pyproject.toml``).  The C++ sources live *directly*
under ``src/`` and are referenced by ``CMakeLists.txt`` via explicit paths
(no globs), so the Python package and the C++ build coexist without collision.
"""

from __future__ import annotations

from .client import BatchItem, TsDevClient
from .exceptions import (
    AlreadyExistsError,
    ArtifactMissingError,
    CompileError,
    ElevationRequiredError,
    EllMissingError,
    EntryOpError,
    InitFailure,
    NotFoundError,
    OutputWriteError,
    SaveSourceError,
    SourceFileError,
    ThreadFailure,
    TsDevCacheCancelledError,
    TsDevCacheError,
    TsDevCrashError,
    TsDevError,
    TsDevWatchdogAbortError,
    UnknownTypeError,
    UsageError,
)
from .results import (
    BatchProgress,
    BatchProgressCallback,
    TsDevBatchResult,
    TsDevErrorInfo,
    TsDevResult,
)

__all__ = [
    "TsDevClient",
    "BatchItem",
    "TsDevResult",
    "TsDevErrorInfo",
    "TsDevBatchResult",
    "BatchProgress",
    "BatchProgressCallback",
    "TsDevError",
    "UsageError",
    "UnknownTypeError",
    "SourceFileError",
    "InitFailure",
    "NotFoundError",
    "OutputWriteError",
    "AlreadyExistsError",
    "ArtifactMissingError",
    "EntryOpError",
    "ElevationRequiredError",
    "SaveSourceError",
    "CompileError",
    "EllMissingError",
    "ThreadFailure",
    "TsDevCrashError",
    "TsDevWatchdogAbortError",
    "TsDevCacheError",
    "TsDevCacheCancelledError",
    "main",
]


def main() -> None:
    """Console-script entry point (delegates to :mod:`tsdev_automation._cli`)."""
    from ._cli import _main as _real_main

    _real_main()
