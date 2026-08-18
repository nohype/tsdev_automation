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

import json
import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from .exceptions import TsDevError, exception_for
from .results import TsDevResult

# The four component types confirmed in tsdev_cli.cpp (parseType).
_VALID_TYPES: frozenset[str] = frozenset({"strategy", "function", "indicator", "tradingapp"})

# Subcommand name per high-level operation.
_SINGLE_SUBCMD: dict[str, str] = {"insert": "insert", "modify": "modify", "delete": "delete"}
_BATCH_SUBCMD: dict[str, str] = {"insert": "batch-insert", "modify": "batch-modify", "delete": "batch-delete"}


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

    Priority: explicit argument > ``TSDEV_CLI_PATH`` env var > repo-root
    ``build/Release/tsdev_cli.exe`` (discovered by walking up from this file
    until a ``CMakeLists.txt`` is found).
    """
    if cli_path is not None:
        return Path(cli_path)
    env = os.environ.get("TSDEV_CLI_PATH")
    if env:
        return Path(env)
    # Walk up from the package directory looking for CMakeLists.txt (repo root).
    here = Path(__file__).resolve().parent
    for parent in [here, *here.parents]:
        if (parent / "CMakeLists.txt").is_file():
            return parent / "build" / "Release" / "tsdev_cli.exe"
    # Fallback: relative to this file (src/tsdev_automation -> repo root).
    return here.parents[1] / "build" / "Release" / "tsdev_cli.exe"


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
    """

    def __init__(
        self,
        cli_path: str | os.PathLike | None = None,
        ts_path: str | None = None,
        timeout: float | None = None,
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

    def delete(self, type: str, name: str) -> TsDevResult:
        """Delete an existing component."""
        return self._single("delete", type, name, None)

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

    # -- internals ---------------------------------------------------------

    def _base_cmd(self) -> list[str]:
        cmd = [str(self.cli_path)]
        if self.ts_path:
            cmd += ["--ts-path", self.ts_path]
        return cmd

    def _single(self, op: str, type_: str, name: str, source: str | os.PathLike | None) -> TsDevResult:
        t = _validate_type(type_)
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
        needs_source = op in ("insert", "modify")
        cleanup: list[Path] = []
        try:
            manifest = self._write_manifest(items, needs_source, cleanup)
            sub = _BATCH_SUBCMD[op]
            cmd = self._base_cmd() + [sub, str(manifest)]
            results = self._run_batch(cmd)
        finally:
            self._cleanup(cleanup)
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

    def _write_manifest(self, items: Sequence[BatchItem], needs_source: bool, cleanup: list[Path]) -> Path:
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
        fd, tmp = tempfile.mkstemp(suffix=".manifest", prefix="tsdev_batch_")
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
            if lines:
                f.write("\n")
        path = Path(tmp)
        cleanup.append(path)
        return path

    def _run_cli(self, cmd: list[str]) -> TsDevResult:
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

    def _run_batch(self, cmd: list[str]) -> list[TsDevResult]:
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
