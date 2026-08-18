# SPDX-License-Identifier: MIT
"""tsdev_automation -- Python wrapper driving the ``tsdev_cli`` executable.

This package provides a high-level, stdlib-only Python API
(:class:`TsDevClient`) for the three TSDev Library operations -- insert,
modify, delete -- plus batch variants, against the TradeStation Development
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
    CompileError,
    EllMissingError,
    EntryOpError,
    InitFailure,
    NotFoundError,
    SaveSourceError,
    SourceFileError,
    ThreadFailure,
    TsDevError,
    UnknownTypeError,
    UsageError,
)
from .results import TsDevBatchResult, TsDevErrorInfo, TsDevResult

__all__ = [
    "TsDevClient",
    "BatchItem",
    "TsDevResult",
    "TsDevErrorInfo",
    "TsDevBatchResult",
    "TsDevError",
    "UsageError",
    "UnknownTypeError",
    "SourceFileError",
    "InitFailure",
    "NotFoundError",
    "AlreadyExistsError",
    "EntryOpError",
    "SaveSourceError",
    "CompileError",
    "EllMissingError",
    "ThreadFailure",
    "main",
]


def main() -> None:
    """Console-script entry point (delegates to :mod:`tsdev_automation._cli`)."""
    from ._cli import _main as _real_main

    _real_main()
