# SPDX-License-Identifier: MIT
"""Unit + E2E tests for the :class:`tsdev_automation.TsDevClient` wrapper.

Unit tests (default, fast) mock ``subprocess.run`` so they never touch the real
``tsdev_cli.exe`` or the TradeStation warehouse.  E2E tests (marked
``@pytest.mark.e2e``) drive the real CLI and are skipped unless the CLI binary
is discoverable (``TSDEV_CLI_PATH`` env var or the default ``build/Release``
path exists).
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import inspect
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import FrozenInstanceError, dataclass
from pathlib import Path

import pytest

from tsdev_automation import (
    AlreadyExistsError,
    ArtifactMissingError,
    BatchItem,
    BatchProgress,
    CompileError,
    NotFoundError,
    TsDevClient,
    TsDevCrashError,
    TsDevError,
    TsDevResult,
    TsDevWatchdogAbortError,
)
from tsdev_automation import exceptions as exc
from tsdev_automation import _pipe
from tsdev_automation.client import (
    _ServeBroker,
    _SHELLEXECUTEINFOW,
    _build_serve_parameters,
    _detect_elevation,
    _is_crash_exit,
    _wait_for_exit,
)
from unittest.mock import MagicMock


# ---------------------------------------------------------------------------
# Helpers / fixtures
# ---------------------------------------------------------------------------


def _make_completed(stdout: str = "", stderr: str = "", returncode: int = 0) -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess(args=[], returncode=returncode, stdout=stdout, stderr=stderr)


@pytest.fixture
def fake_cli(tmp_path: Path) -> Path:
    """An empty file standing in for tsdev_cli.exe so the existence check passes."""
    p = tmp_path / "tsdev_cli.exe"
    p.write_bytes(b"")
    return p


@pytest.fixture
def client(fake_cli: Path) -> TsDevClient:
    c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
    # Force elevated so the direct-subprocess path is exercised without
    # tripping the use_serve=False / non-elevated elevation guard.  These
    # tests mock subprocess.run and are not concerned with real elevation.
    c._is_elevated = True
    return c


def _success_json(op: str, type_: str, name: str, prog_id: int = 42, ell: str = "C:/x.ell") -> str:
    return json.dumps(
        {"op": op, "type": type_, "name": name, "success": True, "exit_code": 0, "prog_id": prog_id, "ell_path": ell}
    )


def _failure_json(
    op: str, type_: str, name: str, code: str, message: str, exit_code: int = 5, **extra
) -> str:
    err = {"code": code, "message": message}
    err.update(extra)
    return json.dumps(
        {"op": op, "type": type_, "name": name, "success": False, "exit_code": exit_code, "error": err}
    )


# ---------------------------------------------------------------------------
# Unit tests (mocked subprocess)
# ---------------------------------------------------------------------------


class TestSingleItem:
    def test_insert_success(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess, "run", lambda *a, **k: _make_completed(_success_json("insert", "strategy", "MyStrat"))
        )
        r = client.insert("strategy", "MyStrat", "input(close, 1);")
        assert isinstance(r, TsDevResult)
        assert r.success is True
        assert r.op == "insert"
        assert r.type == "strategy"
        assert r.name == "MyStrat"
        assert r.prog_id == 42
        assert r.ell_path == "C:/x.ell"
        assert r.error is None

    def test_insert_already_exists(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json("insert", "strategy", "Dup", "already_exists", "exists", exit_code=5, existing_prog_id=7)
            ),
        )
        with pytest.raises(AlreadyExistsError) as ei:
            client.insert("strategy", "Dup", "x;")
        assert ei.value.result.error.existing_prog_id == 7
        assert ei.value.result.exit_code == 5

    def test_delete_not_found(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(_failure_json("delete", "function", "Nope", "not_found", "missing", exit_code=5)),
        )
        with pytest.raises(NotFoundError):
            client.delete("function", "Nope")

    def test_insert_compile_error(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json(
                    "insert", "strategy", "Bad", "compile_error", "bad", exit_code=8, prog_id=9, compile_errors="Line 1: oops"
                )
            ),
        )
        with pytest.raises(CompileError) as ei:
            client.insert("strategy", "Bad", "garbage code")
        assert ei.value.result.error.compile_errors == "Line 1: oops"
        # For compile errors the CLI nests prog_id inside error (exit 8).
        assert ei.value.result.error.prog_id == 9

    def test_insert_artifact_missing_raises_typed_error(self, client: TsDevClient, monkeypatch) -> None:
        message = (
            "persistence integrity check FAILED: artifacts are inconsistent "
            "at C:/items/missing.ell"
        )
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json("insert", "strategy", "Broken", "artifact_missing", message, exit_code=9)
            ),
        )
        with pytest.raises(ArtifactMissingError) as ei:
            client.insert("strategy", "Broken", "garbage code")
        assert ei.value.result is not None
        assert ei.value.result.error is not None
        assert ei.value.result.error.code == "artifact_missing"
        assert ei.value.result.error.message == message
        assert str(ei.value) == message

    def test_insert_ell_missing_preserves_legacy_mapping(self, client: TsDevClient, monkeypatch) -> None:
        """Historical compatibility coverage for the legacy ``.ell``-only code.

        The current exit-9 contract is ``artifact_missing`` for missing or
        inconsistent persistence artifacts; this keeps the older payload
        mapping covered without presenting it as the current CLI label.
        """
        message = "compilation succeeded but .ell file not found at C:/items/missing.ell"
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json("insert", "strategy", "Legacy", "ell_missing", message, exit_code=9)
            ),
        )
        with pytest.raises(exc.EllMissingError) as ei:
            client.insert("strategy", "Legacy", "garbage code")
        assert ei.value.result is not None
        assert ei.value.result.error is not None
        assert ei.value.result.error.code == "ell_missing"
        assert ei.value.result.error.message == message
        assert str(ei.value) == message

    def test_unknown_type_raises_valueerror(self, client: TsDevClient) -> None:
        with pytest.raises(ValueError):
            client.insert("bogus", "X", "x;")
        with pytest.raises(ValueError):
            client.delete("bogus", "X")
        with pytest.raises(ValueError):
            client.modify("bogus", "X", "x;")
        with pytest.raises(ValueError):
            client.exists("bogus", "X")
        with pytest.raises(ValueError):
            client.upsert("bogus", "X", "x;")
        with pytest.raises(ValueError):
            client.get_source("bogus", "X")

    def test_string_source_temp_file_cleaned(self, client: TsDevClient, monkeypatch, tmp_path: Path) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            # The temp source file must exist at call time.
            src = cmd[-1]
            seen["src"] = src
            assert os.path.isfile(src), "temp source file should exist when CLI runs"
            return _make_completed(_success_json("insert", "strategy", "Tmp"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.insert("strategy", "Tmp", "input(close, 1);")
        assert not os.path.isfile(seen["src"]), "temp source file must be cleaned up"

    def test_file_path_source_used_directly(self, client: TsDevClient, monkeypatch, tmp_path: Path) -> None:
        src = tmp_path / "real.el"
        src.write_text("input(close, 1);", encoding="utf-8")
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["src"] = cmd[-1]
            return _make_completed(_success_json("insert", "strategy", "Real"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.insert("strategy", "Real", str(src))
        assert os.path.normpath(seen["src"]) == os.path.normpath(str(src)), "existing file path used directly"
        assert src.exists(), "caller's file must not be deleted"

    def test_cli_not_found(self, tmp_path: Path) -> None:
        c = TsDevClient(cli_path=str(tmp_path / "does_not_exist.exe"))
        with pytest.raises(FileNotFoundError):
            c.insert("strategy", "X", "x;")

    def test_timeout(self, client: TsDevClient, monkeypatch) -> None:
        def boom(*a, **k):
            raise subprocess.TimeoutExpired(cmd=a, timeout=0.01)

        monkeypatch.setattr(subprocess, "run", boom)
        with pytest.raises(TimeoutError):
            client.insert("strategy", "X", "x;")

    def test_json_parse_failure(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed("not json{", stderr="oops"))
        with pytest.raises(TsDevError):
            client.insert("strategy", "X", "x;")

    def test_modify_success(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess, "run", lambda *a, **k: _make_completed(_success_json("modify", "indicator", "Ind"))
        )
        r = client.modify("indicator", "Ind", "plot(close);")
        assert r.success and r.op == "modify"

    def test_ts_path_passed_through(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_success_json("delete", "strategy", "X"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS", use_serve=False)
        c._is_elevated = True
        c.delete("strategy", "X")
        assert "--ts-path" in seen["cmd"]
        assert "C:/TS" in seen["cmd"]


class TestUpsert:
    def test_upsert_success(self, client: TsDevClient, monkeypatch, tmp_path: Path) -> None:
        src = tmp_path / "real.el"
        src.write_text("input(close, 1);", encoding="utf-8")
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_success_json("upsert", "strategy", "MyStrat", prog_id=1234))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = client.upsert("strategy", "MyStrat", str(src))
        assert isinstance(r, TsDevResult)
        assert r.success is True
        assert r.op == "upsert"
        assert r.prog_id == 1234
        assert seen["cmd"][-5:] == ["--json", "upsert", "strategy", "MyStrat", str(src)]
        assert src.exists(), "caller's file must not be deleted"

    def test_upsert_compile_error_raises(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json(
                    "upsert", "strategy", "Bad", "compile_error", "bad", exit_code=8, prog_id=9, compile_errors="syntax error at line 1"
                )
            ),
        )
        with pytest.raises(CompileError) as ei:
            client.upsert("strategy", "Bad", "garbage code")
        assert ei.value.result.error.compile_errors == "syntax error at line 1"
        # For compile errors the CLI nests prog_id inside error (exit 8).
        assert ei.value.result.error.prog_id == 9

    def test_upsert_source_file_error_raises(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json("upsert", "strategy", "MyStrat", "source_file_error", "failed to read source file", exit_code=3)
            ),
        )
        with pytest.raises(exc.SourceFileError):
            client.upsert("strategy", "MyStrat", "input(close, 1);")

    def test_upsert_string_source_materializes_temp_file(self, client: TsDevClient, monkeypatch, tmp_path: Path) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            # The temp source file must exist at call time.
            src = cmd[-1]
            seen["src"] = src
            assert os.path.isfile(src), "temp source file should exist when CLI runs"
            assert src.endswith(".el"), "temp source file should have .el suffix"
            return _make_completed(_success_json("upsert", "strategy", "Tmp"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.upsert("strategy", "Tmp", "input(close, 1);")
        assert not os.path.isfile(seen["src"]), "temp source file must be cleaned up"

    def test_upsert_ts_path_passed_through(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_success_json("upsert", "strategy", "MyStrat"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS", use_serve=False)
        c._is_elevated = True
        c.upsert("strategy", "MyStrat", "input(close, 1);")
        assert "--ts-path" in seen["cmd"]
        assert "C:/TS" in seen["cmd"]


def _exists_json(
    type_: str,
    name: str,
    exists: bool,
    prog_id: int | None = None,
    stored_name: str | None = None,
) -> str:
    """Fake stdout for the ``exists`` subcommand (success payload).

    ``prog_id`` is omitted from the JSON when the item is not found, matching
    the C++ CLI schema (no ``prog_id`` key on a not-found result).
    ``stored_name`` mirrors the C++ CLI ``stored_name`` field (the verbatim
    stored name when found, empty string when not found).
    """
    obj: dict = {"op": "exists", "type": type_, "name": name, "success": True, "exit_code": 0, "exists": exists}
    if prog_id is not None:
        obj["prog_id"] = prog_id
    obj["stored_name"] = stored_name if stored_name is not None else (name if exists else "")
    return json.dumps(obj)


class TestExists:
    def test_exists_found(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "MyStrat", True, prog_id=1234))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = client.exists("strategy", "MyStrat")
        assert isinstance(r, TsDevResult)
        assert r.success is True
        assert r.exists is True
        assert r.prog_id == 1234
        # The command tail must be exactly: --json exists <type> <name>
        assert seen["cmd"][-4:] == ["--json", "exists", "strategy", "MyStrat"]

    def test_exists_not_found_no_raise(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess, "run", lambda *a, **k: _make_completed(_exists_json("strategy", "Nope", False))
        )
        r = client.exists("strategy", "Nope")
        assert isinstance(r, TsDevResult)
        assert r.success is True
        assert r.exists is False
        assert r.prog_id == 0

    def test_exists_ts_path_passed_through(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "MyStrat", True, prog_id=1234))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS", use_serve=False)
        c._is_elevated = True
        c.exists("strategy", "MyStrat")
        assert "--ts-path" in seen["cmd"]
        assert "C:/TS" in seen["cmd"]

    def test_exists_does_not_append_source(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "MyStrat", True, prog_id=1234))

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.exists("strategy", "MyStrat")
        cmd = seen["cmd"]
        # exists takes no source file: the command ends with the name, nothing appended.
        assert cmd[-4:] == ["--json", "exists", "strategy", "MyStrat"]
        assert cmd[-1] == "MyStrat"

    def test_exists_stored_name_parsed(self, client: TsDevClient, monkeypatch) -> None:
        """The ``stored_name`` field from the CLI JSON is surfaced on the result."""
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _exists_json("strategy", "ZZZ_QUERY", True, prog_id=55, stored_name="zzz_query")
            ),
        )
        r = client.exists("strategy", "ZZZ_QUERY")
        assert r.exists is True
        assert r.prog_id == 55
        assert r.stored_name == "zzz_query"

    def test_exists_stored_name_empty_when_not_found(self, client: TsDevClient, monkeypatch) -> None:
        """When the item is not found, ``stored_name`` is the empty string."""
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(_exists_json("strategy", "Nope", False)),
        )
        r = client.exists("strategy", "Nope")
        assert r.exists is False
        assert r.prog_id == 0
        assert r.stored_name == ""

    def test_exists_case_insensitive_passes_flag(self, client: TsDevClient, monkeypatch) -> None:
        """``case_insensitive=True`` appends ``--case-insensitive`` to the CLI cmd."""
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(
                _exists_json("strategy", "ZZZ_QUERY", True, prog_id=77, stored_name="zzz_query")
            )

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = client.exists("strategy", "ZZZ_QUERY", case_insensitive=True)
        cmd = seen["cmd"]
        # The --case-insensitive flag must be appended after the name.
        assert "--case-insensitive" in cmd
        assert cmd[-5:] == ["--json", "exists", "strategy", "ZZZ_QUERY", "--case-insensitive"]
        assert r.exists is True
        assert r.stored_name == "zzz_query"
        assert r.prog_id == 77

    def test_exists_case_insensitive_default_false_no_flag(self, client: TsDevClient, monkeypatch) -> None:
        """``case_insensitive`` defaults to False: no ``--case-insensitive`` flag."""
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "MyStrat", True, prog_id=1))

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.exists("strategy", "MyStrat")
        assert "--case-insensitive" not in seen["cmd"]

    def test_exists_case_insensitive_not_found(self, client: TsDevClient, monkeypatch) -> None:
        """Case-insensitive search for a nonexistent item: exists=False, no raise."""
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "ZZZ_NOPE", False))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = client.exists("strategy", "ZZZ_NOPE", case_insensitive=True)
        assert r.success is True
        assert r.exists is False
        assert r.prog_id == 0
        assert r.stored_name == ""
        assert "--case-insensitive" in seen["cmd"]


def _get_source_json(type_: str, name: str, source: str = "inputs: MyVar;\nplot MyVar;\n") -> str:
    """Fake stdout for the ``get-source`` subcommand (success payload).

    The C++ CLI emits a ``source`` field (the retrieved EasyLanguage text) in
    JSON mode, even when ``--output`` writes the source to a file as a side
    effect.
    """
    return json.dumps(
        {"op": "get-source", "type": type_, "name": name, "success": True, "exit_code": 0, "source": source}
    )


class TestGetSource:
    def test_get_source_success(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_get_source_json("strategy", "MyStrat"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = client.get_source("strategy", "MyStrat")
        assert isinstance(r, TsDevResult)
        assert r.success is True
        assert r.source == "inputs: MyVar;\nplot MyVar;\n"
        # The command tail must be exactly: --json get-source <type> <name>
        # (NO --output appended in stdout mode).
        assert seen["cmd"][-4:] == ["--json", "get-source", "strategy", "MyStrat"]

    def test_get_source_not_found_raises(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json(
                    "get-source",
                    "strategy",
                    "Nope",
                    "not_found",
                    "'Nope' not found or source unavailable.",
                    exit_code=5,
                )
            ),
        )
        with pytest.raises(NotFoundError):
            client.get_source("strategy", "Nope")

    def test_get_source_output_write_error_raises(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed(
                _failure_json(
                    "get-source",
                    "strategy",
                    "MyStrat",
                    "output_write_error",
                    'failed to write source to output file "C:/bad/path.el"',
                    exit_code=3,
                )
            ),
        )
        with pytest.raises(exc.OutputWriteError):
            client.get_source("strategy", "MyStrat", output_file="C:/bad/path.el")

    def test_get_source_with_output_file(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_get_source_json("strategy", "MyStrat"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = client.get_source("strategy", "MyStrat", output_file="C:/tmp/out.el")
        assert isinstance(r, TsDevResult)
        assert r.success is True
        # The JSON still carries the source field even in --output mode.
        assert r.source == "inputs: MyVar;\nplot MyVar;\n"
        cmd = seen["cmd"]
        assert "--output" in cmd
        assert cmd[cmd.index("--output") + 1] == "C:/tmp/out.el"

    def test_get_source_ts_path_passed_through(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_get_source_json("strategy", "MyStrat"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS", use_serve=False)
        c._is_elevated = True
        c.get_source("strategy", "MyStrat")
        assert "--ts-path" in seen["cmd"]
        assert "C:/TS" in seen["cmd"]


class TestBatch:
    def test_batch_insert_mixed(self, client: TsDevClient, monkeypatch) -> None:
        arr = json.dumps(
            [
                json.loads(_success_json("insert", "strategy", "A")),
                json.loads(_failure_json("insert", "strategy", "B", "compile_error", "bad", exit_code=8, compile_errors="err")),
                json.loads(_success_json("insert", "function", "C")),
            ]
        )
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr, returncode=2))
        results = client.batch_insert(
            [BatchItem("strategy", "A", "a;"), BatchItem("strategy", "B", "bad"), BatchItem("function", "C", "c;")]
        )
        assert len(results) == 3
        assert results[0].success is True
        assert results[1].success is False
        assert results[1].error.compile_errors == "err"
        assert results[2].success is True

    def test_batch_insert_manifest_error(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed("", stderr="manifest not found", returncode=1))
        with pytest.raises(TsDevError):
            client.batch_insert([BatchItem("strategy", "A", "a;")])

    def test_batch_delete_no_source(self, client: TsDevClient, monkeypatch) -> None:
        arr = json.dumps([json.loads(_success_json("delete", "strategy", "A"))])
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr))
        results = client.batch_delete([BatchItem("strategy", "A")])
        assert len(results) == 1 and results[0].success

    def test_batch_modify_mixed(self, client: TsDevClient, monkeypatch) -> None:
        arr = json.dumps(
            [
                json.loads(_success_json("modify", "strategy", "A", prog_id=77)),
                json.loads(_failure_json("modify", "strategy", "B", "not_found", "missing", exit_code=5)),
            ]
        )
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr, returncode=2))
        results = client.batch_modify(
            [BatchItem("strategy", "A", "a;"), BatchItem("strategy", "B", "b;")]
        )
        assert len(results) == 2
        assert results[0].success is True
        assert results[0].prog_id == 77
        assert results[1].success is False
        assert results[1].error.code == "not_found"

    def test_batch_string_sources_cleaned(self, client: TsDevClient, monkeypatch) -> None:
        paths: list[str] = []
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            assert "input" in kw and isinstance(kw["input"], str)
            for line in kw["input"].splitlines():
                if line.strip() and not line.startswith("#"):
                    parts = line.split(None, 2)
                    if len(parts) == 3:
                        paths.append(parts[2])
            arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.batch_insert([BatchItem("strategy", "A", "input(close,1);")])
        cmd = seen["cmd"]
        assert "--batch-stdin" in cmd
        assert cmd[-1] == "--batch-stdin", "cmd must end with --batch-stdin, not a manifest file path"
        assert not any(str(c).endswith(".manifest") for c in cmd), "no manifest file path may appear in cmd"
        assert "input" in seen["kw"]
        assert isinstance(seen["kw"]["input"], str)
        for p in paths:
            assert not os.path.isfile(p), f"temp source {p} must be cleaned up"


class TestBatchStdin:
    """Tests for the inline-batch-via-stdin change to ``TsDevClient._batch``.

    The manifest is built as an in-memory string and piped to the CLI through
    stdin via the ``--batch-stdin`` flag, instead of being written to a
    temporary ``.manifest`` file.  String-source materialization to temp
    ``.el`` files is still required for insert/modify items; only the manifest
    file is eliminated.
    """

    def test_batch_insert_uses_stdin_not_file(self, client: TsDevClient, monkeypatch, tmp_path: Path) -> None:
        src_a = tmp_path / "a.el"
        src_a.write_text("input(close, 1);", encoding="utf-8")
        src_b = tmp_path / "b.el"
        src_b.write_text("plot(close);", encoding="utf-8")
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps(
                [
                    json.loads(_success_json("insert", "strategy", "A")),
                    json.loads(_success_json("insert", "strategy", "B")),
                ]
            )
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_insert([BatchItem("strategy", "A", str(src_a)), BatchItem("strategy", "B", str(src_b))])
        assert len(results) == 2 and all(r.success for r in results)
        cmd = seen["cmd"]
        # (a) the cmd carries --batch-stdin and ends with it (no manifest path).
        assert "--batch-stdin" in cmd
        assert cmd[-1] == "--batch-stdin", "cmd must end with --batch-stdin, not a manifest file path"
        assert not any(str(c).endswith(".manifest") for c in cmd), "no manifest file path may appear in cmd"
        # (b) subprocess.run was called with a non-empty `input` string kwarg.
        assert "input" in seen["kw"], "subprocess.run must be called with an `input` kwarg"
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"], "input string must be non-empty"
        # (c) the input string contains the two items' lines.
        assert "strategy" in seen["kw"]["input"]
        assert "A" in seen["kw"]["input"]
        assert "B" in seen["kw"]["input"]
        # (d) no .manifest temp file was created (no manifest path in cmd).
        assert not any(Path(str(c)).suffix == ".manifest" for c in cmd)

    def test_batch_delete_stdin_no_source(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps(
                [
                    json.loads(_success_json("delete", "strategy", "A")),
                    json.loads(_success_json("delete", "function", "B")),
                ]
            )
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_delete([BatchItem("strategy", "A"), BatchItem("function", "B")])
        assert len(results) == 2 and all(r.success for r in results)
        cmd = seen["cmd"]
        assert "--batch-stdin" in cmd
        assert cmd[-1] == "--batch-stdin"
        assert "input" in seen["kw"]
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"], "input string must be non-empty"
        # The two delete lines are `<type> <name>` with no source paths.
        assert "strategy A" in seen["kw"]["input"]
        assert "function B" in seen["kw"]["input"]
        assert ".el" not in seen["kw"]["input"], "delete manifest must not reference source files"

    def test_batch_modify_string_source_materializes_temp_el(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            # The materialized temp .el file must exist at call time.
            assert "input" in kw and isinstance(kw["input"], str)
            arr = json.dumps([json.loads(_success_json("modify", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_modify([BatchItem("strategy", "A", "input(close, 1);")])
        assert len(results) == 1 and results[0].success
        cmd = seen["cmd"]
        assert "--batch-stdin" in cmd
        assert cmd[-1] == "--batch-stdin"
        assert not any(str(c).endswith(".manifest") for c in cmd), "no manifest file may be created"
        # A temp .el file was materialized and referenced in the input string.
        assert ".el" in seen["kw"]["input"], "string source must be materialized to a temp .el file"
        assert "strategy A" in seen["kw"]["input"]

    def test_batch_stdin_input_format(self, client: TsDevClient, monkeypatch, tmp_path: Path) -> None:
        src = tmp_path / "real.el"
        src.write_text("plot(close);", encoding="utf-8")
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["kw"] = kw
            arr = json.dumps(
                [
                    json.loads(_success_json("insert", "strategy", "FileSrc")),
                    json.loads(_success_json("insert", "indicator", "StrSrc")),
                ]
            )
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.batch_insert(
            [BatchItem("strategy", "FileSrc", str(src)), BatchItem("indicator", "StrSrc", "plot(close);")]
        )
        assert "input" in seen["kw"]
        payload = seen["kw"]["input"]
        # Text mode: the input must be a str (not bytes).
        assert isinstance(payload, str)
        lines = [ln for ln in payload.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
        # One non-empty, non-comment line per item.
        assert len(lines) == 2
        # insert/modify lines are `<type> <name> <source_file>`.
        assert lines[0] == f"strategy FileSrc {src}"
        assert lines[1].startswith("indicator StrSrc ")
        assert lines[1].endswith(".el"), "string source must be materialized to a .el path"

    def test_batch_stdin_empty_items(self, client: TsDevClient, monkeypatch) -> None:
        # REAL C++ CLI contract (cmdBatch): an empty manifest -- whether from a
        # file or from stdin -- is a manifest/usage error: exit 1, stderr
        # "tsdev_cli: manifest from stdin is empty\n" (stdin mode), and NO JSON
        # array on stdout.  The Python wrapper's _run_batch() raises TsDevError
        # on exit 1, so empty items -> TsDevError.  The stdin change must
        # PRESERVE this contract (empty-items semantics are out of scope): the
        # wrapper still pipes an empty string via stdin (`input=""`), drives the
        # CLI with --batch-stdin, and surfaces the CLI's exit-1 failure as a
        # TsDevError.
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed("", stderr="tsdev_cli: manifest from stdin is empty\n", returncode=1)

        monkeypatch.setattr(subprocess, "run", fake_run)
        with pytest.raises(TsDevError) as exc_info:
            client.batch_insert([])
        # The stdin mechanics must still be exercised for the empty case: the
        # cmd carries --batch-stdin (no manifest file) and an empty `input`.
        cmd = seen["cmd"]
        assert "--batch-stdin" in cmd
        assert cmd[-1] == "--batch-stdin"
        assert not any(str(c).endswith(".manifest") for c in cmd), "no manifest file may be created"
        assert "input" in seen["kw"], "subprocess.run must still be called with an `input` kwarg"
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"] == "", "empty item list must pipe an empty manifest string"
        # The CLI's manifest-level failure message must surface on the exception.
        assert "manifest from stdin is empty" in str(exc_info.value)


class TestBatchUpsert:
    """Mocked-subprocess unit tests for ``TsDevClient.batch_upsert``.

    Mirrors :class:`TestBatch` / :class:`TestBatchStdin`: ``batch_upsert``
    drives the ``batch-upsert --batch-stdin`` CLI subcommand, materializing
    string sources to temp ``.el`` files (cleaned up after the call) and
    piping the manifest via stdin.  Per-item failures are returned in the
    list (not raised); only a manifest-level failure (exit 1) raises
    :class:`TsDevError`.
    """

    def test_batch_upsert_cmd_construction(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            assert "input" in kw and isinstance(kw["input"], str)
            arr = json.dumps(
                [
                    json.loads(_success_json("upsert", "strategy", "A")),
                    json.loads(_success_json("upsert", "function", "B")),
                ]
            )
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_upsert(
            [BatchItem("strategy", "A", "a;"), BatchItem("function", "B", "b;")]
        )
        cmd = seen["cmd"]
        assert cmd == [str(client.cli_path), "--json", "batch-upsert", "--batch-stdin"]
        assert cmd[-1] == "--batch-stdin"
        assert "--batch-stdin" in cmd
        # The input manifest contains lines for both items.
        payload = seen["kw"]["input"]
        lines = [ln for ln in payload.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
        assert len(lines) == 2
        assert lines[0].startswith("strategy A ")
        assert lines[0].endswith(".el"), "string source must be materialized to a .el path"
        assert lines[1].startswith("function B ")
        assert lines[1].endswith(".el")
        # Temp source files must be cleaned up after the call.
        for ln in lines:
            src_path = ln.split(None, 2)[2]
            assert not os.path.isfile(src_path), f"temp source {src_path} must be cleaned up"
        # Results parsed correctly.
        assert len(results) == 2
        assert all(r.success for r in results)
        assert all(r.op == "upsert" for r in results)

    def test_batch_upsert_needs_source(self, client: TsDevClient, monkeypatch) -> None:
        def fail_run(*a, **k):
            pytest.fail("subprocess.run must not be called when source is None")

        monkeypatch.setattr(subprocess, "run", fail_run)
        with pytest.raises(ValueError, match="source is required"):
            client.batch_upsert([BatchItem("strategy", "A", None)])

    def test_batch_upsert_mixed_results(self, client: TsDevClient, monkeypatch) -> None:
        arr = json.dumps(
            [
                json.loads(_success_json("upsert", "strategy", "A", prog_id=77)),
                json.loads(
                    _failure_json("upsert", "strategy", "B", "compile_error", "bad", exit_code=8, compile_errors="err")
                ),
            ]
        )
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr, returncode=2))
        results = client.batch_upsert(
            [BatchItem("strategy", "A", "a;"), BatchItem("strategy", "B", "bad")]
        )
        assert len(results) == 2
        assert results[0].success is True
        assert results[0].op == "upsert"
        assert results[0].prog_id == 77
        assert results[1].success is False
        assert results[1].error is not None
        assert results[1].error.compile_errors == "err"


class TestBatchDir:
    """Mocked-subprocess unit tests for the ``batch_*_dir`` methods.

    Each drives the corresponding ``batch-<op>-dir`` CLI subcommand with
    ``<type> <dir_path> [-t <ext>...]`` arguments.  Extensions default to
    ``None`` (no ``-t`` flags → CLI uses its built-in defaults of .el+.txt).
    An empty extensions list is treated the same as ``None``.
    """

    def test_batch_insert_dir_cmd_construction(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_insert_dir("strategy", "C:/mydir")
        cmd = seen["cmd"]
        assert cmd == [str(client.cli_path), "--json", "batch-insert-dir", "strategy", "C:/mydir"]
        assert "-t" not in cmd, "no -t flags when extensions=None"
        assert seen["kw"].get("input") == "", "input must be empty string for dir batch"
        assert len(results) == 1 and results[0].success

    def test_batch_modify_dir_cmd_construction(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps([json.loads(_success_json("modify", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_modify_dir("strategy", "C:/mydir")
        cmd = seen["cmd"]
        assert cmd == [str(client.cli_path), "--json", "batch-modify-dir", "strategy", "C:/mydir"]
        assert "-t" not in cmd
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success

    def test_batch_delete_dir_cmd_construction(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps([json.loads(_success_json("delete", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_delete_dir("strategy", "C:/mydir")
        cmd = seen["cmd"]
        assert cmd == [str(client.cli_path), "--json", "batch-delete-dir", "strategy", "C:/mydir"]
        assert "-t" not in cmd
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success

    def test_batch_upsert_dir_cmd_construction(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps([json.loads(_success_json("upsert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_upsert_dir("strategy", "C:/mydir")
        cmd = seen["cmd"]
        assert cmd == [str(client.cli_path), "--json", "batch-upsert-dir", "strategy", "C:/mydir"]
        assert "-t" not in cmd
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success

    def test_batch_insert_dir_extensions(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        # Single extension.
        client.batch_insert_dir("strategy", "C:/mydir", extensions=["el"])
        cmd = seen["cmd"]
        assert cmd.count("-t") == 1
        assert cmd[cmd.index("-t") + 1] == "el"
        # Two extensions in order.
        client.batch_insert_dir("strategy", "C:/mydir", extensions=["el", "txt"])
        cmd = seen["cmd"]
        t_indices = [i for i, c in enumerate(cmd) if c == "-t"]
        assert len(t_indices) == 2
        assert cmd[t_indices[0] + 1] == "el"
        assert cmd[t_indices[1] + 1] == "txt"
        # Empty list → treated as None → no -t flags.
        client.batch_insert_dir("strategy", "C:/mydir", extensions=[])
        cmd = seen["cmd"]
        assert "-t" not in cmd
        # None → no -t flags.
        client.batch_insert_dir("strategy", "C:/mydir", extensions=None)
        cmd = seen["cmd"]
        assert "-t" not in cmd

    def test_batch_dir_exit1_raises(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess, "run", lambda *a, **k: _make_completed("", stderr="usage error", returncode=1)
        )
        with pytest.raises(TsDevError):
            client.batch_insert_dir("strategy", "C:/mydir")

    def test_batch_dir_unknown_type_raises(self, client: TsDevClient, monkeypatch) -> None:
        def fail_run(*a, **k):
            pytest.fail("subprocess.run must not be called for an unknown type")

        monkeypatch.setattr(subprocess, "run", fail_run)
        with pytest.raises(ValueError):
            client.batch_insert_dir("bogus", "C:/mydir")

    def test_batch_dir_invalid_extension_raises(self, client: TsDevClient, monkeypatch) -> None:
        def fail_run(*a, **k):
            pytest.fail("subprocess.run must not be called for an invalid extension")

        monkeypatch.setattr(subprocess, "run", fail_run)
        with pytest.raises(ValueError, match="extension"):
            client.batch_insert_dir("strategy", "C:/mydir", extensions=[""])
        with pytest.raises(ValueError, match="extension"):
            client.batch_insert_dir("strategy", "C:/mydir", extensions=["el", ""])


def _batch_progress_stderr(op: str) -> str:
    result = json.dumps(json.loads(_success_json(op, "strategy", "A")), separators=(",", ":"))
    return (
        f'{{"event":"batch_item_started","batch_operation":"{op}","index":1,"total":1,"type":"strategy","name":"A"}}\n'
        f'{{"event":"batch_item_completed","batch_operation":"{op}","index":1,"total":1,"type":"strategy","name":"A","success":true,"exit_code":0,"result":{result}}}\n'
        f'{{"event":"batch_completed","batch_operation":"{op}","completed":1,"total":1,"successes":1,"failures":0,"not_run":0,"cancelled":false,"close_failed":false,"exit_code":0}}\n'
        "orlib20: warehouse flushed\n"
    )


class TestBatchDirectJson:
    def test_batch_insert_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("insert")
        arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_insert([BatchItem("strategy", "A", "a;")])
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-insert", "--batch-stdin"]
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"], "input string must be non-empty"
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_modify_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("modify")
        arr = json.dumps([json.loads(_success_json("modify", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_modify([BatchItem("strategy", "A", "a;")])
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-modify", "--batch-stdin"]
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"], "input string must be non-empty"
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_delete_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("delete")
        arr = json.dumps([json.loads(_success_json("delete", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_delete([BatchItem("strategy", "A")])
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-delete", "--batch-stdin"]
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"], "input string must be non-empty"
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_upsert_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("upsert")
        arr = json.dumps([json.loads(_success_json("upsert", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_upsert([BatchItem("strategy", "A", "a;")])
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-upsert", "--batch-stdin"]
        assert isinstance(seen["kw"]["input"], str)
        assert seen["kw"]["input"], "input string must be non-empty"
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_insert_dir_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("insert")
        arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_insert_dir("strategy", "C:/mydir")
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-insert-dir", "strategy", "C:/mydir"]
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_modify_dir_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("modify")
        arr = json.dumps([json.loads(_success_json("modify", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_modify_dir("strategy", "C:/mydir")
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-modify-dir", "strategy", "C:/mydir"]
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_delete_dir_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("delete")
        arr = json.dumps([json.loads(_success_json("delete", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_delete_dir("strategy", "C:/mydir")
        assert seen["cmd"] == [str(client.cli_path), "--json", "batch-delete-dir", "strategy", "C:/mydir"]
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr

    def test_batch_upsert_dir_direct_includes_json(self, client: TsDevClient, monkeypatch) -> None:
        stderr = _batch_progress_stderr("upsert")
        arr = json.dumps([json.loads(_success_json("upsert", "strategy", "A"))])
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            seen["kw"] = kw
            return _make_completed(arr, stderr=stderr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        results = client.batch_upsert_dir("strategy", "C:/mydir", extensions=["el", "txt"])
        assert seen["cmd"] == [
            str(client.cli_path), "--json", "batch-upsert-dir", "strategy", "C:/mydir", "-t", "el", "-t", "txt"
        ]
        assert seen["kw"].get("input") == ""
        assert len(results) == 1 and results[0].success
        assert results[0].stdout == arr
        assert results[0].stderr == stderr


class TestVerboseForwarding:
    """Tests for the ``verbose`` parameter (repeatable ``-v`` CLI flags).

    Mirrors the C++ CLI's position-free, repeatable ``-v``/``--verbose``
    global flag: 0 = warnings+errors (default, no flags emitted), 1 = info
    (``-v``), >=2 = debug (``-vv``, clamped).  In serve mode the level is
    applied to the broker process at launch; the serve JSON protocol has no
    per-request verbosity field.
    """

    def test_direct_single_verbose_1(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "X", True, prog_id=7))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, verbose=1)
        c._is_elevated = True
        c.exists("strategy", "X")
        assert seen["cmd"] == [str(c.cli_path), "-v", "--json", "exists", "strategy", "X"]

    def test_direct_single_verbose_2(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "X", True, prog_id=7))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, verbose=2)
        c._is_elevated = True
        c.exists("strategy", "X")
        assert seen["cmd"] == [str(c.cli_path), "-v", "-v", "--json", "exists", "strategy", "X"]

    def test_verbose_clamped_to_two(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "X", True, prog_id=7))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, verbose=99)
        c._is_elevated = True
        assert c.verbose == 2
        c.exists("strategy", "X")
        assert seen["cmd"].count("-v") == 2

    def test_verbose_clamped_to_zero(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "X", True, prog_id=7))

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, verbose=-3)
        c._is_elevated = True
        assert c.verbose == 0
        c.exists("strategy", "X")
        assert seen["cmd"].count("-v") == 0

    def test_default_client_emits_no_verbose_flags(self, client: TsDevClient, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            return _make_completed(_exists_json("strategy", "X", True, prog_id=7))

        monkeypatch.setattr(subprocess, "run", fake_run)
        assert client.verbose == 0
        client.exists("strategy", "X")
        assert "-v" not in seen["cmd"]

    def test_batch_stdin_verbose_2(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, verbose=2)
        c._is_elevated = True
        results = c.batch_insert([BatchItem("strategy", "A", "input(close, 1);")])
        assert len(results) == 1 and results[0].success
        assert seen["cmd"] == [str(c.cli_path), "-v", "-v", "--json", "batch-insert", "--batch-stdin"]

    def test_batch_dir_verbose_2(self, fake_cli: Path, monkeypatch) -> None:
        seen: dict = {}

        def fake_run(cmd, **kw):
            seen["cmd"] = cmd
            arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, verbose=2)
        c._is_elevated = True
        results = c.batch_insert_dir("strategy", "C:/mydir")
        assert len(results) == 1 and results[0].success
        assert seen["cmd"] == [str(c.cli_path), "-v", "-v", "--json", "batch-insert-dir", "strategy", "C:/mydir"]

    def test_build_serve_parameters_verbose(self) -> None:
        assert _build_serve_parameters("C:/TS", "C:/hs.pipe", verbose=2) == subprocess.list2cmdline(
            ["--ts-path", "C:/TS", "serve", "--serve-pipe-file", "C:/hs.pipe", "-v", "-v"]
        )
        assert _build_serve_parameters(None, "C:/hs.pipe", verbose=1) == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe", "-v"]
        )

    def test_build_serve_parameters_default_unchanged(self) -> None:
        assert _build_serve_parameters("C:/TS", "C:/hs.pipe") == subprocess.list2cmdline(
            ["--ts-path", "C:/TS", "serve", "--serve-pipe-file", "C:/hs.pipe"]
        )
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )

    def test_build_serve_parameters_vas_guard_env_forwarding(self, monkeypatch) -> None:
        """TSDEV_WHSERVER_* env overrides are forwarded to the broker as
        explicit --whserver-* flags (ShellExecuteExW runas does not inherit
        the caller's environment block)."""
        monkeypatch.delenv("TSDEV_WHSERVER_FREE_MB", raising=False)
        monkeypatch.delenv("TSDEV_WHSERVER_LARGEST_FREE_MB", raising=False)
        monkeypatch.delenv("TSDEV_NO_WHSERVER_VAS_GUARD", raising=False)
        # No env -> byte-identical default command line.
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )
        monkeypatch.setenv("TSDEV_WHSERVER_FREE_MB", "5000")
        monkeypatch.setenv("TSDEV_WHSERVER_LARGEST_FREE_MB", "128")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe",
             "--whserver-free-mb", "5000", "--whserver-largest-free-mb", "128"]
        )
        # Garbage/out-of-range values are dropped (env semantics: keep default).
        monkeypatch.setenv("TSDEV_WHSERVER_FREE_MB", "bogus")
        monkeypatch.setenv("TSDEV_WHSERVER_LARGEST_FREE_MB", "9999999")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )
        # Disable switch: any value but "0" forwards; "0"/unset do not.
        monkeypatch.delenv("TSDEV_WHSERVER_FREE_MB")
        monkeypatch.delenv("TSDEV_WHSERVER_LARGEST_FREE_MB")
        monkeypatch.setenv("TSDEV_NO_WHSERVER_VAS_GUARD", "1")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe", "--no-whserver-vas-guard"]
        )
        monkeypatch.setenv("TSDEV_NO_WHSERVER_VAS_GUARD", "0")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )

    def test_build_serve_parameters_dispatch_timeout_env_forwarding(self, monkeypatch) -> None:
        """TSDEV_DISPATCH_TIMEOUT_SECS is forwarded to the broker as
        --dispatch-timeout-secs (ShellExecuteExW runas does not inherit
        the caller's environment block)."""
        monkeypatch.delenv("TSDEV_DISPATCH_TIMEOUT_SECS", raising=False)
        # Unset -> byte-identical default command line.
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )
        # Valid values forward verbatim, including "0" (disable).
        monkeypatch.setenv("TSDEV_DISPATCH_TIMEOUT_SECS", "120")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe",
             "--dispatch-timeout-secs", "120"]
        )
        monkeypatch.setenv("TSDEV_DISPATCH_TIMEOUT_SECS", "0")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe",
             "--dispatch-timeout-secs", "0"]
        )
        monkeypatch.setenv("TSDEV_DISPATCH_TIMEOUT_SECS", "86400")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe",
             "--dispatch-timeout-secs", "86400"]
        )
        # Garbage/out-of-range values are dropped (env semantics: keep default).
        for bad in ("bogus", "86401", "-5", "12.5", "", "²"):
            monkeypatch.setenv("TSDEV_DISPATCH_TIMEOUT_SECS", bad)
            assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
                ["serve", "--serve-pipe-file", "C:/hs.pipe"]
            ), f"value {bad!r} should not be forwarded"

    def test_build_serve_parameters_traverse_strategy_env_forwarding(self, monkeypatch) -> None:
        """TSDEV_TRAVERSE_STRATEGY_DEPENDENTS is forwarded to the broker as
        --traverse-strategy-dependents (ShellExecuteExW runas does not
        inherit the caller's environment block)."""
        monkeypatch.delenv("TSDEV_TRAVERSE_STRATEGY_DEPENDENTS", raising=False)
        # Unset -> no flag.
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )
        # Any value other than "0" forwards.
        monkeypatch.setenv("TSDEV_TRAVERSE_STRATEGY_DEPENDENTS", "1")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe",
             "--traverse-strategy-dependents"]
        )
        # "0" and "" do not forward.
        monkeypatch.setenv("TSDEV_TRAVERSE_STRATEGY_DEPENDENTS", "0")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )
        monkeypatch.setenv("TSDEV_TRAVERSE_STRATEGY_DEPENDENTS", "")
        assert _build_serve_parameters(None, "C:/hs.pipe") == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", "C:/hs.pipe"]
        )

    def test_serve_broker_start_forwards_verbose(self, monkeypatch, tmp_path: Path) -> None:
        """``_ServeBroker`` with verbose forwards ``-v`` flags in lpParameters."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")
        broker = _ServeBroker(cli_exe, verbose=2)
        handshake_dir = tmp_path / "verbose handshake directory"
        handshake_dir.mkdir()
        handshake = handshake_dir / "verbose handshake.pipe"
        pipe_name = r"\\.\pipe\tsdev_test_verbose"
        fake_handle = 4245
        captured: dict[str, str] = {}

        def fake_mkstemp(*a, **k):
            fd = os.open(handshake, os.O_CREAT | os.O_RDWR)
            return fd, str(handshake)

        def fake_shell_execute(sei):
            info = ctypes.cast(sei, ctypes.POINTER(_SHELLEXECUTEINFOW)).contents
            captured["parameters"] = info.lpParameters
            info.hProcess = fake_handle
            handshake.write_text(pipe_name, encoding="utf-8")
            return True

        monkeypatch.setattr("tsdev_automation.client.tempfile.mkstemp", fake_mkstemp)
        monkeypatch.setattr(ctypes.windll.shell32, "ShellExecuteExW", fake_shell_execute)
        monkeypatch.setattr(broker, "_process_exited", lambda: False)
        monkeypatch.setattr(_pipe, "connect", lambda name, timeout_ms=5000: 7779)
        monkeypatch.setattr("tsdev_automation.client.atexit.register", lambda func: None)

        broker.start()

        assert captured["parameters"] == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", str(handshake), "-v", "-v"]
        )

        # Avoid a real CloseHandle/pipe cleanup when this mocked broker is
        # finalized after the test.
        broker._started = False
        broker._pipe_handle = None
        broker._process_handle = None

    def test_serve_broker_start_default_no_verbose(self, monkeypatch, tmp_path: Path) -> None:
        """A default ``_ServeBroker`` emits no ``-v`` flags in lpParameters."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")
        broker = _ServeBroker(cli_exe)
        handshake = tmp_path / "default handshake.pipe"
        pipe_name = r"\\.\pipe\tsdev_test_verbose_default"
        fake_handle = 4246
        captured: dict[str, str] = {}

        def fake_mkstemp(*a, **k):
            fd = os.open(handshake, os.O_CREAT | os.O_RDWR)
            return fd, str(handshake)

        def fake_shell_execute(sei):
            info = ctypes.cast(sei, ctypes.POINTER(_SHELLEXECUTEINFOW)).contents
            captured["parameters"] = info.lpParameters
            info.hProcess = fake_handle
            handshake.write_text(pipe_name, encoding="utf-8")
            return True

        monkeypatch.setattr("tsdev_automation.client.tempfile.mkstemp", fake_mkstemp)
        monkeypatch.setattr(ctypes.windll.shell32, "ShellExecuteExW", fake_shell_execute)
        monkeypatch.setattr(broker, "_process_exited", lambda: False)
        monkeypatch.setattr(_pipe, "connect", lambda name, timeout_ms=5000: 7780)
        monkeypatch.setattr("tsdev_automation.client.atexit.register", lambda func: None)

        broker.start()

        assert captured["parameters"] == subprocess.list2cmdline(
            ["serve", "--serve-pipe-file", str(handshake)]
        )

        broker._started = False
        broker._pipe_handle = None
        broker._process_handle = None

    def test_ensure_broker_passes_client_verbose(self, fake_cli: Path, monkeypatch) -> None:
        """``_ensure_broker`` forwards the client's clamped verbose to the broker."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True, verbose=1)
        c._is_elevated = False
        constructed: dict = {}

        class SpyBroker:
            def __init__(self, cli_path, ts_path=None, verbose=0):
                constructed["cli_path"] = cli_path
                constructed["ts_path"] = ts_path
                constructed["verbose"] = verbose

            def call(self, request):
                return json.loads(_exists_json("strategy", "X", True, prog_id=3))

        monkeypatch.setattr("tsdev_automation.client._ServeBroker", SpyBroker)
        c.exists("strategy", "X")
        assert constructed["verbose"] == 1
        assert isinstance(c._broker, SpyBroker)

    def test_serve_request_dicts_stay_clean_of_verbose(self, fake_cli: Path) -> None:
        """Serve request dicts must NOT gain a ``verbose`` key (no per-request
        verbosity field exists in the serve protocol)."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True, verbose=2)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_success_json("insert", "strategy", "X"))
        c._broker = fake_broker

        c.insert("strategy", "X", "input(close,1);")
        sent = fake_broker.call.call_args[0][0]
        assert "verbose" not in sent
        assert sent == {"op": "insert", "type": "strategy", "name": "X", "source": "input(close,1);"}


class TestCliMain:
    def test_main_insert_dispatch(self, monkeypatch, fake_cli: Path, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def insert(self, type_, name, source):
                calls["insert"] = (type_, name, source)
                return TsDevResult(
                    op="insert", type=type_, name=name, success=True, exit_code=0, prog_id=1, ell_path="C:/x.ell"
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        monkeypatch.setattr(
            sys, "argv", ["tsdev-automation", "--cli-path", str(fake_cli), "insert", "strategy", "MyName", "x;"]
        )
        from tsdev_automation import main

        main()
        assert calls["insert"] == ("strategy", "MyName", "x;")


class TestCliNewSubcommands:
    """CLI dispatch tests for the ``exists``, ``get-source`` and ``upsert``
    subcommands.  These mirror :class:`TestCliMain`'s mocking style: a
    ``FakeClient`` is monkeypatched in for ``tsdev_automation._cli.TsDevClient``
    so we can assert on the dispatched method calls, and ``_main`` is invoked
    directly with an explicit ``argv`` so the integer exit code is observable.

    The subcommands are implemented; these tests verify the current dispatch
    through :func:`_main` and :func:`_build_parser`.
    """

    def test_cli_exists_found(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def exists(self, type_, name, case_insensitive=False):
                calls["exists"] = (type_, name, case_insensitive)
                return TsDevResult(
                    op="exists", type=type_, name=name, success=True, exit_code=0, exists=True, prog_id=1234
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["exists", "strategy", "MyStrat"])
        out = capsys.readouterr().out
        # The client method must be dispatched with the parsed positional args.
        assert calls["exists"] == ("strategy", "MyStrat", False)
        # A found result is a successful query: exit 0, no exception.
        assert rc == 0
        # Human output must mention the name, the prog_id, and indicate existence.
        assert "MyStrat" in out
        assert "1234" in out
        assert "exists" in out.lower()

    def test_cli_exists_not_found(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def exists(self, type_, name, case_insensitive=False):
                calls["exists"] = (type_, name, case_insensitive)
                return TsDevResult(
                    op="exists", type=type_, name=name, success=True, exit_code=0, exists=False, prog_id=0
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["exists", "strategy", "Nope"])
        out = capsys.readouterr().out
        assert calls["exists"] == ("strategy", "Nope", False)
        # Not-found is a valid query result: success, exit 0, no exception raised.
        assert rc == 0
        assert "Nope" in out
        # Output must indicate absence (robust to phrasing: "not found" / "false" / "missing").
        assert ("not" in out.lower() or "false" in out.lower() or "missing" in out.lower())

    def test_cli_exists_case_insensitive_flag(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def exists(self, type_, name, case_insensitive=False):
                calls["exists"] = (type_, name, case_insensitive)
                return TsDevResult(
                    op="exists", type=type_, name=name, success=True, exit_code=0,
                    exists=True, prog_id=1234, stored_name="mystrat",
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["exists", "strategy", "MyStrat", "--case-insensitive"])
        out = capsys.readouterr().out
        # The --case-insensitive flag must be forwarded as case_insensitive=True.
        assert calls["exists"] == ("strategy", "MyStrat", True)
        assert rc == 0
        assert "MyStrat" in out
        assert "1234" in out

    def test_cli_get_source_stdout(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def get_source(self, type_, name, output_file=None):
                calls["get_source"] = (type_, name, output_file)
                return TsDevResult(
                    op="get-source", type=type_, name=name, success=True, exit_code=0, source="inputs: MyVar;\n"
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["get-source", "strategy", "MyStrat"])
        out = capsys.readouterr().out
        # No --output flag: output_file must be None.
        assert calls["get_source"] == ("strategy", "MyStrat", None)
        assert rc == 0
        # In stdout mode the retrieved EL source is printed to stdout.
        assert "inputs: MyVar;" in out

    def test_cli_get_source_output_file(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def get_source(self, type_, name, output_file=None):
                calls["get_source"] = (type_, name, output_file)
                return TsDevResult(
                    op="get-source", type=type_, name=name, success=True, exit_code=0, source="inputs: MyVar;\n"
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["get-source", "strategy", "MyStrat", "--output", "out.el"])
        out = capsys.readouterr().out
        # --output forwards the file path to the client (str or Path accepted).
        assert calls["get_source"][:2] == ("strategy", "MyStrat")
        assert calls["get_source"][2] is not None
        assert str(calls["get_source"][2]) == "out.el"
        assert rc == 0
        # In --output mode a confirmation message naming the file is printed.
        assert "out.el" in out

    def test_cli_upsert(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def upsert(self, type_, name, source):
                calls["upsert"] = (type_, name, source)
                return TsDevResult(
                    op="upsert", type=type_, name=name, success=True, exit_code=0, prog_id=1234, ell_path="C:/x.ell"
                )

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["upsert", "strategy", "MyStrat", "src.el"])
        out = capsys.readouterr().out
        assert calls["upsert"] == ("strategy", "MyStrat", "src.el")
        assert rc == 0
        # Success output mirrors insert/modify: name + prog_id present.
        assert "MyStrat" in out
        assert "1234" in out

    def test_cli_batch_upsert(self, monkeypatch, capsys, tmp_path: Path) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def batch_upsert(self, items):
                calls["batch_upsert"] = items
                return [
                    TsDevResult(
                        op="upsert", type=it.type, name=it.name, success=True, exit_code=0, prog_id=1, ell_path="C:/x.ell"
                    )
                    for it in items
                ]

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        manifest = tmp_path / "manifest.txt"
        manifest.write_text("strategy A /path/a.el\nfunction B /path/b.el\n", encoding="utf-8")
        rc = _main(["batch-upsert", str(manifest)])
        out = capsys.readouterr().out
        items = calls["batch_upsert"]
        assert len(items) == 2
        assert isinstance(items[0], BatchItem)
        assert items[0].type == "strategy"
        assert items[0].name == "A"
        assert items[0].source == "/path/a.el"
        assert items[1].type == "function"
        assert items[1].name == "B"
        assert items[1].source == "/path/b.el"
        assert rc == 0, "all success → exit 0"
        assert "A" in out
        assert "B" in out

    def test_cli_batch_insert_dir(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def batch_insert_dir(self, type_, dir_path, extensions=None):
                calls.setdefault("batch_insert_dir", []).append((type_, dir_path, extensions))
                return [
                    TsDevResult(
                        op="insert", type=type_, name="X", success=True, exit_code=0, prog_id=1, ell_path="C:/x.ell"
                    )
                ]

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["batch-insert-dir", "strategy", "C:/mydir"])
        assert calls["batch_insert_dir"][-1] == ("strategy", "C:/mydir", None)
        assert rc == 0
        rc = _main(["batch-insert-dir", "strategy", "C:/mydir", "-t", "el", "-t", "txt"])
        assert calls["batch_insert_dir"][-1] == ("strategy", "C:/mydir", ["el", "txt"])
        assert rc == 0

    def test_cli_batch_modify_dir(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def batch_modify_dir(self, type_, dir_path, extensions=None):
                calls.setdefault("batch_modify_dir", []).append((type_, dir_path, extensions))
                return [
                    TsDevResult(
                        op="modify", type=type_, name="X", success=True, exit_code=0, prog_id=1, ell_path="C:/x.ell"
                    )
                ]

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["batch-modify-dir", "strategy", "C:/mydir"])
        assert calls["batch_modify_dir"][-1] == ("strategy", "C:/mydir", None)
        assert rc == 0
        rc = _main(["batch-modify-dir", "strategy", "C:/mydir", "-t", "el", "-t", "txt"])
        assert calls["batch_modify_dir"][-1] == ("strategy", "C:/mydir", ["el", "txt"])
        assert rc == 0

    def test_cli_batch_delete_dir(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def batch_delete_dir(self, type_, dir_path, extensions=None):
                calls.setdefault("batch_delete_dir", []).append((type_, dir_path, extensions))
                return [
                    TsDevResult(op="delete", type=type_, name="X", success=True, exit_code=0)
                ]

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["batch-delete-dir", "strategy", "C:/mydir"])
        assert calls["batch_delete_dir"][-1] == ("strategy", "C:/mydir", None)
        assert rc == 0
        rc = _main(["batch-delete-dir", "strategy", "C:/mydir", "-t", "el", "-t", "txt"])
        assert calls["batch_delete_dir"][-1] == ("strategy", "C:/mydir", ["el", "txt"])
        assert rc == 0

    def test_cli_batch_upsert_dir(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def batch_upsert_dir(self, type_, dir_path, extensions=None):
                calls.setdefault("batch_upsert_dir", []).append((type_, dir_path, extensions))
                return [
                    TsDevResult(
                        op="upsert", type=type_, name="X", success=True, exit_code=0, prog_id=1, ell_path="C:/x.ell"
                    )
                ]

        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", FakeClient)
        from tsdev_automation._cli import _main

        rc = _main(["batch-upsert-dir", "strategy", "C:/mydir"])
        assert calls["batch_upsert_dir"][-1] == ("strategy", "C:/mydir", None)
        assert rc == 0
        rc = _main(["batch-upsert-dir", "strategy", "C:/mydir", "-t", "el", "-t", "txt"])
        assert calls["batch_upsert_dir"][-1] == ("strategy", "C:/mydir", ["el", "txt"])
        assert rc == 0


class TestCliVerboseFlag:
    """``-v``/``-vv``/``--verbose`` reach :class:`TsDevClient` as ``verbose=``."""

    def _fake_client(self, calls: dict) -> type:
        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None, verbose=0):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout, "verbose": verbose}

            def exists(self, type_, name, case_insensitive=False):
                return TsDevResult(
                    op="exists", type=type_, name=name, success=True, exit_code=0, exists=True, prog_id=1
                )

        return FakeClient

    def test_cli_vv_forwards_verbose_2(self, monkeypatch, capsys) -> None:
        calls: dict = {}
        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", self._fake_client(calls))
        from tsdev_automation._cli import _main

        rc = _main(["-vv", "exists", "strategy", "MyStrat"])
        assert calls["init"]["verbose"] == 2
        assert rc == 0

    def test_cli_v_forwards_verbose_1(self, monkeypatch, capsys) -> None:
        calls: dict = {}
        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", self._fake_client(calls))
        from tsdev_automation._cli import _main

        rc = _main(["-v", "exists", "strategy", "MyStrat"])
        assert calls["init"]["verbose"] == 1
        assert rc == 0

    def test_cli_long_verbose_forwards_1(self, monkeypatch, capsys) -> None:
        calls: dict = {}
        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", self._fake_client(calls))
        from tsdev_automation._cli import _main

        rc = _main(["--verbose", "exists", "strategy", "MyStrat"])
        assert calls["init"]["verbose"] == 1
        assert rc == 0

    def test_cli_default_verbose_0(self, monkeypatch, capsys) -> None:
        calls: dict = {}
        monkeypatch.setattr("tsdev_automation._cli.TsDevClient", self._fake_client(calls))
        from tsdev_automation._cli import _main

        rc = _main(["exists", "strategy", "MyStrat"])
        assert calls["init"]["verbose"] == 0
        assert rc == 0


class TestExceptionHierarchy:
    def test_all_subclass_tsdeverror(self) -> None:
        for name in [
            "UsageError",
            "UnknownTypeError",
            "SourceFileError",
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
        ]:
            cls = getattr(exc, name)
            assert issubclass(cls, TsDevError)
            assert issubclass(cls, Exception)

    def test_exception_carries_result(self) -> None:
        r = TsDevResult(op="insert", type="strategy", name="X", success=False, exit_code=8)
        e = CompileError(r)
        assert e.result is r

    def test_crash_error_subclass_and_exit_code(self) -> None:
        """``TsDevCrashError`` is a ``TsDevError`` subclass with an ``exit_code``."""
        e = TsDevCrashError(exit_code=0xC0000005)
        assert isinstance(e, TsDevError)
        assert isinstance(e, Exception)
        assert e.exit_code == 0xC0000005
        # Default message includes the hex exit code.
        assert "0xC0000005" in str(e)

    def test_crash_error_default_message_no_exit_code(self) -> None:
        """Without an exit code, a generic message is used."""
        e = TsDevCrashError()
        assert isinstance(e, TsDevError)
        assert e.exit_code is None
        assert "crashed" in str(e).lower()

    def test_crash_error_custom_message(self) -> None:
        """An explicit message is used verbatim."""
        e = TsDevCrashError(message="boom", exit_code=42)
        assert str(e) == "boom"
        assert e.exit_code == 42


class TestResolveCliPath:
    """Unit tests for the ``_resolve_cli_path`` auto-discovery logic."""

    def test_bundle_layout_auto_discovery(self, tmp_path: Path, monkeypatch) -> None:
        """The bundle layout resolves ``<package_parent>/tsdev_cli.exe``.

        Mimics ``dist/tsdev_automation/client.py`` -> ``dist/tsdev_cli.exe`` by
        pointing ``client.__file__`` at a fake package location inside a temp
        dir and placing a fake (empty) ``tsdev_cli.exe`` as its sibling (one
        level above the package directory).
        """
        from tsdev_automation import client as client_module
        from tsdev_automation.client import _resolve_cli_path

        # Build the bundle layout:
        #   <tmp>/tsdev_automation/client.py   (fake package file)
        #   <tmp>/tsdev_cli.exe                (fake empty exe, sibling of pkg)
        pkg_dir = tmp_path / "tsdev_automation"
        pkg_dir.mkdir()
        fake_client_py = pkg_dir / "client.py"
        fake_client_py.write_bytes(b"")
        bundle_exe = tmp_path / "tsdev_cli.exe"
        bundle_exe.write_bytes(b"")

        # Ensure the env var does not short-circuit before the bundle check,
        # and make _resolve_cli_path resolve relative to the fake client.py.
        monkeypatch.delenv("TSDEV_CLI_PATH", raising=False)
        monkeypatch.setattr(client_module, "__file__", str(fake_client_py))

        resolved = _resolve_cli_path(None)
        assert resolved.resolve() == bundle_exe.resolve()

    def test_bundle_layout_falls_through_when_no_sibling_exe(
        self, tmp_path: Path, monkeypatch
    ) -> None:
        """Without a sibling exe, the bundle check is skipped (falls through).

        In the source tree (``src/tsdev_automation/client.py``) there is no
        ``src/tsdev_cli.exe``, so the bundle check must not return a path here.
        We assert the bundle check is bypassed by confirming the resolved path
        is NOT the (nonexistent) sibling exe.
        """
        from tsdev_automation import client as client_module
        from tsdev_automation.client import _resolve_cli_path

        pkg_dir = tmp_path / "tsdev_automation"
        pkg_dir.mkdir()
        fake_client_py = pkg_dir / "client.py"
        fake_client_py.write_bytes(b"")
        # NOTE: no tsdev_cli.exe placed in tmp_path.

        monkeypatch.delenv("TSDEV_CLI_PATH", raising=False)
        monkeypatch.setattr(client_module, "__file__", str(fake_client_py))

        resolved = _resolve_cli_path(None)
        # The bundle candidate is <tmp>/tsdev_cli.exe; it must not be returned
        # because it does not exist. The function falls through to the
        # CMakeLists.txt walk-up / fallback instead.
        assert resolved.resolve() != (tmp_path / "tsdev_cli.exe").resolve()


# ---------------------------------------------------------------------------
# Serve-broker integration (TVS-K.5) — mocked unit tests
# ---------------------------------------------------------------------------


class TestPipeFramedJson:
    """Unit tests for :mod:`tsdev_automation._pipe` send/recv with mocked
    ``WriteFile``/``ReadFile`` (no real named pipe)."""

    def test_send_request_writes_framed_json(self, monkeypatch) -> None:
        """``send_request`` writes a 4-byte LE length prefix + UTF-8 JSON payload."""
        written: list[bytes] = []

        def fake_write_all(handle, data):
            written.append(bytes(data))

        monkeypatch.setattr(_pipe, "_write_all", fake_write_all)
        _pipe.send_request(12345, {"op": "insert", "name": "X"})
        # Two writes: the 4-byte header and the payload.
        assert len(written) == 2
        import struct as _struct

        (n,) = _struct.unpack("<I", written[0])
        assert n == len(written[1])
        assert json.loads(written[1].decode("utf-8")) == {"op": "insert", "name": "X"}

    def test_recv_response_reads_framed_json(self, monkeypatch) -> None:
        """``recv_response`` reads the 4-byte length prefix then the payload."""
        payload = json.dumps({"op": "exists", "exists": True}).encode("utf-8")
        import struct as _struct

        frames = [_struct.pack("<I", len(payload)), payload]
        calls: list[int] = []

        def fake_read_exact(handle, n):
            idx = len(calls)
            calls.append(n)
            return frames[idx]

        monkeypatch.setattr(_pipe, "_read_exact", fake_read_exact)
        obj = _pipe.recv_response(999)
        assert obj == {"op": "exists", "exists": True}
        assert calls == [4, len(payload)]

    def test_recv_response_empty_payload(self, monkeypatch) -> None:
        """A zero-length payload decodes to an empty dict."""
        import struct as _struct

        frames = [_struct.pack("<I", 0)]

        def fake_read_exact(handle, n):
            return frames.pop(0)

        monkeypatch.setattr(_pipe, "_read_exact", fake_read_exact)
        assert _pipe.recv_response(1) == {}

    def test_close_noop_for_invalid_handle(self) -> None:
        """``close`` is a no-op for None / 0 / INVALID_HANDLE_VALUE."""
        _pipe.close(None)
        _pipe.close(0)
        _pipe.close(_pipe.INVALID_HANDLE_VALUE)


class TestServeBrokerStart:
    """Unit tests for :class:`_ServeBroker.start` with mocked
    ``ShellExecuteExW`` and pipe I/O (no real broker / UAC)."""

    def test_start_polls_pipe_file_and_connects(self, monkeypatch, tmp_path: Path) -> None:
        """``start`` writes the pipe name to the temp file, polls it, then connects."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")  # start() validates the CLI path exists first.
        broker = _ServeBroker(cli_exe)

        # Fake ShellExecuteExW: populate hProcess with a fake handle.
        fake_handle = 4242

        def fake_shell_execute(sei):
            ctypes.cast(sei, ctypes.POINTER(_SHELLEXECUTEINFOW)).contents.hProcess = fake_handle
            return True

        monkeypatch.setattr(
            ctypes.windll.shell32, "ShellExecuteExW", fake_shell_execute
        )
        # Fake WaitForSingleObject: report WAIT_TIMEOUT (process still running).
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.WaitForSingleObject",
            lambda *a, **k: 0x00000102,  # WAIT_TIMEOUT
        )
        # Intercept tempfile.mkstemp so we control the path; write the pipe
        # name to it so the poll loop finds it.  Mock os.close / os.unlink so
        # the file survives to the poll loop (the real code closes the fd and
        # unlinks the placeholder before the broker rewrites it).
        pipe_name = r"\\.\pipe\tsdev_test_1234"
        pipe_file = tmp_path / "serve.pipe"

        def fake_mkstemp(*a, **k):
            pipe_file.write_text(pipe_name, encoding="utf-8")
            return (999, str(pipe_file))

        monkeypatch.setattr("tsdev_automation.client.tempfile.mkstemp", fake_mkstemp)
        monkeypatch.setattr("tsdev_automation.client.os.close", lambda fd: None)
        monkeypatch.setattr("tsdev_automation.client.os.unlink", lambda p: None)
        # Fake _pipe.connect: just return a fake handle.
        monkeypatch.setattr(_pipe, "connect", lambda name, timeout_ms=5000: 7777)
        # Prevent atexit registration from running shutdown on interpreter exit.
        monkeypatch.setattr("tsdev_automation.client.atexit.register", lambda func: None)

        broker.start()
        assert broker._started is True
        assert broker._process_handle == fake_handle
        assert broker._pipe_handle == 7777

    def test_start_quotes_paths_and_cleans_handshake_file(
        self, monkeypatch, tmp_path: Path
    ) -> None:
        """ShellExecuteExW receives correctly quoted path arguments."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")
        broker = _ServeBroker(cli_exe, ts_path=str(tmp_path / "Trade Station" / "Install"))
        handshake_dir = tmp_path / "broker handshake directory"
        handshake_dir.mkdir()
        handshake = handshake_dir / "handshake path.pipe"
        unrelated = tmp_path / "unrelated.txt"
        unrelated.write_text("keep", encoding="utf-8")
        pipe_name = r"\\.\pipe\tsdev_test_quoted"
        fake_handle = 4243
        captured: dict[str, str] = {}

        def fake_mkstemp(*a, **k):
            fd = os.open(handshake, os.O_CREAT | os.O_RDWR)
            return fd, str(handshake)

        def fake_shell_execute(sei):
            info = ctypes.cast(sei, ctypes.POINTER(_SHELLEXECUTEINFOW)).contents
            captured["parameters"] = info.lpParameters
            info.hProcess = fake_handle
            handshake.write_text(pipe_name, encoding="utf-8")
            return True

        monkeypatch.setattr("tsdev_automation.client.tempfile.mkstemp", fake_mkstemp)
        monkeypatch.setattr(ctypes.windll.shell32, "ShellExecuteExW", fake_shell_execute)
        monkeypatch.setattr(broker, "_process_exited", lambda: False)
        monkeypatch.setattr(_pipe, "connect", lambda name, timeout_ms=5000: 7778)
        monkeypatch.setattr("tsdev_automation.client.atexit.register", lambda func: None)

        broker.start()

        assert captured["parameters"] == _build_serve_parameters(
            broker._ts_path, str(handshake)
        )
        assert captured["parameters"] == subprocess.list2cmdline(
            [
                "--ts-path",
                str(tmp_path / "Trade Station" / "Install"),
                "serve",
                "--serve-pipe-file",
                str(handshake),
            ]
        )
        assert not handshake.exists(), "handshake must be removed after normal startup"
        assert unrelated.exists(), "startup cleanup must not touch unrelated paths"

        # Avoid a real CloseHandle/pipe cleanup when this mocked broker is
        # finalized after the test.
        broker._started = False
        broker._pipe_handle = None
        broker._process_handle = None

    def test_start_raises_permission_error_on_uac_decline(self, monkeypatch, tmp_path: Path) -> None:
        """ShellExecuteExW failure preserves the error and removes the handshake."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")  # start() validates the CLI path exists first.
        broker = _ServeBroker(cli_exe)
        handshake = tmp_path / "uac failure handshake.pipe"
        handshake.write_text("placeholder", encoding="utf-8")
        fd = os.open(handshake, os.O_RDWR)
        real_close = os.close
        real_unlink = os.unlink
        unlink_calls: list[str] = []

        def fake_unlink(path):
            unlink_calls.append(os.fspath(path))
            if len(unlink_calls) == 1:
                return None  # Leave the placeholder for the outer finally.
            return real_unlink(path)

        monkeypatch.setattr(
            ctypes.windll.shell32, "ShellExecuteExW", lambda sei: False
        )
        monkeypatch.setattr(
            "tsdev_automation.client.tempfile.mkstemp",
            lambda *a, **k: (fd, str(handshake)),
        )
        monkeypatch.setattr("tsdev_automation.client.os.close", lambda value: real_close(value))
        monkeypatch.setattr("tsdev_automation.client.os.unlink", fake_unlink)
        monkeypatch.setattr("tsdev_automation.client._kernel32.GetLastError", lambda: 5)
        with pytest.raises(PermissionError, match="err=5"):
            broker.start()
        assert unlink_calls == [str(handshake), str(handshake)]
        assert not handshake.exists()

    def test_start_missing_process_handle_still_cleans_handshake(
        self, monkeypatch, tmp_path: Path
    ) -> None:
        """A successful ShellExecuteExW call without hProcess still cleans up."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")
        broker = _ServeBroker(cli_exe)
        handshake = tmp_path / "missing process handshake.pipe"
        handshake.write_text("placeholder", encoding="utf-8")
        fd = os.open(handshake, os.O_RDWR)
        real_close = os.close
        real_unlink = os.unlink
        unlink_calls: list[str] = []

        def fake_unlink(path):
            unlink_calls.append(os.fspath(path))
            if len(unlink_calls) == 1:
                return None
            return real_unlink(path)

        monkeypatch.setattr(
            "tsdev_automation.client.tempfile.mkstemp",
            lambda *a, **k: (fd, str(handshake)),
        )
        monkeypatch.setattr("tsdev_automation.client.os.close", lambda value: real_close(value))
        monkeypatch.setattr("tsdev_automation.client.os.unlink", fake_unlink)
        monkeypatch.setattr(ctypes.windll.shell32, "ShellExecuteExW", lambda sei: True)

        with pytest.raises(PermissionError, match="no process handle"):
            broker.start()

        assert unlink_calls == [str(handshake), str(handshake)]
        assert not handshake.exists()

    def test_start_polling_exception_cleans_handshake_and_preserves_error(
        self, monkeypatch, tmp_path: Path
    ) -> None:
        """Polling failures clean up while preserving the original exception."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")
        broker = _ServeBroker(cli_exe)
        handshake = tmp_path / "polling exception handshake.pipe"
        handshake.write_text("placeholder", encoding="utf-8")
        fd = os.open(handshake, os.O_RDWR)
        real_close = os.close
        real_unlink = os.unlink
        unlink_calls: list[str] = []

        def fake_unlink(path):
            unlink_calls.append(os.fspath(path))
            if len(unlink_calls) == 1:
                return None
            return real_unlink(path)

        def fake_shell_execute(sei):
            ctypes.cast(sei, ctypes.POINTER(_SHELLEXECUTEINFOW)).contents.hProcess = 4244
            return True

        monkeypatch.setattr(
            "tsdev_automation.client.tempfile.mkstemp",
            lambda *a, **k: (fd, str(handshake)),
        )
        monkeypatch.setattr("tsdev_automation.client.os.close", lambda value: real_close(value))
        monkeypatch.setattr("tsdev_automation.client.os.unlink", fake_unlink)
        monkeypatch.setattr(ctypes.windll.shell32, "ShellExecuteExW", fake_shell_execute)
        monkeypatch.setattr(
            broker,
            "_process_exited",
            lambda: (_ for _ in ()).throw(RuntimeError("poll failed")),
        )
        monkeypatch.setattr("tsdev_automation.client._wait_for_exit", lambda *a, **k: True)
        monkeypatch.setattr("tsdev_automation.client._kernel32.CloseHandle", lambda handle: True)

        with pytest.raises(RuntimeError, match="poll failed"):
            broker.start()

        assert unlink_calls == [str(handshake), str(handshake)]
        assert not handshake.exists()
        assert broker._process_handle is None

    def test_shutdown_sends_quit_and_closes_handles(self, monkeypatch, tmp_path: Path) -> None:
        """``shutdown`` sends ``{"op":"quit"}`` over the pipe and closes handles."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._started = True
        broker._pipe_handle = 111
        broker._process_handle = 222
        sent: list[dict] = []

        def fake_send(handle, obj):
            sent.append(obj)

        monkeypatch.setattr(_pipe, "send_request", fake_send)
        monkeypatch.setattr(_pipe, "close", lambda handle: None)
        monkeypatch.setattr(
            "tsdev_automation.client._wait_for_exit", lambda handle, timeout_ms: True
        )
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.CloseHandle", lambda handle: True
        )

        broker.shutdown()
        assert sent == [{"op": "quit"}]
        assert broker._pipe_handle is None
        assert broker._process_handle is None
        assert broker._started is False


class TestServeBrokerTermination:
    """Unit tests for confirmed broker process termination."""

    def test_wait_for_exit_reports_terminate_api_failure(self, monkeypatch) -> None:
        """TerminateProcess=False must not be reported as successful."""
        wait_results = iter([0x00000102])  # WAIT_TIMEOUT
        terminate_calls: list[tuple] = []
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.WaitForSingleObject",
            lambda *args: next(wait_results),
        )
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.TerminateProcess",
            lambda *args: terminate_calls.append(args) or False,
        )

        assert _wait_for_exit(222, timeout_ms=5000) is False
        assert len(terminate_calls) == 1

    def test_wait_for_exit_reports_process_still_alive(self, monkeypatch) -> None:
        """A successful terminate request still requires a terminal wait."""
        wait_results = iter([0x00000102, 0x00000102])  # timeout, timeout
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.WaitForSingleObject",
            lambda *args: next(wait_results),
        )
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.TerminateProcess",
            lambda *args: True,
        )

        assert _wait_for_exit(222, timeout_ms=5000) is False

    def test_shutdown_retains_handle_when_termination_is_unconfirmed(
        self, monkeypatch, tmp_path: Path
    ) -> None:
        """shutdown must not close or discard a live process handle."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._started = True
        broker._process_handle = 222
        monkeypatch.setattr(
            "tsdev_automation.client._wait_for_exit", lambda *args, **kwargs: False
        )
        close_calls: list[int] = []
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.CloseHandle",
            lambda handle: close_calls.append(handle.value) or True,
        )

        with pytest.raises(TimeoutError, match="did not terminate"):
            broker.shutdown()

        assert broker._process_handle == 222
        assert close_calls == []
        assert broker._started is True

    def test_reset_retains_handle_when_termination_is_unconfirmed(
        self, monkeypatch, tmp_path: Path
    ) -> None:
        """Reconnect cleanup must not discard a broker that is still alive."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._started = True
        broker._pipe_handle = 111
        broker._process_handle = 222
        monkeypatch.setattr(_pipe, "close", lambda handle: None)
        monkeypatch.setattr(
            "tsdev_automation.client._wait_for_exit", lambda *args, **kwargs: False
        )

        with pytest.raises(TimeoutError, match="did not terminate"):
            broker._reset_pipe()

        assert broker._pipe_handle is None
        assert broker._process_handle == 222
        assert broker._started is False


class TestServeBrokerWatchdogAbort:
    """A broker killed by the dispatch watchdog (exit 15) must NOT be
    reconnected/retried — a fresh elevated broker would retry the same
    request into the same wedged WHServer."""

    def test_exit_15_raises_watchdog_abort_without_reconnect(
        self, monkeypatch, tmp_path: Path
    ) -> None:
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._started = True
        broker._pipe_handle = 111
        broker._process_handle = 222

        def fail_send(handle, obj):
            raise _pipe.PipeError("broker died mid-call")

        monkeypatch.setattr(_pipe, "send_request", fail_send)
        monkeypatch.setattr(_pipe, "close", lambda handle: None)
        # The broker process object exited 15 (watchdog abort), not a crash.
        monkeypatch.setattr(broker, "_get_exit_code", lambda: 15)
        monkeypatch.setattr(
            "tsdev_automation.client._wait_for_exit", lambda *a, **kw: True
        )
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.CloseHandle", lambda h: True
        )
        # Any reconnect attempt would go through start() — record it.
        start_calls: list = []
        monkeypatch.setattr(broker, "start", lambda: start_calls.append(1))

        with pytest.raises(TsDevWatchdogAbortError, match="exit 15"):
            broker.call({"op": "exists", "type": "strategy", "name": "X"})

        assert start_calls == [], "watchdog abort must not reconnect/retry"
        assert broker._started is False
        assert broker._process_handle is None  # dead broker reaped

    def test_exit_15_is_not_a_crash_exit(self) -> None:
        """``_is_crash_exit`` must not classify the watchdog abort code."""
        is_crash, crash_type = _is_crash_exit(15)
        assert is_crash is False
        assert crash_type is None

    def test_watchdog_abort_error_maps_from_error_code(self) -> None:
        """``exception_for`` maps ``whserver_dispatch_watchdog_abort``."""
        r = TsDevResult.from_json(
            {
                "op": "insert",
                "type": "strategy",
                "name": "X",
                "success": False,
                "exit_code": 15,
                "error": {
                    "code": "whserver_dispatch_watchdog_abort",
                    "message": "w",
                },
            }
        )
        e = exc.exception_for(r)
        assert isinstance(e, TsDevWatchdogAbortError)


class TestServeRouting:
    """Unit tests for :class:`TsDevClient` serve-broker routing decisions."""

    def test_uses_subprocess_when_use_serve_false(self, fake_cli: Path, monkeypatch) -> None:
        """When ``use_serve=False`` is passed explicitly (and elevated), the
        subprocess.run path is used directly — no broker."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        # Force _is_elevated to True: use_serve=False means no broker, and when
        # elevated the direct subprocess path works without the elevation guard.
        c._is_elevated = True
        assert c._serve_active() is False

        called: dict = {}

        def fake_run(cmd, **kw):
            called["cmd"] = cmd
            return _make_completed(_success_json("insert", "strategy", "X"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = c.insert("strategy", "X", "x;")
        assert r.success
        assert c._broker is None, "no broker should be created when serve is inactive"

    def test_uses_subprocess_when_elevated(self, fake_cli: Path, monkeypatch) -> None:
        """When the session is already elevated, the subprocess.run path is used
        even if ``use_serve=True`` (zero overhead, no extra UAC)."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = True
        assert c._serve_active() is False

        called: dict = {}

        def fake_run(cmd, **kw):
            called["cmd"] = cmd
            return _make_completed(_success_json("delete", "strategy", "X"))

        monkeypatch.setattr(subprocess, "run", fake_run)
        r = c.delete("strategy", "X")
        assert r.success
        assert c._broker is None, "no broker should be created when elevated"

    def test_routes_through_broker_when_not_elevated_and_use_serve(
        self, fake_cli: Path, monkeypatch
    ) -> None:
        """When not elevated AND ``use_serve=True``, ops go through the broker."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        assert c._serve_active() is True

        # Inject a fake broker so no real UAC / pipe is touched.
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_success_json("insert", "strategy", "X"))
        c._broker = fake_broker

        r = c.insert("strategy", "X", "input(close,1);")
        assert r.success
        assert r.op == "insert"
        # The broker was called with the right request (source read inline).
        sent = fake_broker.call.call_args[0][0]
        assert sent["op"] == "insert"
        assert sent["type"] == "strategy"
        assert sent["name"] == "X"
        assert sent["source"] == "input(close,1);"

    def test_broker_exists_request_includes_case_insensitive(
        self, fake_cli: Path, monkeypatch
    ) -> None:
        """``exists`` with ``case_insensitive=True`` forwards the flag to the broker."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(
            _exists_json("strategy", "X", True, prog_id=5, stored_name="x")
        )
        c._broker = fake_broker

        r = c.exists("strategy", "X", case_insensitive=True)
        assert r.exists is True
        sent = fake_broker.call.call_args[0][0]
        assert sent["op"] == "exists"
        assert sent["case_insensitive"] is True

    def test_broker_exists_request_omits_case_insensitive_when_false(
        self, fake_cli: Path
    ) -> None:
        """``exists`` without ``case_insensitive`` does NOT add the field."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_exists_json("strategy", "X", False))
        c._broker = fake_broker

        c.exists("strategy", "X")
        sent = fake_broker.call.call_args[0][0]
        assert "case_insensitive" not in sent

    def test_broker_get_source_request_includes_output_path(
        self, fake_cli: Path
    ) -> None:
        """``get_source`` with ``output_file`` forwards it as ``output_path``."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_get_source_json("strategy", "X"))
        c._broker = fake_broker

        c.get_source("strategy", "X", output_file="C:/out.el")
        sent = fake_broker.call.call_args[0][0]
        assert sent["op"] == "get-source"
        assert sent["output_path"] == "C:/out.el"

    def test_broker_get_source_request_omits_output_path_when_none(
        self, fake_cli: Path
    ) -> None:
        """``get_source`` without ``output_file`` does NOT add ``output_path``."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_get_source_json("strategy", "X"))
        c._broker = fake_broker

        c.get_source("strategy", "X")
        sent = fake_broker.call.call_args[0][0]
        assert "output_path" not in sent

    def test_broker_delete_has_no_source(self, fake_cli: Path) -> None:
        """``delete`` through the broker omits the ``source`` field."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_success_json("delete", "strategy", "X"))
        c._broker = fake_broker

        c.delete("strategy", "X")
        sent = fake_broker.call.call_args[0][0]
        assert sent["op"] == "delete"
        assert "source" not in sent

    def test_broker_lazy_started_on_first_op(self, fake_cli: Path, monkeypatch) -> None:
        """The broker is NOT created in ``__init__``; it is created on first op."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        assert c._broker is None

        # Patch _ServeBroker so start() is a no-op and call() returns a success.
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_success_json("insert", "strategy", "X"))
        monkeypatch.setattr(
            "tsdev_automation.client._ServeBroker", lambda *a, **k: fake_broker
        )
        c.insert("strategy", "X", "x;")
        assert c._broker is fake_broker

    def test_broker_failure_raises_typed_exception(self, fake_cli: Path) -> None:
        """A failed broker response is surfaced via the typed exception path."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(
            _failure_json("insert", "strategy", "Dup", "already_exists", "exists", exit_code=5, existing_prog_id=7)
        )
        c._broker = fake_broker

        with pytest.raises(AlreadyExistsError) as ei:
            c.insert("strategy", "Dup", "x;")
        assert ei.value.result.error.existing_prog_id == 7


def _serve_client(tmp_path: Path, broker=None) -> TsDevClient:
    """A non-elevated serve-routed client with an (optional) injected broker."""
    fake_cli = tmp_path / "fake_cli.exe"
    fake_cli.write_text("")
    c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
    c._is_elevated = False
    if broker is not None:
        c._broker = broker
    return c


_HEX_DIGITS = frozenset("0123456789abcdef")


def _progress_member(batch: dict) -> dict:
    """The ``progress`` member a serve response carries for a tagged request."""
    return {
        "batch_id": batch.get("id"),
        "index": batch.get("index"),
        "total": batch.get("total"),
        "phase": "completed",
    }


def _serve_batch_broker(outcomes: list[dict]) -> MagicMock:
    """Fake serve broker enforcing the tagged-batch request/response contract.

    ``call()`` requires every request to carry ``batch`` metadata of exactly
    ``{id, index, total}`` — a ``uuid4().hex`` id stable across the whole
    batch and ``1 <= index <= total`` — and returns the next outcome dict
    enriched with the matching ``progress`` member, mirroring the real serve
    response enrichment.
    """
    broker = MagicMock()
    calls: list[dict] = []
    seen_ids: list[str] = []

    def _call(request: dict) -> dict:
        batch = request.get("batch")
        assert isinstance(batch, dict), f"batch request must carry batch metadata: {request!r}"
        assert set(batch) == {"id", "index", "total"}, (
            f"batch metadata must be exactly id/index/total: {batch!r}"
        )
        assert (
            isinstance(batch["id"], str)
            and len(batch["id"]) == 32
            and set(batch["id"]) <= _HEX_DIGITS
        ), f"batch id must be a uuid4().hex string: {batch!r}"
        assert isinstance(batch["index"], int) and isinstance(batch["total"], int)
        assert 1 <= batch["index"] <= batch["total"], (
            f"batch index/total out of range: {batch!r}"
        )
        if seen_ids:
            assert batch["id"] == seen_ids[0], (
                f"batch id must be stable across the batch: {batch!r}"
            )
        else:
            seen_ids.append(batch["id"])
        calls.append(request)
        resp = dict(outcomes[len(calls) - 1])
        resp["progress"] = _progress_member(batch)
        return resp

    broker.call.side_effect = _call
    return broker


def _assert_batch_request(
    request: dict,
    op: str,
    type_: str,
    name: str,
    index: int,
    total: int,
    source: str | None = None,
) -> str:
    """Assert a tagged serve request's ordinary fields and exact batch metadata.

    Returns the batch id so tests can assert id stability across requests.
    """
    expected = {"op": op, "type": type_, "name": name}
    if source is not None:
        expected["source"] = source
    req = dict(request)
    batch = req.pop("batch")
    assert req == expected
    assert set(batch) == {"id", "index", "total"}
    assert (
        isinstance(batch["id"], str)
        and len(batch["id"]) == 32
        and set(batch["id"]) <= _HEX_DIGITS
    )
    assert batch["index"] == index
    assert batch["total"] == total
    return batch["id"]


def _assert_cancelled_marker(result: TsDevResult, op: str, type_: str, name: str) -> None:
    """Assert the exact synthetic not-run cancellation marker fields."""
    assert result.op == op
    assert result.type == type_
    assert result.name == name
    assert result.success is False
    assert result.exit_code == 12
    assert result.error is not None
    assert result.error.code == "cancelled"
    assert result.error.message == "Batch cancelled before this item was attempted"


class TestServeBatchRouting:
    """Batch ops route through the serve broker when _serve_active()."""

    def test_batch_insert_serve_routes_through_broker(self, tmp_path):
        # Two items: one success, one failure (already_exists).
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "A", "success": True, "exit_code": 0, "prog_id": 100},
            {"op": "insert", "type": "strategy", "name": "B", "success": False, "exit_code": 5,
             "error": {"code": "already_exists", "message": "B exists"}},
        ])
        c = _serve_client(tmp_path, fake_broker)
        items = [
            BatchItem(type="strategy", name="A", source="// EL source A"),
            BatchItem(type="strategy", name="B", source="// EL source B"),
        ]
        results = c.batch_insert(items)
        assert len(results) == 2
        assert results[0].success is True
        assert results[0].name == "A"
        assert results[1].success is False
        assert results[1].error is not None
        assert results[1].error.code == "already_exists"
        # Verify broker was called twice with correctly tagged requests.
        assert fake_broker.call.call_count == 2
        req0 = fake_broker.call.call_args_list[0][0][0]
        batch_id = _assert_batch_request(req0, "insert", "strategy", "A", 1, 2, "// EL source A")
        req1 = fake_broker.call.call_args_list[1][0][0]
        assert _assert_batch_request(req1, "insert", "strategy", "B", 2, 2, "// EL source B") == batch_id

    def test_batch_upsert_serve_routes_through_broker(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "upsert", "type": "function", "name": "F", "success": True, "exit_code": 0, "prog_id": 200},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_upsert([BatchItem(type="function", name="F", source="// EL")])
        assert len(results) == 1
        assert results[0].success is True
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "upsert", "function", "F", 1, 1, "// EL")

    def test_batch_delete_serve_routes_through_broker(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "X", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_delete([BatchItem(type="strategy", name="X")])
        assert len(results) == 1
        assert results[0].success is True
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "delete", "strategy", "X", 1, 1)

    def test_batch_modify_serve_routes_through_broker(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "modify", "type": "indicator", "name": "I", "success": True, "exit_code": 0, "prog_id": 300},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_modify([BatchItem(type="indicator", name="I", source="// EL")])
        assert len(results) == 1
        assert results[0].success is True
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "modify", "indicator", "I", 1, 1, "// EL")

    def test_batch_serve_source_none_raises_valueerror(self, tmp_path):
        c = _serve_client(tmp_path, MagicMock())
        with pytest.raises(ValueError, match="source is required"):
            c.batch_insert([BatchItem(type="strategy", name="A", source=None)])

    def test_batch_serve_per_item_failure_not_raised(self, tmp_path):
        """Per-item failures are collected, not raised (matching non-serve batch)."""
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "A", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "B", "success": False, "exit_code": 8,
             "error": {"code": "compile_error", "message": "bad code"}},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert([
            BatchItem(type="strategy", name="A", source="// ok"),
            BatchItem(type="strategy", name="B", source="// bad"),
        ])
        assert len(results) == 2
        assert results[0].success is True
        assert results[1].success is False
        assert results[1].error.code == "compile_error"

    def test_batch_serve_validates_type_upfront(self, tmp_path):
        """Unknown type raises ValueError before any broker call."""
        fake_broker = MagicMock()
        c = _serve_client(tmp_path, fake_broker)
        with pytest.raises(ValueError, match="unknown component type"):
            c.batch_insert([BatchItem(type="bogus", name="A", source="// EL")])
        fake_broker.call.assert_not_called()

    def test_batch_works_when_elevated_even_with_use_serve(self, monkeypatch, tmp_path):
        """Elevated + use_serve=True -> subprocess.run path (no broker)."""
        fake_cli = tmp_path / "fake_cli.exe"
        fake_cli.write_text("")
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = True
        fake_run = MagicMock(return_value=_make_completed(
            '[{"op":"insert","type":"strategy","name":"A","success":true,"exit_code":0,"prog_id":1}]'
        ))
        monkeypatch.setattr(subprocess, "run", fake_run)
        results = c.batch_insert([BatchItem(type="strategy", name="A", source="// EL")])
        assert len(results) == 1
        assert results[0].success is True
        fake_run.assert_called_once()

    def test_batch_works_when_use_serve_false(self, monkeypatch, tmp_path):
        """use_serve=False + elevated -> subprocess.run path."""
        fake_cli = tmp_path / "fake_cli.exe"
        fake_cli.write_text("")
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        c._is_elevated = True
        fake_run = MagicMock(return_value=_make_completed(
            '[{"op":"delete","type":"strategy","name":"X","success":true,"exit_code":0}]'
        ))
        monkeypatch.setattr(subprocess, "run", fake_run)
        results = c.batch_delete([BatchItem(type="strategy", name="X")])
        assert len(results) == 1
        assert results[0].success is True
        fake_run.assert_called_once()


    def test_batch_serve_broker_crash_propagates(self, tmp_path):
        """Broker crash during batch propagates as TsDevCrashError, not collected."""
        fake_broker = MagicMock()
        fake_broker.call.side_effect = TsDevCrashError(message="broker crashed", exit_code=0xC0000005)
        c = _serve_client(tmp_path, fake_broker)
        with pytest.raises(TsDevCrashError, match="broker crashed"):
            c.batch_insert([BatchItem(type="strategy", name="A", source="// EL")])

    def test_batch_serve_source_as_file_path(self, tmp_path):
        """BatchItem.source as a file path is read via _read_source."""
        # Create a real .el file to use as source.
        src_file = tmp_path / "my_strategy.el"
        src_file.write_text("// real file source")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "test", "success": True, "exit_code": 0, "prog_id": 1},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert([BatchItem(type="strategy", name="test", source=str(src_file))])
        assert len(results) == 1
        assert results[0].success is True
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "insert", "strategy", "test", 1, 1, "// real file source")

    def test_batch_serve_vas_abort_fills_not_run_markers(self, tmp_path):
        """A broker exit-14 refusal (WHServer VAS headroom) aborts the batch:
        the refusal is recorded as the item's result, the unattempted
        remainder becomes exit-12 markers carrying the abort message, and no
        further requests are sent."""
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "A", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "B", "success": False, "exit_code": 14,
             "error": {"code": "whserver_vas_abort",
                       "message": "WHServer VAS headroom below threshold: free=10MB largest=4MB "
                                  "(whserver_vas_free=10MB); refusing insert before dispatch"}},
        ])
        c = _serve_client(tmp_path, fake_broker)
        progressed: list[int] = []

        def on_progress(p):
            progressed.append(p.index)
            return True

        results = c.batch_insert(
            [
                BatchItem(type="strategy", name="A", source="// a"),
                BatchItem(type="strategy", name="B", source="// b"),
                BatchItem(type="strategy", name="C", source="// c"),
            ],
            on_progress=on_progress,
        )
        # No request was sent for item C — the batch stopped at the refusal.
        assert fake_broker.call.call_count == 2
        assert len(results) == 3
        assert results[0].success is True
        # The refused item keeps its real exit-14 result.
        assert results[1].success is False
        assert results[1].exit_code == 14
        assert results[1].error is not None
        assert results[1].error.code == "whserver_vas_abort"
        assert "WHServer VAS headroom" in results[1].error.message
        # The never-attempted remainder is an exit-12 marker that says WHY.
        assert results[2].success is False
        assert results[2].exit_code == 12
        assert results[2].name == "C"
        assert results[2].error is not None
        assert results[2].error.code == "cancelled"
        assert results[2].error.message == (
            "Batch aborted (WHServer VAS headroom) before this item was attempted"
        )
        # The refused item was never attempted -> no on_progress callback
        # for index 2 (only item 1 reported real progress).
        assert progressed == [1]

    def test_batch_serve_vas_abort_first_item(self, tmp_path):
        """A refusal on the very first item leaves a full-length list:
        index 1 is the exit-14 refusal, the rest are exit-12 abort markers."""
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "A", "success": False, "exit_code": 14,
             "error": {"code": "whserver_vas_abort", "message": "WHServer VAS headroom"}},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_delete([
            BatchItem(type="strategy", name="A"),
            BatchItem(type="strategy", name="B"),
        ])
        assert fake_broker.call.call_count == 1
        assert len(results) == 2
        assert results[0].exit_code == 14
        assert results[0].error.code == "whserver_vas_abort"
        assert results[1].exit_code == 12
        assert results[1].error.message == (
            "Batch aborted (WHServer VAS headroom) before this item was attempted"
        )


class TestServeBatchDirRouting:
    """batch_*_dir ops route through the serve broker when _serve_active()."""

    def test_batch_insert_dir_serve_enumerates_files(self, tmp_path):
        # Create a temp dir with .el files.
        d = tmp_path / "el_dir"
        d.mkdir()
        (d / "alpha.el").write_text("// alpha source")
        (d / "beta.el").write_text("// beta source")
        (d / "readme.md").write_text("not EL")  # should be skipped
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "alpha", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "beta", "success": True, "exit_code": 0, "prog_id": 2},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert len(results) == 2
        assert results[0].name == "alpha"
        assert results[1].name == "beta"
        assert fake_broker.call.call_count == 2
        req0 = fake_broker.call.call_args_list[0][0][0]
        batch_id = _assert_batch_request(req0, "insert", "strategy", "alpha", 1, 2, "// alpha source")
        req1 = fake_broker.call.call_args_list[1][0][0]
        assert _assert_batch_request(req1, "insert", "strategy", "beta", 2, 2, "// beta source") == batch_id

    def test_batch_delete_dir_serve_no_source_read(self, tmp_path):
        """batch_delete_dir in serve mode does not read file content."""
        d = tmp_path / "del_dir"
        d.mkdir()
        (d / "gamma.el").write_text("content ignored")
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "gamma", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_delete_dir("strategy", str(d))
        assert len(results) == 1
        assert results[0].success is True
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "delete", "strategy", "gamma", 1, 1)

    def test_batch_dir_serve_extension_filtering(self, tmp_path):
        d = tmp_path / "ext_dir"
        d.mkdir()
        (d / "a.el").write_text("// a")
        (d / "b.txt").write_text("// b")
        (d / "c.md").write_text("// c")
        # Default extensions: .el + .txt -> a and b, not c.
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "a", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "b", "success": True, "exit_code": 0, "prog_id": 2},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert len(results) == 2
        names = [r.name for r in results]
        assert "a" in names
        assert "b" in names
        assert "c" not in names

    def test_batch_dir_serve_t_override(self, tmp_path):
        """Custom -t extension overrides defaults."""
        d = tmp_path / "override_dir"
        d.mkdir()
        (d / "x.el").write_text("// x")
        (d / "y.eld").write_text("// y")
        # Only .eld files.
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "y", "success": True, "exit_code": 0, "prog_id": 1},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d), extensions=["eld"])
        assert len(results) == 1
        assert results[0].name == "y"

    def test_batch_dir_serve_nonexistent_dir_raises(self, tmp_path):
        c = _serve_client(tmp_path, MagicMock())
        with pytest.raises(TsDevError, match="failed to open directory"):
            c.batch_insert_dir("strategy", str(tmp_path / "nonexistent"))

    def test_batch_dir_serve_empty_dir_raises(self, tmp_path):
        d = tmp_path / "empty_dir"
        d.mkdir()
        c = _serve_client(tmp_path, MagicMock())
        with pytest.raises(TsDevError, match="no files matching"):
            c.batch_insert_dir("strategy", str(d))

    def test_batch_dir_serve_unknown_type_raises(self, tmp_path):
        c = _serve_client(tmp_path, MagicMock())
        with pytest.raises(ValueError, match="unknown component type"):
            c.batch_insert_dir("bogus", str(tmp_path))

    def test_batch_dir_serve_invalid_extension_raises(self, tmp_path):
        d = tmp_path / "ext_err_dir"
        d.mkdir()
        c = _serve_client(tmp_path, MagicMock())
        with pytest.raises(ValueError, match="invalid extension"):
            c.batch_insert_dir("strategy", str(d), extensions=[""])

    def test_batch_dir_serve_subdirs_skipped(self, tmp_path):
        d = tmp_path / "subdir_dir"
        d.mkdir()
        (d / "real.el").write_text("// real")
        sub = d / "subdir.el"
        sub.mkdir()  # a directory named .el — should be skipped
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "real", "success": True, "exit_code": 0, "prog_id": 1},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert len(results) == 1
        assert results[0].name == "real"

    def test_batch_dir_serve_items_sorted_by_name(self, tmp_path):
        d = tmp_path / "sort_dir"
        d.mkdir()
        (d / "zebra.el").write_text("// z")
        (d / "alpha.el").write_text("// a")
        (d / "mango.el").write_text("// m")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "alpha", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "mango", "success": True, "exit_code": 0, "prog_id": 2},
            {"op": "insert", "type": "strategy", "name": "zebra", "success": True, "exit_code": 0, "prog_id": 3},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert [r.name for r in results] == ["alpha", "mango", "zebra"]


    def test_batch_dir_serve_source_read_failure_collected(self, monkeypatch, tmp_path):
        """Source-read failure produces source_file_error result, not raised."""
        d = tmp_path / "src_err_dir"
        d.mkdir()
        good = d / "good.el"
        good.write_text("// good source")
        bad = d / "bad.el"
        bad.write_text("// bad source")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "good", "success": True, "exit_code": 0, "prog_id": 1},
        ])
        c = _serve_client(tmp_path, fake_broker)
        # Make read_text raise OSError only for bad.el.
        real_read_text = Path.read_text
        def fake_read_text(self, *args, **kwargs):
            if self.name == "bad.el":
                raise OSError("permission denied")
            return real_read_text(self, *args, **kwargs)
        monkeypatch.setattr(Path, "read_text", fake_read_text)
        results = c.batch_insert_dir("strategy", str(d))
        assert len(results) == 2
        # bad.el comes before good.el alphabetically.
        assert results[0].name == "bad"
        assert results[0].success is False
        assert results[0].exit_code == 3
        assert results[0].error is not None
        assert results[0].error.code == "source_file_error"
        assert results[1].name == "good"
        assert results[1].success is True
        # Broker was only called for the good file — at its input position.
        assert fake_broker.call.call_count == 1
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "insert", "strategy", "good", 2, 2, "// good source")

    def test_batch_dir_serve_broker_crash_propagates(self, tmp_path):
        """Broker crash during batch_dir propagates as exception, not collected."""
        d = tmp_path / "crash_dir"
        d.mkdir()
        (d / "x.el").write_text("// x")
        fake_broker = MagicMock()
        fake_broker.call.side_effect = TsDevCrashError(message="broker crashed", exit_code=0xC0000005)
        c = _serve_client(tmp_path, fake_broker)
        with pytest.raises(TsDevCrashError, match="broker crashed"):
            c.batch_insert_dir("strategy", str(d))

    def test_batch_dir_serve_vas_abort_fills_not_run_markers(self, tmp_path):
        """A broker exit-14 refusal aborts the dir batch the same way as
        _batch_serve: refusal recorded, remainder exit-12 abort markers, no
        further requests."""
        d = tmp_path / "vas_dir"
        d.mkdir()
        (d / "a.el").write_text("// a")
        (d / "b.el").write_text("// b")
        (d / "c.el").write_text("// c")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "a", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "b", "success": False, "exit_code": 14,
             "error": {"code": "whserver_vas_abort",
                       "message": "WHServer VAS headroom below threshold"}},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert fake_broker.call.call_count == 2
        assert len(results) == 3
        assert results[0].success is True
        assert results[1].exit_code == 14
        assert results[1].error.code == "whserver_vas_abort"
        assert results[2].name == "c"
        assert results[2].exit_code == 12
        assert results[2].error.message == (
            "Batch aborted (WHServer VAS headroom) before this item was attempted"
        )

    def test_batch_dir_serve_case_insensitive_extension(self, tmp_path):
        """File extension matching is case-insensitive."""
        d = tmp_path / "ci_dir"
        d.mkdir()
        (d / "upper.EL").write_text("// upper")
        (d / "lower.el").write_text("// lower")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "lower", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "upper", "success": True, "exit_code": 0, "prog_id": 2},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert len(results) == 2
        names = [r.name for r in results]
        assert "upper" in names
        assert "lower" in names

    def test_batch_dir_serve_dot_prefix_optional(self, tmp_path):
        """Extension without dot prefix matches same as with dot."""
        d = tmp_path / "dot_dir"
        d.mkdir()
        (d / "a.el").write_text("// a")
        (d / "b.txt").write_text("// b")
        # "el" (no dot) should match .el files only (overrides defaults).
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "a", "success": True, "exit_code": 0, "prog_id": 1},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d), extensions=["el"])
        assert len(results) == 1
        assert results[0].name == "a"

    def test_batch_dir_serve_multi_dot_stem(self, tmp_path):
        """path.stem() strips only the last extension."""
        d = tmp_path / "multi_dot_dir"
        d.mkdir()
        (d / "my.strategy.el").write_text("// ms")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "my.strategy", "success": True, "exit_code": 0, "prog_id": 1},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_insert_dir("strategy", str(d))
        assert len(results) == 1
        assert results[0].name == "my.strategy"


class TestBatchProgressApi:
    """TVS B public API surface: BatchProgress model, alias, signatures."""

    _BATCH_METHODS = (
        "batch_insert", "batch_modify", "batch_delete", "batch_upsert",
        "batch_insert_dir", "batch_modify_dir", "batch_delete_dir", "batch_upsert_dir",
    )

    def test_batch_progress_frozen_dataclass_exported(self) -> None:
        from tsdev_automation import BatchProgress, BatchProgressCallback
        from tsdev_automation.results import (
            BatchProgress as ResultsBatchProgress,
            BatchProgressCallback as ResultsBatchProgressCallback,
        )

        assert BatchProgress is ResultsBatchProgress
        assert BatchProgressCallback is ResultsBatchProgressCallback
        result = TsDevResult(op="insert", type="strategy", name="A", success=True, exit_code=0)
        progress = BatchProgress(index=1, total=3, result=result)
        assert progress.index == 1
        assert progress.total == 3
        assert progress.result is result
        with pytest.raises(FrozenInstanceError):
            progress.index = 2

    def test_batch_progress_callback_alias(self) -> None:
        from typing import Callable

        from tsdev_automation import BatchProgress, BatchProgressCallback

        assert BatchProgressCallback == Callable[[BatchProgress], bool | None]

    @pytest.mark.parametrize("method_name", _BATCH_METHODS)
    def test_on_progress_is_keyword_only(self, method_name: str) -> None:
        sig = inspect.signature(getattr(TsDevClient, method_name))
        param = sig.parameters.get("on_progress")
        assert param is not None, f"{method_name} must accept on_progress"
        assert param.kind is inspect.Parameter.KEYWORD_ONLY
        assert param.default is None

    @pytest.mark.parametrize("method_name", _BATCH_METHODS)
    def test_on_progress_rejects_positional(self, method_name: str, client: TsDevClient) -> None:
        method = getattr(client, method_name)
        if method_name.endswith("_dir"):
            args: tuple = ("strategy", "C:/mydir", ["el"], lambda p: None)
        else:
            args = ([BatchItem("strategy", "A", "a;")], lambda p: None)
        with pytest.raises(TypeError):
            method(*args)


class TestServeBatchOnProgress:
    """TVS B serve path: tagged requests, validated progress, on_progress."""

    _OPS = [
        pytest.param(
            "batch_insert", "insert",
            [BatchItem("strategy", "A", "// a"), BatchItem("function", "B", "// b")],
            id="insert",
        ),
        pytest.param(
            "batch_modify", "modify",
            [BatchItem("indicator", "A", "// a"), BatchItem("strategy", "B", "// b")],
            id="modify",
        ),
        pytest.param(
            "batch_delete", "delete",
            [BatchItem("strategy", "A"), BatchItem("function", "B")],
            id="delete",
        ),
        pytest.param(
            "batch_upsert", "upsert",
            [BatchItem("strategy", "A", "// a"), BatchItem("function", "B", "// b")],
            id="upsert",
        ),
    ]

    @pytest.mark.parametrize("method_name,op,items", _OPS)
    def test_tagged_requests_and_callbacks_all_ops(self, tmp_path, method_name, op, items):
        outcomes = [
            {"op": op, "type": it.type, "name": it.name, "success": True, "exit_code": 0, "prog_id": i + 1}
            for i, it in enumerate(items)
        ]
        fake_broker = _serve_batch_broker(outcomes)
        c = _serve_client(tmp_path, fake_broker)
        from tsdev_automation import BatchProgress

        seen: list = []
        results = getattr(c, method_name)(items, on_progress=seen.append)
        assert len(results) == 2
        assert all(r.success for r in results)
        assert fake_broker.call.call_count == 2
        batch_id = None
        for i, it in enumerate(items):
            req = fake_broker.call.call_args_list[i][0][0]
            rid = _assert_batch_request(req, op, it.type, it.name, i + 1, 2, it.source)
            if batch_id is None:
                batch_id = rid
            else:
                assert rid == batch_id, "batch id must be stable across the batch"
            progress = seen[i]
            assert isinstance(progress, BatchProgress)
            assert progress.index == i + 1
            assert progress.total == 2
            assert progress.result == results[i]
        assert len(seen) == 2

    def test_no_callback_still_tags_requests(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "A", "success": True, "exit_code": 0},
            {"op": "delete", "type": "strategy", "name": "B", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        results = c.batch_delete([BatchItem("strategy", "A"), BatchItem("strategy", "B")])
        assert [r.success for r in results] == [True, True]
        assert fake_broker.call.call_count == 2

    def test_callback_invoked_after_success_and_failure_in_order(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "A", "success": True, "exit_code": 0, "prog_id": 1},
            {"op": "insert", "type": "strategy", "name": "B", "success": False, "exit_code": 8,
             "error": {"code": "compile_error", "message": "bad"}},
            {"op": "insert", "type": "strategy", "name": "C", "success": True, "exit_code": 0, "prog_id": 2},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []
        results = c.batch_insert(
            [
                BatchItem("strategy", "A", "// a"),
                BatchItem("strategy", "B", "// b"),
                BatchItem("strategy", "C", "// c"),
            ],
            on_progress=seen.append,
        )
        assert len(results) == 3
        assert [p.index for p in seen] == [1, 2, 3]
        assert [p.total for p in seen] == [3, 3, 3]
        assert [p.result.success for p in seen] == [True, False, True]
        assert seen[1].result.error is not None
        assert seen[1].result.error.code == "compile_error"
        for p, r in zip(seen, results):
            assert p.result == r

    def test_true_none_and_zero_continue(self, tmp_path):
        """Only ``return is False`` cancels: True/None/0 all continue."""
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "A", "success": True, "exit_code": 0},
            {"op": "delete", "type": "strategy", "name": "B", "success": True, "exit_code": 0},
            {"op": "delete", "type": "strategy", "name": "C", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        returns = iter([True, None, 0])
        results = c.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("strategy", "B"), BatchItem("strategy", "C")],
            on_progress=lambda p: next(returns),
        )
        assert fake_broker.call.call_count == 3
        assert len(results) == 3
        assert all(r.success for r in results)

    def test_false_cancels_after_success(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "A", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []

        def cb(p):
            seen.append(p)
            return False

        results = c.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("function", "B"), BatchItem("indicator", "C")],
            on_progress=cb,
        )
        assert fake_broker.call.call_count == 1
        assert len(seen) == 1, "callback must not be invoked for not-run markers"
        assert len(results) == 3
        assert results[0].success is True
        _assert_cancelled_marker(results[1], "delete", "function", "B")
        _assert_cancelled_marker(results[2], "delete", "indicator", "C")

    def test_false_cancels_after_failure(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "A", "success": False, "exit_code": 5,
             "error": {"code": "not_found", "message": "missing"}},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []

        def cb(p):
            seen.append(p)
            return False

        results = c.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("strategy", "B")],
            on_progress=cb,
        )
        assert fake_broker.call.call_count == 1
        assert len(seen) == 1
        assert len(results) == 2
        assert results[0].success is False
        assert results[0].error is not None and results[0].error.code == "not_found"
        _assert_cancelled_marker(results[1], "delete", "strategy", "B")

    def test_false_on_final_item_ignored(self, tmp_path):
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "A", "success": True, "exit_code": 0},
            {"op": "delete", "type": "strategy", "name": "B", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []

        def cb(p):
            seen.append(p)
            # False only on the final item — must be ignored.
            return p.index < p.total

        results = c.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("strategy", "B")],
            on_progress=cb,
        )
        assert fake_broker.call.call_count == 2
        assert len(seen) == 2
        assert len(results) == 2
        assert all(r.success for r in results)

    def test_callback_exception_stops_and_reraises(self, tmp_path):
        """A callback exception stops broker requests, re-raises the original,
        and leaves the broker usable (no marker list is returned)."""
        broker = MagicMock()

        def _call(request: dict) -> dict:
            resp = {
                "op": request.get("op"), "type": "strategy",
                "name": request.get("name"), "success": True, "exit_code": 0,
            }
            batch = request.get("batch")
            if batch is not None:
                resp["progress"] = _progress_member(batch)
            return resp

        broker.call.side_effect = _call
        c = _serve_client(tmp_path, broker)
        seen: list = []

        class Boom(Exception):
            pass

        def cb(p):
            seen.append(p)
            raise Boom("callback failed")

        with pytest.raises(Boom, match="callback failed"):
            c.batch_delete(
                [BatchItem("strategy", "A"), BatchItem("strategy", "B")],
                on_progress=cb,
            )
        assert broker.call.call_count == 1, "no further broker requests after callback exception"
        assert len(seen) == 1
        # The broker stays usable for subsequent ops through the same client.
        assert c.exists("strategy", "Z").success is True

    _OMIT = object()

    @pytest.mark.parametrize(
        "make_progress",
        [
            pytest.param(lambda b: TestServeBatchOnProgress._OMIT, id="missing"),
            pytest.param(lambda b: "completed", id="nonobject-string"),
            pytest.param(lambda b: 7, id="nonobject-int"),
            pytest.param(lambda b: {**_progress_member(b), "extra": 1}, id="extra-member"),
            pytest.param(lambda b: {**_progress_member(b), "batch_id": "0" * 32}, id="wrong-batch-id"),
            pytest.param(lambda b: {**_progress_member(b), "batch_id": 12345}, id="wrong-batch-id-type"),
            pytest.param(lambda b: {**_progress_member(b), "index": 99}, id="wrong-index"),
            pytest.param(lambda b: {**_progress_member(b), "index": True}, id="index-bool"),
            pytest.param(lambda b: {**_progress_member(b), "total": 99}, id="wrong-total"),
            pytest.param(lambda b: {**_progress_member(b), "total": True}, id="total-bool"),
            pytest.param(lambda b: {**_progress_member(b), "phase": "started"}, id="wrong-phase"),
            pytest.param(
                lambda b: {"batch_id": b.get("id"), "index": b.get("index"), "total": b.get("total")},
                id="missing-phase",
            ),
        ],
    )
    def test_progress_validation_matrix(self, tmp_path, make_progress):
        """Malformed/mismatched ``progress`` response members raise TsDevError."""
        broker = MagicMock()

        def _call(request: dict) -> dict:
            resp = {"op": "delete", "type": "strategy", "name": "A", "success": True, "exit_code": 0}
            progress = make_progress(request.get("batch") or {})
            if progress is not TestServeBatchOnProgress._OMIT:
                resp["progress"] = progress
            return resp

        broker.call.side_effect = _call
        c = _serve_client(tmp_path, broker)
        with pytest.raises(TsDevError):
            c.batch_delete([BatchItem("strategy", "A")])

    @pytest.mark.parametrize(
        "response",
        [
            pytest.param([{"op": "delete", "type": "strategy", "name": "A", "success": True, "exit_code": 0}], id="top-level-list"),
            pytest.param("completed", id="top-level-string"),
            pytest.param(7, id="top-level-int"),
        ],
    )
    def test_response_non_object_raises(self, tmp_path, response):
        """A non-dict top-level broker response raises TsDevError, not AttributeError."""
        broker = MagicMock()
        broker.call.return_value = response
        c = _serve_client(tmp_path, broker)
        with pytest.raises(TsDevError):
            c.batch_delete([BatchItem("strategy", "A")])

    def test_empty_batch_returns_empty_no_callback(self, tmp_path):
        broker = MagicMock()
        c = _serve_client(tmp_path, broker)
        seen: list = []
        results = c.batch_insert([], on_progress=seen.append)
        assert results == []
        assert seen == []
        broker.call.assert_not_called()

    def test_prevalidation_runs_before_broker_and_callback(self, tmp_path):
        broker = MagicMock()
        c = _serve_client(tmp_path, broker)
        seen: list = []
        with pytest.raises(ValueError, match="unknown component type"):
            c.batch_insert([BatchItem("bogus", "A", "// a")], on_progress=seen.append)
        broker.call.assert_not_called()
        assert seen == []
        with pytest.raises(ValueError, match="source is required"):
            c.batch_insert([BatchItem("strategy", "A", None)], on_progress=seen.append)
        broker.call.assert_not_called()
        assert seen == []

    def test_from_json_tolerates_progress_member(self):
        """A response carrying ``progress`` parses as a normal TsDevResult."""
        obj = json.loads(_success_json("delete", "strategy", "A"))
        obj["progress"] = {"batch_id": "x" * 32, "index": 1, "total": 2, "phase": "completed"}
        r = TsDevResult.from_json(obj)
        assert r.success is True
        assert r.name == "A"


class TestServeBatchDirOnProgress:
    """TVS B serve directory batches: tagged requests + on_progress."""

    _DIR_OPS = [
        pytest.param("batch_insert_dir", "insert", id="insert"),
        pytest.param("batch_modify_dir", "modify", id="modify"),
        pytest.param("batch_delete_dir", "delete", id="delete"),
        pytest.param("batch_upsert_dir", "upsert", id="upsert"),
    ]

    @staticmethod
    def _make_dir(tmp_path: Path, *names: str) -> Path:
        d = tmp_path / "el_dir"
        d.mkdir()
        for n in names:
            stem = Path(n).stem
            (d / n).write_text(f"// {stem} source")
        return d

    @staticmethod
    def _fail_read_of(monkeypatch, filename: str) -> None:
        real_read_text = Path.read_text

        def fake_read_text(self, *args, **kwargs):
            if self.name == filename:
                raise OSError("permission denied")
            return real_read_text(self, *args, **kwargs)

        monkeypatch.setattr(Path, "read_text", fake_read_text)

    @pytest.mark.parametrize("method_name,op", _DIR_OPS)
    def test_dir_ops_tagged_and_callback(self, tmp_path, method_name, op):
        d = self._make_dir(tmp_path, "alpha.el", "beta.el")
        needs_source = op != "delete"
        fake_broker = _serve_batch_broker([
            {"op": op, "type": "strategy", "name": "alpha", "success": True, "exit_code": 0},
            {"op": op, "type": "strategy", "name": "beta", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []
        results = getattr(c, method_name)("strategy", str(d), on_progress=seen.append)
        assert len(results) == 2
        assert all(r.success for r in results)
        assert fake_broker.call.call_count == 2
        batch_id = None
        for i, n in enumerate(("alpha", "beta")):
            req = fake_broker.call.call_args_list[i][0][0]
            rid = _assert_batch_request(
                req, op, "strategy", n, i + 1, 2,
                f"// {n} source" if needs_source else None,
            )
            if batch_id is None:
                batch_id = rid
            else:
                assert rid == batch_id
            assert seen[i].index == i + 1
            assert seen[i].total == 2
            assert seen[i].result == results[i]
        assert len(seen) == 2

    def test_dir_items_sorted_order_with_callbacks(self, tmp_path):
        d = self._make_dir(tmp_path, "zebra.el", "alpha.el", "mango.el")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "alpha", "success": True, "exit_code": 0},
            {"op": "insert", "type": "strategy", "name": "mango", "success": True, "exit_code": 0},
            {"op": "insert", "type": "strategy", "name": "zebra", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []
        results = c.batch_insert_dir("strategy", str(d), on_progress=seen.append)
        assert [p.result.name for p in seen] == ["alpha", "mango", "zebra"]
        assert [p.index for p in seen] == [1, 2, 3]
        assert [p.total for p in seen] == [3, 3, 3]
        assert [r.name for r in results] == ["alpha", "mango", "zebra"]

    def test_dir_read_failure_invokes_callback_and_can_cancel(self, tmp_path, monkeypatch):
        """A local source-read failure is an actual completed result: it
        invokes the callback, and False cancels — remaining items become
        exact not-run markers."""
        d = self._make_dir(tmp_path, "bad.el", "good.el", "last.el")
        broker = MagicMock()
        c = _serve_client(tmp_path, broker)
        self._fail_read_of(monkeypatch, "bad.el")
        seen: list = []

        def cb(p):
            seen.append(p)
            return False

        results = c.batch_insert_dir("strategy", str(d), on_progress=cb)
        assert len(results) == 3
        assert len(seen) == 1
        assert seen[0].index == 1
        assert seen[0].total == 3
        assert seen[0].result.success is False
        assert seen[0].result.error is not None
        assert seen[0].result.error.code == "source_file_error"
        broker.call.assert_not_called()
        _assert_cancelled_marker(results[1], "insert", "strategy", "good")
        _assert_cancelled_marker(results[2], "insert", "strategy", "last")

    def test_dir_read_failure_continues_when_not_cancelled(self, tmp_path, monkeypatch):
        d = self._make_dir(tmp_path, "bad.el", "good.el")
        fake_broker = _serve_batch_broker([
            {"op": "insert", "type": "strategy", "name": "good", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        self._fail_read_of(monkeypatch, "bad.el")
        seen: list = []
        results = c.batch_insert_dir("strategy", str(d), on_progress=seen.append)
        assert len(results) == 2
        assert len(seen) == 2
        assert seen[0].index == 1
        assert seen[0].result.error is not None
        assert seen[0].result.error.code == "source_file_error"
        assert seen[1].index == 2
        assert seen[1].result.success is True
        assert fake_broker.call.call_count == 1
        req = fake_broker.call.call_args[0][0]
        _assert_batch_request(req, "insert", "strategy", "good", 2, 2, "// good source")

    def test_dir_false_cancels_after_broker_item(self, tmp_path):
        d = self._make_dir(tmp_path, "alpha.el", "beta.el", "gamma.el")
        fake_broker = _serve_batch_broker([
            {"op": "delete", "type": "strategy", "name": "alpha", "success": True, "exit_code": 0},
        ])
        c = _serve_client(tmp_path, fake_broker)
        seen: list = []

        def cb(p):
            seen.append(p)
            return False

        results = c.batch_delete_dir("strategy", str(d), on_progress=cb)
        assert fake_broker.call.call_count == 1
        assert len(seen) == 1
        assert len(results) == 3
        assert results[0].success is True
        _assert_cancelled_marker(results[1], "delete", "strategy", "beta")
        _assert_cancelled_marker(results[2], "delete", "strategy", "gamma")


# ---------------------------------------------------------------------------
# TVS C direct-mode on_progress harness (mocked subprocess.Popen)
# ---------------------------------------------------------------------------
#
# The fake below simulates ``tsdev_cli --batch-progress-control`` over
# bounded-buffer in-memory pipes (a kernel-buffer model): stderr carries the
# NDJSON batch events, stdin carries the framed ``TSDEV_BATCH_CONTROL/1``
# manifest followed by per-item ``continue``/``cancel`` decision lines, and
# stdout carries the single final JSON array.  A scripted child thread runs
# the real protocol, so the tests stay strict on wire bytes, decisions,
# concurrency (a controller that never drains stdout/stderr deadlocks just
# like a real full pipe) and cleanup, while remaining tolerant of how the
# production controller splits its own reads/writes across threads.

_BATCH_CONTROL_HEADER = b"TSDEV_BATCH_CONTROL/1 "
_FAKE_PIPE_CAPACITY = 8192
# Bounds pathological deadlocks (a controller that never drains, never
# writes decisions, or never flushes).  Correct runs complete in ms.
_FAKE_STALL_S = 10.0


class _FakePipeCore:
    """Shared state of one bounded-buffer pipe (kernel-buffer model)."""

    def __init__(self, capacity: int = _FAKE_PIPE_CAPACITY) -> None:
        self.cond = threading.Condition()
        self.buf = bytearray()
        self.capacity = capacity
        self.write_eof = False  # writer closed -> readers observe EOF
        self.read_eof = False   # reader closed -> writers observe EPIPE
        self.aborted = False    # terminate()/kill(): pending ops fail fast


class _FakePipeReader(io.BufferedIOBase):
    """Read end of a fake pipe: blocking reads, EOF once the writer closes."""

    def __init__(self, core: _FakePipeCore) -> None:
        super().__init__()
        self._core = core

    def readable(self) -> bool:
        return True

    def writable(self) -> bool:
        return False

    def seekable(self) -> bool:
        return False

    def _take(self, n: int) -> bytes:
        c = self._core
        with c.cond:
            while not c.buf:
                if c.write_eof or c.aborted or c.read_eof:
                    return b""
                c.cond.wait()
            take = len(c.buf) if n is None or n < 0 else min(n, len(c.buf))
            out = bytes(c.buf[:take])
            del c.buf[:take]
            c.cond.notify_all()
            return out

    def readinto(self, b) -> int:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        data = self._take(len(b))
        b[: len(data)] = data
        return len(data)

    def readinto1(self, b) -> int:
        return self.readinto(b)

    def read(self, size: int = -1) -> bytes:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        if size is None or size < 0:
            chunks: list[bytes] = []
            while True:
                chunk = self._take(65536)
                if not chunk:
                    return b"".join(chunks)
                chunks.append(chunk)
        return self._take(size)

    def read1(self, size: int = -1) -> bytes:
        return self.read(size)

    def peek(self, size: int = 1) -> bytes:
        c = self._core
        with c.cond:
            while not c.buf and not (c.write_eof or c.aborted or c.read_eof):
                c.cond.wait()
            return bytes(c.buf[: max(1, size)])

    def readline(self, size: int = -1) -> bytes:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        c = self._core
        out = bytearray()
        with c.cond:
            while True:
                if c.buf:
                    limit = len(c.buf) if size is None or size < 0 else min(len(c.buf), size - len(out))
                    if limit <= 0:
                        return bytes(out)
                    nl = c.buf.find(b"\n", 0, limit)
                    take = nl + 1 if nl >= 0 else limit
                    out += c.buf[:take]
                    del c.buf[:take]
                    c.cond.notify_all()
                    if nl >= 0 or (size is not None and 0 <= size <= len(out)):
                        return bytes(out)
                    continue
                if c.write_eof or c.aborted or c.read_eof:
                    return bytes(out)
                c.cond.wait()

    def close(self) -> None:
        if self.closed:
            return
        c = self._core
        with c.cond:
            c.read_eof = True
            c.cond.notify_all()
        super().close()


class _FakePipeWriter(io.BufferedIOBase):
    """Write end of a fake pipe.

    With ``buffered=True`` this emulates the real ``Popen(stdin=PIPE)``
    ``BufferedWriter``: bytes sit in a pending buffer until ``flush()`` /
    ``close()`` (or the pending buffer fills) — a producer that never
    flushes never delivers, which is the real-pipe contract the controller
    must honor.  With ``buffered=False`` bytes go straight into the pipe
    (the child's own stdout/stderr producers).
    """

    def __init__(
        self,
        core: _FakePipeCore,
        *,
        buffered: bool,
        wire_sink: bytearray | None = None,
    ) -> None:
        super().__init__()
        self._core = core
        self._buffered = buffered
        self._pending = bytearray()
        self._wire = wire_sink
        self.flushes = 0

    def writable(self) -> bool:
        return True

    def readable(self) -> bool:
        return False

    def seekable(self) -> bool:
        return False

    def _push(self, data: bytes) -> None:
        if not data:
            return
        c = self._core
        deadline = time.monotonic() + _FAKE_STALL_S
        with c.cond:
            off = 0
            while off < len(data):
                if c.aborted or c.read_eof:
                    raise BrokenPipeError("fake pipe read end is closed")
                room = c.capacity - len(c.buf)
                if room <= 0:
                    rem = deadline - time.monotonic()
                    if rem <= 0:
                        raise BrokenPipeError("fake pipe write stalled: reader is not draining")
                    c.cond.wait(min(0.2, rem))
                    continue
                n = min(room, len(data) - off)
                chunk = data[off : off + n]
                c.buf += chunk
                if self._wire is not None:
                    self._wire += chunk
                off += n
                c.cond.notify_all()

    def write(self, b) -> int:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        if isinstance(b, str):
            b = b.encode("utf-8")
        data = bytes(b)
        if self._buffered:
            self._pending += data
            if len(self._pending) >= _FAKE_PIPE_CAPACITY:
                self._push(bytes(self._pending))
                self._pending.clear()
        else:
            self._push(data)
        return len(data)

    def flush(self) -> None:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        self.flushes += 1
        if self._pending:
            self._push(bytes(self._pending))
            self._pending.clear()

    def close(self) -> None:
        if self.closed:
            return
        if self._pending:
            try:
                self._push(bytes(self._pending))
            except BrokenPipeError:
                pass
            self._pending.clear()
        c = self._core
        with c.cond:
            c.write_eof = True
            c.cond.notify_all()
        super().close()


def _as_bytes(x) -> bytes:
    return bytes(x) if isinstance(x, (bytes, bytearray)) else str(x).encode("utf-8")


def _auto_item_lines(op: str, index: int, total: int, item: "_CliItem") -> list[str]:
    """Canonical compact NDJSON started/completed event pair (C++ schema)."""
    result = item.result or {}
    started = {
        "event": "batch_item_started", "batch_operation": op,
        "index": index, "total": total, "type": item.type, "name": item.name,
    }
    completed = {
        "event": "batch_item_completed", "batch_operation": op,
        "index": index, "total": total, "type": item.type, "name": item.name,
        "success": result.get("success"), "exit_code": result.get("exit_code"),
        "result": result,
    }
    return [
        json.dumps(started, separators=(",", ":")),
        json.dumps(completed, separators=(",", ":")),
    ]


def _cancelled_marker(op: str, type_: str, name: str) -> dict:
    """The CLI's synthesized not-run array element (exit 12)."""
    return {
        "op": op, "type": type_, "name": name,
        "success": False, "exit_code": 12,
        "error": {
            "code": "cancelled",
            "message": "Batch cancelled before this item was attempted",
        },
    }


def _control_error_line(token: str) -> str:
    """The ``batch_control_error`` diagnostic emitted on channel anomalies."""
    msg = (
        "control channel closed before decision was read"
        if token == "<eof>"
        else f"invalid decision token {token!r}"
    )
    return json.dumps(
        {"event": "batch_control_error", "message": msg, "action": "cancel"},
        separators=(",", ":"),
    )


@dataclass
class _CliItem:
    """One scripted batch item for the fake CLI.

    ``lines=None`` emits the canonical ``batch_item_started`` +
    ``batch_item_completed`` NDJSON pair; an explicit tuple replaces it (for
    malformed-event coverage).  ``extra`` appends raw lines after the item's
    events (interleaved diagnostics).  ``await_decision=None`` follows the
    real protocol: a decision is read only when ``index < total``.
    """

    type: str
    name: str
    result: dict | None = None
    lines: tuple | None = None
    extra: tuple = ()
    await_decision: bool | None = None


@dataclass
class _CliScenario:
    """Script for the fake ``--batch-progress-control`` CLI process."""

    op: str
    items: list
    exit_code: int = 0
    stdout: object = None           # None -> auto JSON array (attempted + markers)
    pre_lines: tuple = ()           # raw stderr lines before any item events
    summary: dict | None = None     # explicit batch_completed object
    summary_lines: tuple | None = None  # raw lines replacing the summary event
    omit_summary: bool = False
    close_failed: bool = False      # rewrite attempted successes to exit-6 failures
    pre_run_stderr: object = None   # pre-run failure: diagnostic + exit, no run
    hang: bool = False              # emit nothing, never exit (until abort/stall)
    crash_after: int | None = None  # emit item events through this index, then die
    manifest: bool | None = None    # None -> infer from "--batch-stdin" in argv


class _FakeCliProc:
    """Thread-safe ``subprocess.Popen`` stand-in running a scripted
    ``tsdev_cli --batch-progress-control`` session on in-memory pipes."""

    def __init__(self, cmd, scenario: _CliScenario) -> None:
        self.args = list(cmd)
        self.pid = 4242
        self.returncode: int | None = None  # set only via wait/poll/communicate
        self.wait_calls = 0
        self.poll_calls = 0
        self.terminated = False
        self.killed = False
        # Observables for the tests.
        self.stdin_wire = bytearray()   # exact bytes the producer flushed
        self.decisions: list[str] = []  # parsed decision tokens in order
        self.manifest_declared: int | None = None
        self.manifest_bytes: bytes | None = None
        self.manifest_error: bytes | None = None
        self.manifest_sources: list[tuple[str, bool]] = []  # (path, existed)
        self.child_error: str | None = None

        self._scenario = scenario
        self._in_core = _FakePipeCore()
        self._out_core = _FakePipeCore()
        self._err_core = _FakePipeCore()
        self.stdin = _FakePipeWriter(self._in_core, buffered=True, wire_sink=self.stdin_wire)
        self.stdout = _FakePipeReader(self._out_core)
        self.stderr = _FakePipeReader(self._err_core)
        self._stdout_w = _FakePipeWriter(self._out_core, buffered=False)
        self._stderr_w = _FakePipeWriter(self._err_core, buffered=False)
        self._abort = threading.Event()
        self._done = threading.Event()
        self._exit_code: int | None = None
        self._term_rc: int | None = None
        self._thread = threading.Thread(target=self._run, name="fake-tsdev-cli", daemon=True)
        self._thread.start()

    # -- Popen surface -----------------------------------------------------

    def wait(self, timeout: float | None = None) -> int:
        self.wait_calls += 1
        if not self._done.wait(timeout):
            raise subprocess.TimeoutExpired(self.args, timeout)
        if self.returncode is None:
            self.returncode = self._exit_code
        return self.returncode

    def poll(self):
        self.poll_calls += 1
        if self._done.is_set() and self.returncode is None:
            self.returncode = self._exit_code
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True
        self._kill(1)

    def kill(self) -> None:
        self.killed = True
        self._kill(1)

    def communicate(self, input=None, timeout=None):
        if input is not None:
            try:
                self.stdin.write(input)
                self.stdin.flush()
                self.stdin.close()
            except (BrokenPipeError, ValueError):
                pass
        drained: dict = {}

        def _drain(name: str, stream) -> None:
            try:
                drained[name] = stream.read()
            except Exception:
                drained[name] = b""

        t_out = threading.Thread(target=_drain, args=("out", self.stdout), daemon=True)
        t_err = threading.Thread(target=_drain, args=("err", self.stderr), daemon=True)
        t_out.start()
        t_err.start()
        self.wait(timeout)
        t_out.join()
        t_err.join()
        return drained.get("out") or b"", drained.get("err") or b""

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        for stream in (self.stdin, self.stdout, self.stderr):
            try:
                stream.close()
            except Exception:
                pass
        try:
            self.wait(_FAKE_STALL_S)
        except Exception:
            pass
        return False

    def _kill(self, rc: int) -> None:
        if self._done.is_set():
            return
        self._term_rc = rc
        self._abort.set()
        for core in (self._in_core, self._out_core, self._err_core):
            with core.cond:
                core.aborted = True
                core.cond.notify_all()
        self._done.wait(_FAKE_STALL_S)

    # -- scripted child (runs on its own thread) ----------------------------

    def _run(self) -> None:
        try:
            self._run_scenario()
        except Exception as e:  # broken pipes, scenario bugs
            self.child_error = f"{type(e).__name__}: {e}"
            if self._exit_code is None:
                self._exit_code = self._term_rc if self._term_rc is not None else 70
        finally:
            for w in (self._stdout_w, self._stderr_w):
                try:
                    w.close()
                except Exception:
                    pass
            with self._in_core.cond:
                self._in_core.read_eof = True
                self._in_core.cond.notify_all()
            if self._exit_code is None:
                self._exit_code = self._term_rc if self._term_rc is not None else 0
            self._done.set()

    def _run_scenario(self) -> None:
        s = self._scenario
        expect_manifest = s.manifest if s.manifest is not None else ("--batch-stdin" in self.args)
        if expect_manifest and not self._read_manifest():
            self._emit_err_line("tsdev_cli: malformed TSDEV_BATCH_CONTROL manifest header")
            self._exit_code = 1
            return
        if s.pre_run_stderr is not None:
            self._emit_err_line(s.pre_run_stderr)
            self._exit_code = s.exit_code
            return
        if s.hang:
            self._abort.wait(_FAKE_STALL_S)
            self._exit_code = self._term_rc if self._term_rc is not None else s.exit_code
            return
        for ln in s.pre_lines:
            self._emit_err_line(ln)
        items = list(s.items)
        total = len(items)
        attempted: list[dict] = []
        cancelled = False
        for i, item in enumerate(items, start=1):
            if self._abort.is_set():
                return
            lines = item.lines if item.lines is not None else _auto_item_lines(s.op, i, total, item)
            for ln in (*lines, *item.extra):
                self._emit_err_line(ln)
            if s.crash_after == i:
                if s.stdout is not None:
                    self._emit_out(s.stdout)
                self._exit_code = s.exit_code
                return
            if isinstance(item.result, dict):
                attempted.append(dict(item.result))
            want = item.await_decision if item.await_decision is not None else i < total
            if want:
                tok = self._read_decision()
                if tok == "continue":
                    continue
                cancelled = True
                if tok != "cancel":
                    self._emit_err_line(_control_error_line(tok))
                break
        # End of batch: close-failure rewrite, not-run markers, summary, array.
        final = attempted
        if s.close_failed:
            for r in final:
                if r.get("success"):
                    r["success"] = False
                    r["exit_code"] = 6
                    r.pop("prog_id", None)
                    r.pop("ell_path", None)
                    r["error"] = {
                        "code": "entry_op_failure",
                        "message": "closeWarehouse failed at end of batch",
                    }
        markers = (
            [_cancelled_marker(s.op, it.type, it.name) for it in items[len(attempted):]]
            if cancelled
            else []
        )
        rc = 3 if cancelled else s.exit_code
        self._emit_summary(s, final, total, len(markers), cancelled, rc)
        payload = (
            s.stdout
            if s.stdout is not None
            else json.dumps(final + markers, separators=(",", ":")) + "\n"
        )
        self._emit_out(payload)
        self._exit_code = rc

    # -- child-side I/O ------------------------------------------------------

    def _emit_err_line(self, ln) -> None:
        data = _as_bytes(ln)
        if data.endswith(b"\n"):
            data = data[:-1]
        self._stderr_w.write(data + b"\n")

    def _emit_out(self, payload) -> None:
        self._stdout_w.write(_as_bytes(payload))

    def _emit_summary(self, s: _CliScenario, final: list, total: int,
                      not_run: int, cancelled: bool, rc: int) -> None:
        if s.omit_summary:
            return
        if s.summary_lines is not None:
            for ln in s.summary_lines:
                self._emit_err_line(ln)
            return
        if s.summary is not None:
            obj = s.summary
        else:
            obj = {
                "event": "batch_completed",
                "batch_operation": s.op,
                "completed": len(final),
                "total": total,
                "successes": sum(1 for r in final if r.get("success")),
                "failures": sum(1 for r in final if not r.get("success")),
                "not_run": not_run,
                "cancelled": cancelled,
                "close_failed": bool(s.close_failed),
                "exit_code": rc,
            }
        self._emit_err_line(json.dumps(obj, separators=(",", ":")))

    def _child_read(self, n: int) -> bytes:
        """Read up to ``n`` stdin bytes (child side), stall-capped."""
        c = self._in_core
        out = bytearray()
        deadline = time.monotonic() + _FAKE_STALL_S
        with c.cond:
            while len(out) < n:
                if c.buf:
                    take = min(n - len(out), len(c.buf))
                    out += c.buf[:take]
                    del c.buf[:take]
                    c.cond.notify_all()
                    continue
                if c.write_eof or c.aborted or c.read_eof:
                    break
                rem = deadline - time.monotonic()
                if rem <= 0:
                    break
                c.cond.wait(min(0.2, rem))
        return bytes(out)

    def _child_readline(self) -> bytes:
        """Read one stdin line (child side); ``b""`` on EOF/stall."""
        c = self._in_core
        out = bytearray()
        deadline = time.monotonic() + _FAKE_STALL_S
        with c.cond:
            while True:
                if c.buf:
                    nl = c.buf.find(b"\n")
                    take = len(c.buf) if nl < 0 else nl + 1
                    out += c.buf[:take]
                    del c.buf[:take]
                    c.cond.notify_all()
                    if nl >= 0:
                        return bytes(out)
                    continue
                if c.write_eof or c.aborted or c.read_eof:
                    return bytes(out)
                rem = deadline - time.monotonic()
                if rem <= 0:
                    return bytes(out)
                c.cond.wait(min(0.2, rem))

    def _read_manifest(self) -> bool:
        """Consume the framed ``TSDEV_BATCH_CONTROL/1 <len>`` manifest."""
        line = self._child_readline()
        if not line.startswith(_BATCH_CONTROL_HEADER):
            self.manifest_error = line or b"<eof>"
            return False
        try:
            n = int(line[len(_BATCH_CONTROL_HEADER):].strip())
        except ValueError:
            self.manifest_error = line
            return False
        self.manifest_declared = n
        self.manifest_bytes = self._child_read(n)
        if len(self.manifest_bytes) != n:
            return False
        text = self.manifest_bytes.decode("utf-8", "replace")
        for ln in text.splitlines():
            parts = ln.split(None, 2)
            if len(parts) == 3:
                self.manifest_sources.append((parts[2], os.path.isfile(parts[2])))
        return True

    def _read_decision(self) -> str:
        """Read one decision line; EOF/stall records the ``<eof>`` anomaly."""
        line = self._child_readline()
        tok = line.rstrip(b"\r\n")
        if not line:
            tok = b"<eof>"
        text = tok.decode("utf-8", "replace")
        self.decisions.append(text)
        return text


def _install_fake_popen(monkeypatch, scenario: _CliScenario) -> list[_FakeCliProc]:
    """Patch ``subprocess.Popen`` to a scripted fake; ``subprocess.run`` is
    forbidden on the ``on_progress`` path.  Returns the spawned-proc list."""
    procs: list[_FakeCliProc] = []

    def factory(cmd, *args, **kwargs):
        proc = _FakeCliProc(list(cmd), scenario)
        procs.append(proc)
        return proc

    monkeypatch.setattr(subprocess, "Popen", factory)
    monkeypatch.setattr(
        subprocess,
        "run",
        MagicMock(
            side_effect=AssertionError(
                "subprocess.run must not be used when on_progress is supplied"
            )
        ),
    )
    return procs


def _assert_proc_clean(proc: _FakeCliProc) -> None:
    """End-of-call hygiene: pipes closed, child reaped, helper thread done."""
    assert proc._done.is_set(), "fake child thread must have exited"
    assert proc.returncode is not None, "child must be reaped via wait/poll/communicate"
    assert proc.stdin.closed, "stdin pipe must be closed after the call"
    assert proc.stdout.closed, "stdout pipe must be closed after the call"
    assert proc.stderr.closed, "stderr pipe must be closed after the call"


class TestDirectBatchOnProgress:
    """TVS C: direct-mode ``on_progress`` drives ``tsdev_cli`` over a
    ``subprocess.Popen`` control channel.

    Final contract replacing the transient TVS B ``NotImplementedError``
    guard: the CLI runs with ``--json`` + ``--batch-progress-control``;
    manifest batches stream a framed ``TSDEV_BATCH_CONTROL/1`` header +
    manifest bytes over stdin (kept open for per-item ``continue`` /
    ``cancel`` decisions, no temp manifest); directory batches keep the
    native ``batch-*-dir`` argv with stdin carrying decisions only.
    ``batch_item_completed`` NDJSON events on stderr drive synchronous
    callbacks; the final stdout JSON array — exit-12 markers and the
    close-failure rewrite included — is the authoritative return value.
    """

    _ITEM_OPS = [
        pytest.param("batch_insert", "insert", True, id="insert"),
        pytest.param("batch_modify", "modify", True, id="modify"),
        pytest.param("batch_delete", "delete", False, id="delete"),
        pytest.param("batch_upsert", "upsert", True, id="upsert"),
    ]
    _DIR_OPS = [
        pytest.param("batch_insert_dir", "insert", id="insert_dir"),
        pytest.param("batch_modify_dir", "modify", id="modify_dir"),
        pytest.param("batch_delete_dir", "delete", id="delete_dir"),
        pytest.param("batch_upsert_dir", "upsert", id="upsert_dir"),
    ]

    @pytest.mark.parametrize("method_name,op,needs_source", _ITEM_OPS)
    def test_manifest_batch_control_wire_and_callbacks(
        self, client: TsDevClient, monkeypatch, tmp_path: Path, method_name, op, needs_source
    ) -> None:
        src_a = tmp_path / "a.el"
        src_a.write_text("// a", encoding="utf-8")
        src_b = tmp_path / "b.el"
        src_b.write_text("// b", encoding="utf-8")
        items = [
            BatchItem("strategy", "A", str(src_a) if needs_source else None),
            BatchItem("function", "B", str(src_b) if needs_source else None),
        ]
        res_a = json.loads(_success_json(op, "strategy", "A", prog_id=11))
        res_b = json.loads(_success_json(op, "function", "B", prog_id=22))
        scenario = _CliScenario(
            op=op,
            items=[_CliItem("strategy", "A", res_a), _CliItem("function", "B", res_b)],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []
        results = getattr(client, method_name)(items, on_progress=seen.append)
        assert len(procs) == 1, "direct on_progress must launch exactly one Popen child"
        proc = procs[0]
        cmd = proc.args
        assert cmd[0] == str(client.cli_path)
        assert "--json" in cmd
        assert f"batch-{op}" in cmd
        assert "--batch-stdin" in cmd
        assert cmd.count("--batch-progress-control") == 1
        assert not any(str(c).endswith(".manifest") for c in cmd), "no temp manifest file"
        # Exact wire bytes: control header + UTF-8 manifest + one decision.
        # delete manifests carry no source path (2-token lines), matching the
        # no-callback contract ("delete manifest must not reference source
        # files"); source ops reference the materialized .el paths.
        manifest = (
            f"strategy A {src_a}\nfunction B {src_b}\n".encode("utf-8")
            if needs_source
            else b"strategy A\nfunction B\n"
        )
        expected = _BATCH_CONTROL_HEADER + str(len(manifest)).encode() + b"\n" + manifest + b"continue\n"
        assert bytes(proc.stdin_wire) == expected
        assert proc.manifest_declared == len(manifest)
        assert proc.manifest_bytes == manifest
        assert proc.decisions == ["continue"], "one continue decision (index<total only)"
        assert proc.stdin.flushes >= 1, "manifest/decisions must be flushed to the child"
        assert len(seen) == 2
        for i, (p, r) in enumerate(zip(seen, results), start=1):
            assert isinstance(p, BatchProgress)
            assert p.index == i and p.total == 2
            assert p.result.success == r.success
            assert p.result.name == r.name
            assert p.result.exit_code == r.exit_code
        assert [r.name for r in results] == ["A", "B"]
        assert results[0].prog_id == 11 and results[1].prog_id == 22
        _assert_proc_clean(proc)

    @pytest.mark.parametrize("method_name,op", _DIR_OPS)
    def test_dir_batch_control_decisions_only_stdin(
        self, client: TsDevClient, monkeypatch, tmp_path: Path, method_name, op
    ) -> None:
        d = tmp_path / "el_dir"
        d.mkdir()
        (d / "a.el").write_text("// a", encoding="utf-8")
        (d / "b.el").write_text("// b", encoding="utf-8")
        res_a = json.loads(_success_json(op, "strategy", "a", prog_id=1))
        res_b = json.loads(_success_json(op, "strategy", "b", prog_id=2))
        scenario = _CliScenario(
            op=op,
            items=[_CliItem("strategy", "a", res_a), _CliItem("strategy", "b", res_b)],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []
        results = getattr(client, method_name)(
            "strategy", str(d), extensions=["el"], on_progress=seen.append
        )
        assert len(procs) == 1
        proc = procs[0]
        cmd = proc.args
        assert f"batch-{op}-dir" in cmd
        assert "strategy" in cmd and str(d) in cmd
        assert "-t" in cmd and "el" in cmd, "native -t extension args preserved"
        assert "--json" in cmd
        assert cmd.count("--batch-progress-control") == 1
        assert "--batch-stdin" not in cmd, "dir batches never use stdin manifest framing"
        assert proc.manifest_bytes is None
        # stdin carries decision lines only — no manifest framing.
        assert bytes(proc.stdin_wire) == b"continue\n"
        assert proc.decisions == ["continue"]
        assert [p.index for p in seen] == [1, 2]
        assert [p.total for p in seen] == [2, 2]
        assert [r.name for r in results] == ["a", "b"]
        _assert_proc_clean(proc)

    def test_callback_true_none_and_zero_continue(self, client: TsDevClient, monkeypatch) -> None:
        """Only ``return is False`` cancels: True/None/0 all write continue."""
        scenario = _CliScenario(
            op="delete",
            items=[
                _CliItem("strategy", n, json.loads(_success_json("delete", "strategy", n)))
                for n in ("A", "B", "C")
            ],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        returns = iter([True, None, 0])
        results = client.batch_delete(
            [BatchItem("strategy", n) for n in ("A", "B", "C")],
            on_progress=lambda p: next(returns),
        )
        assert procs[0].decisions == ["continue", "continue"]
        assert [r.success for r in results] == [True, True, True]
        _assert_proc_clean(procs[0])

    def test_false_cancels_after_success_full_markers(self, client: TsDevClient, monkeypatch) -> None:
        scenario = _CliScenario(
            op="delete",
            items=[
                _CliItem("strategy", "A", json.loads(_success_json("delete", "strategy", "A"))),
                _CliItem("function", "B", json.loads(_success_json("delete", "function", "B"))),
                _CliItem("indicator", "C", json.loads(_success_json("delete", "indicator", "C"))),
            ],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []

        def cb(p):
            seen.append(p)
            return False

        results = client.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("function", "B"), BatchItem("indicator", "C")],
            on_progress=cb,
        )
        proc = procs[0]
        assert proc.decisions == ["cancel"]
        assert len(seen) == 1, "callback must not be invoked for not-run markers"
        assert len(results) == 3, "final array is full length on cancel"
        assert results[0].success is True
        for r, (t, n) in zip(results[1:], [("function", "B"), ("indicator", "C")]):
            assert r.success is False
            assert r.exit_code == 12
            assert r.op == "delete" and r.type == t and r.name == n
            assert r.error is not None and r.error.code == "cancelled"
            assert r.error.message == "Batch cancelled before this item was attempted"
        assert proc.returncode == 3, "cooperative cancel exits 3"
        _assert_proc_clean(proc)

    def test_false_cancels_after_failure_full_markers(self, client: TsDevClient, monkeypatch) -> None:
        res_a = json.loads(_failure_json("delete", "strategy", "A", "not_found", "missing", exit_code=5))
        res_b = json.loads(_success_json("delete", "function", "B"))
        scenario = _CliScenario(
            op="delete",
            items=[_CliItem("strategy", "A", res_a), _CliItem("function", "B", res_b)],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []

        def cb(p):
            seen.append(p)
            return False

        results = client.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("function", "B")], on_progress=cb
        )
        proc = procs[0]
        assert proc.decisions == ["cancel"]
        assert len(seen) == 1
        assert len(results) == 2
        assert results[0].success is False
        assert results[0].error is not None and results[0].error.code == "not_found"
        assert results[1].exit_code == 12 and results[1].error.code == "cancelled"
        assert proc.returncode == 3
        _assert_proc_clean(proc)

    def test_final_item_false_writes_no_decision(self, client: TsDevClient, monkeypatch) -> None:
        """``False`` on the final item is a no-op: no decision is requested
        (``index < total`` guard) so none may be written."""
        scenario = _CliScenario(
            op="delete",
            items=[
                _CliItem("strategy", n, json.loads(_success_json("delete", "strategy", n)))
                for n in ("A", "B", "C")
            ],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        results = client.batch_delete(
            [BatchItem("strategy", n) for n in ("A", "B", "C")],
            on_progress=lambda p: p.index < p.total,
        )
        proc = procs[0]
        assert proc.decisions == ["continue", "continue"]
        wire = bytes(proc.stdin_wire)
        assert wire.count(b"continue\n") == 2
        assert b"cancel" not in wire, "no decision may be written after the final item"
        assert [r.success for r in results] == [True, True, True]
        _assert_proc_clean(proc)

    def test_callback_exception_sends_cancel_then_reraises(self, client: TsDevClient, monkeypatch) -> None:
        scenario = _CliScenario(
            op="delete",
            items=[
                _CliItem("strategy", n, json.loads(_success_json("delete", "strategy", n)))
                for n in ("A", "B", "C")
            ],
        )
        procs = _install_fake_popen(monkeypatch, scenario)

        class Boom(Exception):
            pass

        def cb(p):
            raise Boom("progress callback failed")

        with pytest.raises(Boom, match="progress callback failed"):
            client.batch_delete(
                [BatchItem("strategy", n) for n in ("A", "B", "C")], on_progress=cb
            )
        proc = procs[0]
        assert proc.decisions == ["cancel"], "exception path must write cancel before re-raising"
        assert proc._done.is_set(), "child must be drained/waited to a safe close"
        assert proc.returncode == 3
        _assert_proc_clean(proc)

    def test_framed_manifest_declares_utf8_byte_length(
        self, client: TsDevClient, monkeypatch, tmp_path: Path
    ) -> None:
        src = tmp_path / "s.el"
        src.write_text("// s", encoding="utf-8")
        name = "名Café"  # multi-byte UTF-8: byte length != char length
        scenario = _CliScenario(
            op="insert",
            items=[_CliItem("strategy", name, json.loads(_success_json("insert", "strategy", name)))],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        results = client.batch_insert(
            [BatchItem("strategy", name, str(src))], on_progress=lambda p: None
        )
        proc = procs[0]
        manifest_text = f"strategy {name} {src}\n"
        manifest = manifest_text.encode("utf-8")
        assert len(manifest) > len(manifest_text), "fixture must exercise multi-byte chars"
        assert proc.manifest_declared == len(manifest), "header declares the UTF-8 byte length"
        assert proc.manifest_bytes == manifest
        expected = _BATCH_CONTROL_HEADER + str(len(manifest)).encode() + b"\n" + manifest
        assert bytes(proc.stdin_wire) == expected
        assert results[0].success is True
        _assert_proc_clean(proc)

    def test_string_sources_materialized_and_cleaned(self, client: TsDevClient, monkeypatch) -> None:
        """String sources still materialize to temp ``.el`` files referenced by
        the framed manifest (the manifest itself is never a temp file); the
        temp sources are cleaned up after the call."""
        scenario = _CliScenario(
            op="insert",
            items=[_CliItem("strategy", "A", json.loads(_success_json("insert", "strategy", "A")))],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        results = client.batch_insert(
            [BatchItem("strategy", "A", "input(close,1);")], on_progress=lambda p: None
        )
        assert results[0].success is True
        proc = procs[0]
        cmd = proc.args
        assert "--batch-stdin" in cmd
        assert not any(str(c).endswith(".manifest") for c in cmd)
        assert len(proc.manifest_sources) == 1
        src_path, existed = proc.manifest_sources[0]
        assert src_path.endswith(".el")
        assert existed, "materialized temp source must exist while the CLI runs"
        assert not os.path.isfile(src_path), "temp source must be cleaned up"
        _assert_proc_clean(proc)

    def test_final_array_authoritative_over_events(self, client: TsDevClient, monkeypatch) -> None:
        """The event-time per-item result may be superseded by the end-of-batch
        close-failure rewrite; the final stdout array is authoritative."""
        res = json.loads(_success_json("insert", "strategy", "A", prog_id=5))
        scenario = _CliScenario(
            op="insert",
            items=[_CliItem("strategy", "A", res)],
            close_failed=True,
            exit_code=2,
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []
        results = client.batch_insert(
            [BatchItem("strategy", "A", "x;")], on_progress=seen.append
        )
        assert len(seen) == 1
        assert seen[0].result.success is True, "event-time view precedes the rewrite"
        assert results[0].success is False, "the final array is authoritative"
        assert results[0].exit_code == 6
        assert results[0].error is not None and results[0].error.code == "entry_op_failure"
        _assert_proc_clean(procs[0])

    def test_interleaved_diagnostics_tolerated(self, client: TsDevClient, monkeypatch) -> None:
        """Non-JSON stderr lines and unrecognized JSON objects are diagnostics,
        not events: they never drive callbacks and never abort the batch."""
        res_a = json.loads(_success_json("delete", "strategy", "A"))
        res_b = json.loads(_success_json("delete", "function", "B"))
        scenario = _CliScenario(
            op="delete",
            items=[
                _CliItem("strategy", "A", res_a, extra=("tsdev_cli: mid-batch diagnostic",)),
                _CliItem("function", "B", res_b),
            ],
            pre_lines=(
                "orlib20: loading warehouse...",
                '{"event":"telemetry","note":"not a batch event"}',
                "[1,2,3]",
                "not json at all {",
            ),
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []
        results = client.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("function", "B")], on_progress=seen.append
        )
        assert len(seen) == 2, "only batch_item_completed events drive callbacks"
        assert [p.index for p in seen] == [1, 2]
        assert [r.success for r in results] == [True, True]
        # stderr diagnostics are retained on the results.
        assert "orlib20: loading warehouse" in results[0].stderr
        assert "mid-batch diagnostic" in results[0].stderr
        _assert_proc_clean(procs[0])

    def test_batch_control_error_retained_as_diagnostic(
        self, client: TsDevClient, monkeypatch
    ) -> None:
        """A ``batch_control_error`` line from the child is a retained
        stderr diagnostic, not a protocol violation — the run still
        completes and returns the authoritative array."""
        res_a = json.loads(_success_json("delete", "strategy", "A"))
        control_err = json.dumps(
            {"event": "batch_control_error",
             "message": "control channel hiccup", "action": "cancel"},
            separators=(",", ":"),
        )
        scenario = _CliScenario(
            op="delete",
            items=[_CliItem("strategy", "A", res_a, extra=(control_err,))],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        results = client.batch_delete(
            [BatchItem("strategy", "A")], on_progress=lambda p: None
        )
        assert results[0].success is True
        assert "batch_control_error" in results[0].stderr
        assert "control channel hiccup" in results[0].stderr
        _assert_proc_clean(procs[0])

    @staticmethod
    def _malformed_scenario(case: str) -> _CliScenario:
        op = "delete"

        def res(name):
            return json.loads(_success_json(op, "strategy", name))

        def started(i, total, name):
            return json.dumps(
                {"event": "batch_item_started", "batch_operation": op,
                 "index": i, "total": total, "type": "strategy", "name": name},
                separators=(",", ":"),
            )

        def completed(i, total, name, result, bop=op, include_result=True):
            ev = {
                "event": "batch_item_completed", "batch_operation": bop,
                "index": i, "total": total, "type": "strategy", "name": name,
                "success": result["success"], "exit_code": result["exit_code"],
            }
            if include_result:
                ev["result"] = result
            return json.dumps(ev, separators=(",", ":"))

        def items(item1):
            return _CliScenario(op=op, items=[item1, _CliItem("strategy", "B", res("B"))])

        if case == "completed_wrong_index":
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(started(1, 2, "A"), completed(2, 2, "A", res("A")))))
        if case == "completed_wrong_total":
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(started(1, 2, "A"), completed(1, 3, "A", res("A")))))
        if case == "completed_wrong_op":
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(started(1, 2, "A"), completed(1, 2, "A", res("A"), bop="insert"))))
        if case == "completed_wrong_name":
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(started(1, 2, "A"), completed(1, 2, "ZZZ", res("A")))))
        if case == "completed_wrong_type":
            ev = json.dumps(
                {"event": "batch_item_completed", "batch_operation": op,
                 "index": 1, "total": 2, "type": "function", "name": "A",
                 "success": True, "exit_code": 0, "result": res("A")},
                separators=(",", ":"),
            )
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(started(1, 2, "A"), ev)))
        if case == "completed_result_missing":
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(started(1, 2, "A"), completed(1, 2, "A", res("A"), include_result=False))))
        if case == "completed_result_not_object":
            ev = json.dumps(
                {"event": "batch_item_completed", "batch_operation": op,
                 "index": 1, "total": 2, "type": "strategy", "name": "A",
                 "success": True, "exit_code": 0, "result": "oops"},
                separators=(",", ":"),
            )
            return items(_CliItem("strategy", "A", res("A"), lines=(started(1, 2, "A"), ev)))
        if case == "completed_before_started":
            return items(_CliItem("strategy", "A", res("A"),
                                  lines=(completed(1, 2, "A", res("A")),)))
        if case == "missing_completion":
            return _CliScenario(op=op, items=[
                _CliItem("strategy", "A", res("A"), lines=(started(1, 1, "A"),))
            ])
        if case == "missing_summary":
            s = items(_CliItem("strategy", "A", res("A")))
            s.omit_summary = True
            return s
        if case == "summary_wrong_total":
            s = items(_CliItem("strategy", "A", res("A")))
            s.summary = {
                "event": "batch_completed", "batch_operation": op,
                "completed": 2, "total": 99, "successes": 2, "failures": 0,
                "not_run": 0, "cancelled": False, "close_failed": False, "exit_code": 0,
            }
            return s
        if case == "summary_wrong_completed":
            s = items(_CliItem("strategy", "A", res("A")))
            s.summary = {
                "event": "batch_completed", "batch_operation": op,
                "completed": 99, "total": 2, "successes": 2, "failures": 0,
                "not_run": 0, "cancelled": False, "close_failed": False, "exit_code": 0,
            }
            return s
        raise AssertionError(f"unknown malformed case {case!r}")

    @pytest.mark.parametrize(
        "case",
        [
            "completed_wrong_index",
            "completed_wrong_total",
            "completed_wrong_op",
            "completed_wrong_name",
            "completed_wrong_type",
            "completed_result_missing",
            "completed_result_not_object",
            "completed_before_started",
            "missing_completion",
            "missing_summary",
            "summary_wrong_total",
            "summary_wrong_completed",
        ],
    )
    def test_malformed_events_raise_tsdev_error(self, client: TsDevClient, monkeypatch, case: str) -> None:
        """Event order/index/total/op/type/name/nested-result validation:
        malformed, mismatched, or missing completion/summary -> TsDevError."""
        scenario = self._malformed_scenario(case)
        procs = _install_fake_popen(monkeypatch, scenario)
        items = [BatchItem(it.type, it.name) for it in scenario.items]
        with pytest.raises(TsDevError):
            client.batch_delete(items, on_progress=lambda p: None)
        _assert_proc_clean(procs[0])

    def test_dir_malformed_event_raises_tsdev_error(
        self, client: TsDevClient, monkeypatch, tmp_path: Path
    ) -> None:
        d = tmp_path / "el_dir"
        d.mkdir()
        (d / "a.el").write_text("// a", encoding="utf-8")
        op = "delete"
        res_a = json.loads(_success_json(op, "strategy", "a"))
        good_started = json.dumps(
            {"event": "batch_item_started", "batch_operation": op,
             "index": 1, "total": 1, "type": "strategy", "name": "a"},
            separators=(",", ":"),
        )
        bad_completed = json.dumps(
            {"event": "batch_item_completed", "batch_operation": op,
             "index": 2, "total": 1, "type": "strategy", "name": "a",
             "success": True, "exit_code": 0, "result": res_a},
            separators=(",", ":"),
        )
        scenario = _CliScenario(
            op=op,
            items=[_CliItem("strategy", "a", res_a,
                            lines=(good_started, bad_completed), await_decision=False)],
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        with pytest.raises(TsDevError):
            client.batch_delete_dir("strategy", str(d), on_progress=lambda p: None)
        _assert_proc_clean(procs[0])

    def test_timeout_maps_to_timeout_error(self, fake_cli: Path, monkeypatch) -> None:
        """A non-responsive child is terminated and waited on, then mapped to
        ``TimeoutError`` — same contract as the ``subprocess.run`` path."""
        scenario = _CliScenario(op="delete", items=[], hang=True)
        procs = _install_fake_popen(monkeypatch, scenario)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, timeout=0.5)
        c._is_elevated = True
        with pytest.raises(TimeoutError):
            c.batch_delete([BatchItem("strategy", "A")], on_progress=lambda p: None)
        proc = procs[0]
        assert proc.terminated or proc.killed, "timeout must terminate the child"
        assert proc.returncode is not None, "child must be reaped after terminate"
        _assert_proc_clean(proc)

    @pytest.mark.parametrize(
        "exit_code,crash_name",
        [(-1073741819, "ACCESS_VIOLATION"), (0xC00000FD, "STACK_OVERFLOW")],
        ids=["access_violation_signed", "stack_overflow"],
    )
    def test_crash_exit_maps_to_crash_error(
        self, client: TsDevClient, monkeypatch, exit_code, crash_name
    ) -> None:
        """An NT-status crash exit mid-run maps through ``_is_crash_exit`` to
        ``TsDevCrashError`` — same contract as the ``subprocess.run`` path."""
        res_a = json.loads(_success_json("delete", "strategy", "A"))
        scenario = _CliScenario(
            op="delete",
            items=[_CliItem("strategy", "A", res_a)],
            crash_after=1,
            exit_code=exit_code,
            stdout=b"",
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        with pytest.raises(TsDevCrashError) as ei:
            client.batch_delete([BatchItem("strategy", "A")], on_progress=lambda p: None)
        assert ei.value.exit_code == exit_code
        assert crash_name in str(ei.value)
        _assert_proc_clean(procs[0])

    @pytest.mark.parametrize("exit_code,fail_b", [(0, False), (2, True)], ids=["clean", "with_failures"])
    def test_run_exit_codes_accepted(
        self, client: TsDevClient, monkeypatch, exit_code, fail_b
    ) -> None:
        """Ran-to-completion exits 0/2 are accepted; the array is returned."""
        res_a = json.loads(_success_json("delete", "strategy", "A"))
        res_b = (
            json.loads(_failure_json("delete", "strategy", "B", "not_found", "missing", exit_code=5))
            if fail_b
            else json.loads(_success_json("delete", "strategy", "B"))
        )
        scenario = _CliScenario(
            op="delete",
            items=[_CliItem("strategy", "A", res_a), _CliItem("strategy", "B", res_b)],
            exit_code=exit_code,
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        results = client.batch_delete(
            [BatchItem("strategy", "A"), BatchItem("strategy", "B")],
            on_progress=lambda p: None,
        )
        proc = procs[0]
        assert proc.returncode == exit_code
        assert len(results) == 2
        assert results[0].success is True
        assert results[1].success is (not fail_b)
        _assert_proc_clean(proc)

    def test_prerun_exit1_raises_tsdev_error(self, client: TsDevClient, monkeypatch) -> None:
        """A pre-run failure (usage/manifest, exit 1) emits no events and maps
        to ``TsDevError`` — same contract as the ``subprocess.run`` path."""
        scenario = _CliScenario(
            op="delete", items=[], exit_code=1,
            pre_run_stderr="tsdev_cli: usage error",
        )
        procs = _install_fake_popen(monkeypatch, scenario)
        seen: list = []
        with pytest.raises(TsDevError):
            client.batch_delete([BatchItem("strategy", "A")], on_progress=seen.append)
        assert seen == []
        _assert_proc_clean(procs[0])

    def test_stdout_drained_concurrently_with_stderr(self, fake_cli: Path, monkeypatch) -> None:
        """A final array far larger than the pipe buffer forces interleaved
        draining: a controller that waits for the child to exit (or for stderr
        EOF) before reading stdout deadlocks exactly like a real full pipe."""
        big = "x" * (256 * 1024)
        res = {
            "op": "delete", "type": "strategy", "name": "A",
            "success": True, "exit_code": 0, "ell_path": big,
        }
        scenario = _CliScenario(op="delete", items=[_CliItem("strategy", "A", res)])
        procs = _install_fake_popen(monkeypatch, scenario)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False, timeout=30)
        c._is_elevated = True
        results = c.batch_delete([BatchItem("strategy", "A")], on_progress=lambda p: None)
        assert results[0].success is True
        assert results[0].ell_path == big
        _assert_proc_clean(procs[0])

    def test_non_elevated_direct_raises_before_popen(self, fake_cli: Path, monkeypatch) -> None:
        """The existing elevation preflight runs before any Popen launch:
        ``use_serve=False`` + non-elevated -> ``ElevationRequiredError``."""
        monkeypatch.setattr("tsdev_automation.client._detect_elevation", lambda: False)
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        assert c._serve_active() is False
        procs = _install_fake_popen(monkeypatch, _CliScenario(op="delete", items=[]))
        with pytest.raises(exc.ElevationRequiredError):
            c.batch_delete([BatchItem("strategy", "A")], on_progress=lambda p: None)
        assert procs == [], "no Popen may be spawned before the elevation preflight"
        with pytest.raises(exc.ElevationRequiredError):
            c.batch_delete_dir("strategy", "C:/mydir", on_progress=lambda p: None)
        assert procs == []
        assert c._broker is None


class TestElevationGuard:
    """Tests for the ``use_serve=False`` / non-elevated hard-error guard.

    When ``use_serve=False`` and the Python session is NOT elevated, the
    wrapper raises :class:`ElevationRequiredError` *before* attempting the
    subprocess call. The default CLI build's ``requireAdministrator``
    manifest is otherwise subject to normal Windows UAC/manifest handling;
    a non-elevated direct launch that cannot be elevated may fail at process
    creation with ``ERROR_ELEVATION_REQUIRED`` (Windows error 740). An
    ``asInvoker`` build reaches the CLI's runtime guard instead and reports
    exit 11 (``not_elevated``).
    """

    def test_single_op_raises_when_not_elevated_and_use_serve_false(
        self, fake_cli: Path, monkeypatch
    ) -> None:
        """A single-item op raises ElevationRequiredError before subprocess.run."""
        # Mock _detect_elevation to report a non-elevated session.
        monkeypatch.setattr(
            "tsdev_automation.client._detect_elevation", lambda: False
        )
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        assert c._is_elevated is False
        assert c._use_serve is False

        # subprocess.run must never be called — fail loudly if it is.
        def _no_run(*a, **k):
            raise AssertionError("subprocess.run must not be called when not elevated")

        monkeypatch.setattr(subprocess, "run", _no_run)

        with pytest.raises(exc.ElevationRequiredError) as ei:
            c.insert("strategy", "X", "x;")
        # Actionable message and stable error code / exit code.
        assert "use_serve=True" in str(ei.value)
        assert ei.value.code == "elevation_required"
        assert ei.value.exit_code == 12

    def test_batch_op_raises_when_not_elevated_and_use_serve_false(
        self, fake_cli: Path, monkeypatch
    ) -> None:
        """A batch op also raises ElevationRequiredError before subprocess.run."""
        monkeypatch.setattr(
            "tsdev_automation.client._detect_elevation", lambda: False
        )
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)

        def _no_run(*a, **k):
            raise AssertionError("subprocess.run must not be called when not elevated")

        monkeypatch.setattr(subprocess, "run", _no_run)

        with pytest.raises(exc.ElevationRequiredError):
            c.batch_delete([BatchItem("strategy", "A")])

    def test_no_raise_when_elevated_and_use_serve_false(
        self, fake_cli: Path, monkeypatch
    ) -> None:
        """When elevated, use_serve=False does NOT trip the guard (subprocess works)."""
        monkeypatch.setattr(
            "tsdev_automation.client._detect_elevation", lambda: True
        )
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        assert c._is_elevated is True

        monkeypatch.setattr(
            subprocess, "run", lambda *a, **k: _make_completed(_success_json("delete", "strategy", "X"))
        )
        r = c.delete("strategy", "X")
        assert r.success

    def test_no_raise_when_use_serve_true_and_not_elevated(
        self, fake_cli: Path, monkeypatch
    ) -> None:
        """When use_serve=True (default) and not elevated, the broker path is
        taken — the elevation guard in _run_cli is never reached."""
        monkeypatch.setattr(
            "tsdev_automation.client._detect_elevation", lambda: False
        )
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        assert c._serve_active() is True

        # Inject a fake broker so no real UAC / pipe is touched.
        fake_broker = MagicMock()
        fake_broker.call.return_value = json.loads(_success_json("insert", "strategy", "X"))
        c._broker = fake_broker

        r = c.insert("strategy", "X", "input(close,1);")
        assert r.success


class TestDetectElevation:
    """Unit tests for the ``_detect_elevation`` helper."""

    def test_returns_bool(self) -> None:
        """``_detect_elevation`` returns a bool (True or False), never raises."""
        result = _detect_elevation()
        assert isinstance(result, bool)

    def test_use_serve_param_accepted(self, fake_cli: Path) -> None:
        """``use_serve`` is an accepted kwarg and stored on the instance."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        assert c._use_serve is True
        c2 = TsDevClient(cli_path=str(fake_cli))
        assert c2._use_serve is True

    def test_use_serve_default_is_true(self, fake_cli: Path) -> None:
        """The ``use_serve`` default is ``True`` (flipped in K.6b).

        Constructing a :class:`TsDevClient` without an explicit ``use_serve``
        argument opts into serve-broker routing for non-elevated sessions.
        """
        c = TsDevClient(cli_path=str(fake_cli))
        assert c._use_serve is True

    def test_token_query_constant_is_correct(self):
        # Regression guard: 0x0020 is TOKEN_ADJUST_PRIVILEGES, not TOKEN_QUERY.
        # A wrong value here makes _detect_elevation() always return False,
        # which makes TsDevClient(use_serve=False) always raise ElevationRequiredError.
        from tsdev_automation.client import _TOKEN_QUERY
        assert _TOKEN_QUERY == 0x0008


# ---------------------------------------------------------------------------
# Crash detection (VEH / NT status code exit codes) — mocked unit tests
# ---------------------------------------------------------------------------


class TestIsCrashExit:
    """Unit tests for the :func:`_is_crash_exit` helper."""

    def test_positive_access_violation(self) -> None:
        is_crash, crash_type = _is_crash_exit(0xC0000005)
        assert is_crash is True
        assert crash_type == "ACCESS_VIOLATION"

    def test_negative_access_violation(self) -> None:
        """subprocess.run may report a negative returncode for a large NT status."""
        # -1073741819 == 0xC0000005 as a signed 32-bit int.
        is_crash, crash_type = _is_crash_exit(-1073741819)
        assert is_crash is True
        assert crash_type == "ACCESS_VIOLATION"

    def test_stack_overflow(self) -> None:
        is_crash, crash_type = _is_crash_exit(0xC00000FD)
        assert is_crash is True
        assert crash_type == "STACK_OVERFLOW"

    def test_stack_buffer_overrun(self) -> None:
        is_crash, crash_type = _is_crash_exit(0xC0000409)
        assert is_crash is True
        assert crash_type == "STACK_BUFFER_OVERRUN"

    def test_heap_corruption(self) -> None:
        is_crash, crash_type = _is_crash_exit(0xC0000374)
        assert is_crash is True
        assert crash_type == "HEAP_CORRUPTION"

    def test_normal_exit_codes_not_crash(self) -> None:
        for rc in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10):
            is_crash, crash_type = _is_crash_exit(rc)
            assert is_crash is False, f"exit {rc} should not be a crash"
            assert crash_type is None

    def test_none_returncode_not_crash(self) -> None:
        is_crash, crash_type = _is_crash_exit(None)  # type: ignore[arg-type]
        assert is_crash is False
        assert crash_type is None


class TestRunCliCrashDetection:
    """Unit tests for crash detection in :meth:`TsDevClient._run_cli`."""

    def test_run_cli_raises_crash_on_access_violation(self, client: TsDevClient, monkeypatch) -> None:
        """A negative returncode (0xC0000005) raises ``TsDevCrashError``."""
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed("", stderr="", returncode=-1073741819),
        )
        with pytest.raises(TsDevCrashError) as ei:
            client.insert("strategy", "X", "x;")
        assert ei.value.exit_code == -1073741819
        assert "ACCESS_VIOLATION" in str(ei.value)

    def test_run_cli_raises_crash_on_positive_code(self, client: TsDevClient, monkeypatch) -> None:
        """A positive NT status exit code also raises ``TsDevCrashError``."""
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed("", stderr="", returncode=0xC00000FD),
        )
        with pytest.raises(TsDevCrashError) as ei:
            client.delete("strategy", "X")
        assert ei.value.exit_code == 0xC00000FD
        assert "STACK_OVERFLOW" in str(ei.value)

    def test_run_cli_no_crash_on_normal_exit(self, client: TsDevClient, monkeypatch) -> None:
        """A normal exit code does NOT raise ``TsDevCrashError``."""
        monkeypatch.setattr(
            subprocess, "run", lambda *a, **k: _make_completed(_success_json("insert", "strategy", "X"))
        )
        r = client.insert("strategy", "X", "x;")
        assert r.success is True


class TestRunBatchCrashDetection:
    """Unit tests for crash detection in :meth:`TsDevClient._run_batch`."""

    def test_run_batch_raises_crash_on_access_violation(self, client: TsDevClient, monkeypatch) -> None:
        monkeypatch.setattr(
            subprocess,
            "run",
            lambda *a, **k: _make_completed("", stderr="", returncode=-1073741819),
        )
        with pytest.raises(TsDevCrashError) as ei:
            client.batch_insert([BatchItem("strategy", "A", "a;")])
        assert ei.value.exit_code == -1073741819
        assert "ACCESS_VIOLATION" in str(ei.value)

    def test_run_batch_no_crash_on_normal_exit(self, client: TsDevClient, monkeypatch) -> None:
        arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr))
        results = client.batch_insert([BatchItem("strategy", "A", "a;")])
        assert len(results) == 1 and results[0].success


class TestServeBrokerCrashDetection:
    """Unit tests for crash detection in :meth:`_ServeBroker.call`."""

    def test_call_raises_crash_when_broker_exited_with_crash_code(self, monkeypatch, tmp_path: Path) -> None:
        """A broken pipe + crash exit code raises ``TsDevCrashError`` (no retry)."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._started = True
        broker._pipe_handle = 111
        broker._process_handle = 222

        # First send_request raises PipeError (broken pipe).
        call_count = {"n": 0}

        def fake_send(handle, obj):
            call_count["n"] += 1
            if call_count["n"] == 1:
                raise _pipe.PipeError("broken pipe")

        monkeypatch.setattr(_pipe, "send_request", fake_send)
        monkeypatch.setattr(_pipe, "recv_response", lambda handle: {})
        # _get_exit_code returns a crash code.
        monkeypatch.setattr(broker, "_get_exit_code", lambda: 0xC0000005)
        # _reset_pipe should not raise (mock the internals).
        monkeypatch.setattr(_pipe, "close", lambda handle: None)
        monkeypatch.setattr(
            "tsdev_automation.client._wait_for_exit", lambda handle, timeout_ms: True
        )
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.CloseHandle", lambda handle: True
        )

        with pytest.raises(TsDevCrashError) as ei:
            broker.call({"op": "insert", "type": "strategy", "name": "X", "source": "x;"})
        assert ei.value.exit_code == 0xC0000005
        assert "ACCESS_VIOLATION" in str(ei.value)
        # The request must NOT be retried after a crash.
        assert call_count["n"] == 1

    def test_call_retries_on_non_crash_broken_pipe(self, monkeypatch, tmp_path: Path) -> None:
        """A broken pipe with the broker still alive triggers a reconnect+retry."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._started = True
        broker._pipe_handle = 111
        broker._process_handle = 222

        call_count = {"n": 0}

        def fake_send(handle, obj):
            call_count["n"] += 1
            if call_count["n"] == 1:
                raise _pipe.PipeError("broken pipe")

        monkeypatch.setattr(_pipe, "send_request", fake_send)
        monkeypatch.setattr(_pipe, "recv_response", lambda handle: {"op": "insert", "success": True})
        # Broker is still alive (None exit code).
        monkeypatch.setattr(broker, "_get_exit_code", lambda: None)
        # _reset_pipe + start() must be no-ops.
        monkeypatch.setattr(_pipe, "close", lambda handle: None)
        monkeypatch.setattr(
            "tsdev_automation.client._wait_for_exit", lambda handle, timeout_ms: True
        )
        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.CloseHandle", lambda handle: True
        )
        monkeypatch.setattr(broker, "start", lambda: setattr(broker, "_pipe_handle", 333))

        obj = broker.call({"op": "insert", "type": "strategy", "name": "X", "source": "x;"})
        # The request was retried (send called twice).
        assert call_count["n"] == 2
        assert obj["success"] is True

    def test_get_exit_code_returns_none_when_still_active(self, monkeypatch, tmp_path: Path) -> None:
        """``_get_exit_code`` returns ``None`` when STILL_ACTIVE (259)."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._process_handle = 999

        def fake_get_exit_code(handle, out):
            ctypes.cast(out, ctypes.POINTER(wintypes.DWORD)).contents.value = 259  # STILL_ACTIVE
            return True

        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.GetExitCodeProcess", fake_get_exit_code
        )
        assert broker._get_exit_code() is None

    def test_get_exit_code_returns_code_when_exited(self, monkeypatch, tmp_path: Path) -> None:
        """``_get_exit_code`` returns the exit code when the process has exited."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._process_handle = 999

        def fake_get_exit_code(handle, out):
            ctypes.cast(out, ctypes.POINTER(wintypes.DWORD)).contents.value = 0xC0000005
            return True

        monkeypatch.setattr(
            "tsdev_automation.client._kernel32.GetExitCodeProcess", fake_get_exit_code
        )
        assert broker._get_exit_code() == 0xC0000005

    def test_get_exit_code_returns_none_when_no_handle(self, tmp_path: Path) -> None:
        """``_get_exit_code`` returns ``None`` when there is no process handle."""
        broker = _ServeBroker(tmp_path / "tsdev_cli.exe")
        broker._process_handle = None
        assert broker._get_exit_code() is None


# ---------------------------------------------------------------------------
# E2E tests (real CLI) -- skipped unless the CLI is available
# ---------------------------------------------------------------------------

# Base name for E2E components; cleaned up after each test/class.
_UNIQUE = "PyAutoTest"

# Valid EasyLanguage sources per component type (verified against the real
# EasyLanguage compiler via tsdev_cli -- all five compile to exit 0).
# Function source MUST assign to a variable named exactly like the component
# for compiler syntax; this is not a metadata lookup convention. Stored names,
# types, and ProgIds come from warehouse records/APIs.
_SOURCES: dict[str, dict[str, str]] = {
    "strategy": {
        "insert": 'Inputs: Length(10); If Close > Average(Close, Length) Then Buy next bar at Market;\n',
        "modify": 'Inputs: Length(20); If Close > Average(Close, Length) Then Sell next bar at Market;\n',
    },
    "indicator": {
        "insert": 'Plot1(Close, "Close");\n',
        "modify": 'Plot1(Close * 2, "Close2");\n',
    },
    "function": {
        # <Name> placeholder is replaced with the actual component name at runtime.
        "insert": "{name} = 1;\n",
        "modify": "{name} = 42;\n",
    },
    "tradingapp": {
        "insert": "using elsystem;\n",
        "modify": "using elsystem;\nusing elsystem.windows.forms;\n",
    },
    # ShowMe (whType 1282) can use Plot statements (like Indicators) but
    # cannot use Buy/Sell order statements (Strategy-only). Verified against
    # the real EasyLanguage compiler via tsdev_cli (see tests/test_showme_e2e.ps1).
    "showme": {
        "insert": 'if Close > Open then\n    Plot1(High, "Up bar");\n',
        "modify": 'if Close < Open then\n    Plot1(Low, "Down bar");\n',
    },
}

# All five supported component types.
_ALL_TYPES = ["strategy", "indicator", "function", "tradingapp", "showme"]


def _src(type_: str, op: str, name: str) -> str:
    """Return the EasyLanguage source for a given type/op, substituting the
    function-name placeholder when present."""
    return _SOURCES[type_][op].format(name=name)


def _cli_available() -> str | None:
    p = os.environ.get("TSDEV_CLI_PATH")
    if p and os.path.isfile(p):
        return p
    # default discovery
    root = Path(__file__).resolve().parent.parent
    cand = root / "build" / "Release" / "tsdev_cli.exe"
    if cand.is_file():
        return str(cand)
    return None


e2e_cli = _cli_available()
e2e_skip = pytest.mark.skipif(e2e_cli is None, reason="tsdev_cli.exe not available")


@e2e_skip
@pytest.mark.e2e
class TestE2E:
    """End-to-end tests driving the real ``tsdev_cli.exe`` and the TradeStation
    warehouse.  These are skipped unless the CLI binary is discoverable.

    Every test cleans up all components it creates via the autouse
    ``_cleanup_all`` fixture, which sweeps all known test names across all five
    types so no orphans remain even if a test fails mid-way.
    """

    # Names used across the E2E suite.  The cleanup fixture sweeps ALL of these
    # across ALL five types so a failure mid-test cannot leak warehouse entries.
    _TEST_NAMES: list[str] = [
        _UNIQUE + "_Strat",
        _UNIQUE + "_Func",
        _UNIQUE + "_Ind",
        _UNIQUE + "_TA",
        _UNIQUE + "_Dup",
        _UNIQUE + "_BadCompile",
        _UNIQUE + "_Batch",
        "zzz_py_ci_probe",
        _UNIQUE + "_UpsA",
        _UNIQUE + "_UpsB",
        _UNIQUE + "_DirA",
        _UNIQUE + "_DirB",
        _UNIQUE + "_DirC",
        _UNIQUE + "_UDirA",
        _UNIQUE + "_UDirB",
        _UNIQUE + "_MDirA",
        _UNIQUE + "_MDirB",
        _UNIQUE + "_TxtA",
        "TS_AUTO_PY_SHOWME_L14A",
        _UNIQUE + "_MixA",
        _UNIQUE + "_MixB",
        _UNIQUE + "_MixC",
    ]

    @pytest.fixture(autouse=True)
    def _real_client(self):
        self.c = TsDevClient()
        yield
        # Sweep: best-effort delete of every test name across all 5 types.
        # A clean warehouse has no orphans, so every delete should raise
        # NotFoundError (exit 5).  We swallow errors here; the dedicated
        # test_no_orphans test asserts the sweep result explicitly.
        for name in self._TEST_NAMES:
            for t in _ALL_TYPES:
                try:
                    self.c.delete(t, name)
                except Exception:
                    pass
        # Also sweep the 20 batch items.
        for i in range(20):
            for t in _ALL_TYPES:
                try:
                    self.c.delete(t, f"{_UNIQUE}_Batch{i:02d}")
                except Exception:
                    pass

    # -- Part 1: insert -> modify -> delete lifecycle per component type -----

    def test_strategy_lifecycle(self) -> None:
        name = _UNIQUE + "_Strat"
        r = self.c.insert("strategy", name, _src("strategy", "insert", name))
        assert r.success, r
        assert r.prog_id is not None and r.prog_id > 0
        assert r.ell_path
        prog_id = r.prog_id
        m = self.c.modify("strategy", name, _src("strategy", "modify", name))
        assert m.success, m
        assert m.prog_id == prog_id, "ProgId must be preserved across modify"
        d = self.c.delete("strategy", name)
        assert d.success, d

    def test_function_lifecycle(self) -> None:
        name = _UNIQUE + "_Func"
        r = self.c.insert("function", name, _src("function", "insert", name))
        assert r.success, r
        assert r.prog_id is not None and r.prog_id > 0
        assert r.ell_path
        prog_id = r.prog_id
        m = self.c.modify("function", name, _src("function", "modify", name))
        assert m.success, m
        assert m.prog_id == prog_id, "ProgId must be preserved across modify"
        d = self.c.delete("function", name)
        assert d.success, d

    def test_indicator_lifecycle(self) -> None:
        name = _UNIQUE + "_Ind"
        r = self.c.insert("indicator", name, _src("indicator", "insert", name))
        assert r.success, r
        assert r.prog_id is not None and r.prog_id > 0
        assert r.ell_path
        prog_id = r.prog_id
        m = self.c.modify("indicator", name, _src("indicator", "modify", name))
        assert m.success, m
        assert m.prog_id == prog_id, "ProgId must be preserved across modify"
        d = self.c.delete("indicator", name)
        assert d.success, d

    def test_tradingapp_lifecycle(self) -> None:
        name = _UNIQUE + "_TA"
        r = self.c.insert("tradingapp", name, _src("tradingapp", "insert", name))
        assert r.success, r
        assert r.prog_id is not None and r.prog_id > 0
        assert r.ell_path
        prog_id = r.prog_id
        m = self.c.modify("tradingapp", name, _src("tradingapp", "modify", name))
        assert m.success, m
        assert m.prog_id == prog_id, "ProgId must be preserved across modify"
        d = self.c.delete("tradingapp", name)
        assert d.success, d

    # -- Part 1b: full ShowMe lifecycle E2E (TVS-L.14a GREEN, test-only) ----

    def test_showme_lifecycle_e2e(self) -> None:
        """TVS-L.14a GREEN: real ShowMe lifecycle E2E through TsDevClient
        against the real ``tsdev_cli.exe`` and TradeStation warehouse.

        Exercises the full insert -> exists -> get-source -> modify ->
        get-source(after modify) -> upsert-existing -> delete -> exists(after
        delete) lifecycle for a ShowMe item (whType 1282). ShowMe studies can
        use Plot statements (like Indicators) but cannot use Buy/Sell order
        statements (Strategy-only).

        Uses a unique disposable ``TS_AUTO_`` name and relies on the autouse
        ``_real_client`` cleanup fixture (which sweeps ``_TEST_NAMES`` across
        ``_ALL_TYPES``) for orphan prevention. ELL/XML artifact and GUID
        checks are intentionally omitted because no such helpers exist for
        any other component type in ``TestE2E`` — the existing lifecycle
        tests assert only ``r.ell_path`` (non-empty), which this test mirrors.
        """
        name = "TS_AUTO_PY_SHOWME_L14A"

        # 1. insert — valid ShowMe source using Plot1.
        r = self.c.insert("showme", name, _src("showme", "insert", name))
        assert r.success, r
        assert r.prog_id is not None and r.prog_id > 0
        assert r.ell_path, "insert must report a non-empty ell_path"
        prog_id = r.prog_id

        # 2. exists — case-sensitive exact match returns the verbatim name
        #    and the same ProgId assigned at insert.
        ex = self.c.exists("showme", name)
        assert ex.success, ex
        assert ex.exists is True
        assert ex.prog_id == prog_id, "exists must report the same ProgId as insert"
        assert ex.stored_name == name, f"stored_name must be {name!r}, got {ex.stored_name!r}"

        # 3. get-source — retrieved source must contain the insert marker.
        gs = self.c.get_source("showme", name)
        assert gs.success, gs
        assert gs.source is not None, "get-source must return a non-null source"
        assert "Plot1" in gs.source, "get-source must contain the Plot1 marker from insert source"

        # 4. modify — replace source; ProgId must be preserved.
        m = self.c.modify("showme", name, _src("showme", "modify", name))
        assert m.success, m
        assert m.prog_id == prog_id, "ProgId must be preserved across modify"
        assert m.ell_path, "modify must report a non-empty ell_path"

        # 5. get-source after modify — the new source must round-trip.
        gs2 = self.c.get_source("showme", name)
        assert gs2.success, gs2
        assert gs2.source is not None
        assert "Down bar" in gs2.source, "get-source after modify must contain the modify marker"

        # 6. upsert-existing — item already exists, so upsert must take the
        #    modify-path and preserve the ProgId.
        u = self.c.upsert("showme", name, _src("showme", "insert", name))
        assert u.success, u
        assert u.prog_id == prog_id, "ProgId must be preserved on upsert-existing (modify-path)"
        assert u.ell_path, "upsert-existing must report a non-empty ell_path"

        # 7. delete — remove the ShowMe item.
        d = self.c.delete("showme", name)
        assert d.success, d

        # 8. exists after delete — must report exists=False, ProgId 0.
        ex2 = self.c.exists("showme", name)
        assert ex2.success, ex2
        assert ex2.exists is False, "exists must be False after delete"
        assert ex2.prog_id == 0, "ProgId must be 0 after delete"
        assert ex2.stored_name == "", "stored_name must be empty after delete"

    # -- Part 2: larger batch (init amortization) ---------------------------

    def test_batch_insert_20_then_delete(self) -> None:
        """Batch-insert 20 strategies, verify all succeed, then batch-delete
        all 20.  Confirms init amortization works through the Python wrapper
        and completes in a reasonable time (< 30 s sanity bound)."""
        import time

        items = [
            BatchItem("strategy", f"{_UNIQUE}_Batch{i:02d}", _src("strategy", "insert", f"batch{i}"))
            for i in range(20)
        ]
        t0 = time.monotonic()
        results = self.c.batch_insert(items)
        elapsed = time.monotonic() - t0
        assert len(results) == 20
        assert all(r.success for r in results), "all 20 batch inserts must succeed"
        for r in results:
            assert r.prog_id is not None and r.prog_id > 0
            assert r.ell_path
        assert elapsed < 30.0, f"batch insert of 20 took {elapsed:.1f}s (expected < 30s)"

        del_items = [BatchItem("strategy", f"{_UNIQUE}_Batch{i:02d}") for i in range(20)]
        t1 = time.monotonic()
        del_results = self.c.batch_delete(del_items)
        elapsed_del = time.monotonic() - t1
        assert len(del_results) == 20
        assert all(r.success for r in del_results), "all 20 batch deletes must succeed"
        assert elapsed_del < 30.0, f"batch delete of 20 took {elapsed_del:.1f}s (expected < 30s)"

    # -- Part 3: exception cases via the Python wrapper ---------------------

    def test_insert_duplicate_raises_already_exists(self) -> None:
        name = _UNIQUE + "_Dup"
        src = _src("strategy", "insert", name)
        r = self.c.insert("strategy", name, src)
        assert r.success
        with pytest.raises(AlreadyExistsError) as ei:
            self.c.insert("strategy", name, src)
        assert ei.value.result.error is not None
        assert ei.value.result.error.existing_prog_id is not None
        assert ei.value.result.error.existing_prog_id > 0
        # cleanup
        self.c.delete("strategy", name)

    def test_insert_invalid_source_raises_compile_error_and_autocleans(self) -> None:
        name = _UNIQUE + "_BadCompile"
        with pytest.raises(CompileError) as ei:
            self.c.insert("strategy", name, "this is not valid easylanguage !!!\n")
        assert ei.value.result.error is not None
        assert ei.value.result.error.compile_errors
        assert isinstance(ei.value.result.error.compile_errors, str)
        assert len(ei.value.result.error.compile_errors) > 0
        # Auto-cleanup: the partial entry from the failed compile should have
        # been removed by the CLI, so a subsequent delete raises NotFoundError.
        with pytest.raises(NotFoundError):
            self.c.delete("strategy", name)

    def test_delete_nonexistent_raises_not_found(self) -> None:
        with pytest.raises(NotFoundError):
            self.c.delete("strategy", "DefinitelyNotHerePyAuto")

    # -- Part 5: case-insensitive exists (TVS-F.5) --------------------------

    def test_exists_case_insensitive_e2e(self) -> None:
        """E2E: case-insensitive ``exists`` returns the verbatim stored name
        and a non-zero ProgId; case-sensitive mode respects exact casing.

        The source includes a ``{ name }`` EasyLanguage comment only as
        arbitrary fixture content. It is not a naming convention, a required
        source shape, or a lookup mechanism; the stored name and ProgId must
        come from the warehouse/API-backed CLI result.
        """
        name = "zzz_py_ci_probe"
        # Include arbitrary brace-comment content; no metadata lookup depends on it.
        src = "{ " + name + " }\n" + _src("strategy", "insert", name)
        r = self.c.insert("strategy", name, src)
        assert r.success, r
        try:
            # Case-insensitive query with uppercase name matches the lowercase item.
            ci = self.c.exists("strategy", "ZZZ_PY_CI_PROBE", case_insensitive=True)
            assert ci.success is True
            assert ci.exists is True
            assert ci.stored_name == name, f"stored_name must be verbatim {name!r}, got {ci.stored_name!r}"
            assert ci.prog_id is not None and ci.prog_id > 0

            # Case-sensitive (default) with uppercase name: must NOT match.
            cs_wrong = self.c.exists("strategy", "ZZZ_PY_CI_PROBE")
            assert cs_wrong.success is True
            assert cs_wrong.exists is False
            assert cs_wrong.prog_id == 0
            assert cs_wrong.stored_name == ""

            # Case-sensitive (default) with exact name: must match.
            cs_exact = self.c.exists("strategy", name)
            assert cs_exact.success is True
            assert cs_exact.exists is True
            assert cs_exact.prog_id is not None and cs_exact.prog_id > 0
            assert cs_exact.stored_name == name
        finally:
            try:
                self.c.delete("strategy", name)
            except Exception:
                pass

    # -- Part 6: batch upsert + directory batch ops (TVS-I.3) --------------

    def test_e2e_batch_upsert_mixed(self) -> None:
        """E2E: batch_upsert with 1 modify-path (pre-existing) + 1 insert-path.

        Item X is pre-inserted so its upsert is a modify (ProgId preserved);
        item Y is new so its upsert is an insert.
        """
        name_x = _UNIQUE + "_UpsA"
        name_y = _UNIQUE + "_UpsB"
        # Pre-insert X so the upsert of X is a modify-path.
        r = self.c.insert("strategy", name_x, _src("strategy", "insert", name_x))
        assert r.success, r
        prog_id_x = r.prog_id
        # Verify exists before upsert.
        ex = self.c.exists("strategy", name_x)
        assert ex.exists and ex.prog_id == prog_id_x
        # batch_upsert: X (modify-path) + Y (insert-path).
        results = self.c.batch_upsert(
            [
                BatchItem("strategy", name_x, _src("strategy", "modify", name_x)),
                BatchItem("strategy", name_y, _src("strategy", "insert", name_y)),
            ]
        )
        assert len(results) == 2
        assert all(r.success for r in results), results
        # X's ProgId must be preserved (modify-path).
        assert results[0].prog_id == prog_id_x, "ProgId must be preserved on modify-path upsert"
        # Y was newly inserted.
        assert results[1].prog_id is not None and results[1].prog_id > 0
        # Verify exists after upsert.
        ex_after = self.c.exists("strategy", name_x)
        assert ex_after.exists and ex_after.prog_id == prog_id_x

    def test_e2e_batch_mixed_outcome_continue_on_failure(self) -> None:
        """E2E: mixed-outcome batch_insert continues on failure (exit 2).

        Mirrors C++ reference ``test_batch_e2e.ps1`` Step 13 through the
        Python wrapper in DIRECT mode: one already-exists, one success, and
        one compile-error.  Verifies ``batch_insert`` returns a full per-item
        result list instead of raising, and that the duplicate and compile-
        error outcomes are reported correctly without corrupting persisted
        state.
        """
        name_a = _UNIQUE + "_MixA"
        name_b = _UNIQUE + "_MixB"
        name_c = _UNIQUE + "_MixC"

        r = self.c.insert("function", name_a, _src("function", "insert", name_a))
        assert r.success, r
        prog_id_a = r.prog_id

        results = self.c.batch_insert(
            [
                BatchItem("function", name_a, _src("function", "insert", name_a)),
                BatchItem("function", name_b, _src("function", "insert", name_b)),
                BatchItem("function", name_c, "this is not valid easylanguage !!!\n"),
            ]
        )

        assert len(results) == 3
        assert results[0].success is False
        assert results[0].exit_code == 5
        assert results[0].error is not None and results[0].error.code == "already_exists"
        assert results[0].error.existing_prog_id == prog_id_a
        assert results[1].success is True
        assert results[1].prog_id is not None and results[1].prog_id > 0
        assert results[2].success is False
        assert results[2].exit_code == 8
        assert results[2].error is not None and results[2].error.code == "compile_error"

        ex_b = self.c.exists("function", name_b)
        assert ex_b.exists and ex_b.prog_id == results[1].prog_id
        ex_a = self.c.exists("function", name_a)
        assert ex_a.exists and ex_a.prog_id == prog_id_a
        ex_c = self.c.exists("function", name_c)
        assert not ex_c.exists

    def test_e2e_batch_insert_dir_then_delete_dir(self, tmp_path: Path) -> None:
        """E2E: batch_insert_dir then batch_delete_dir on a temp dir of .el files."""
        names = [_UNIQUE + "_DirA", _UNIQUE + "_DirB", _UNIQUE + "_DirC"]
        d = tmp_path / "batchdir"
        d.mkdir()
        for name in names:
            (d / f"{name}.el").write_text(_src("strategy", "insert", name), encoding="utf-8")
        # Insert all 3.
        results = self.c.batch_insert_dir("strategy", str(d))
        assert len(results) == 3
        assert all(r.success for r in results), results
        # Delete all 3.
        del_results = self.c.batch_delete_dir("strategy", str(d))
        assert len(del_results) == 3
        assert all(r.success for r in del_results), del_results

    def test_e2e_batch_upsert_dir_mixed(self, tmp_path: Path) -> None:
        """E2E: batch_upsert_dir with 1 pre-existing (modify) + 1 new (insert)."""
        name_a = _UNIQUE + "_UDirA"
        name_b = _UNIQUE + "_UDirB"
        d = tmp_path / "upsertdir"
        d.mkdir()
        # Pre-insert A.
        r = self.c.insert("strategy", name_a, _src("strategy", "insert", name_a))
        assert r.success, r
        prog_id_a = r.prog_id
        # Write both .el files (A with modified source, B with insert source).
        (d / f"{name_a}.el").write_text(_src("strategy", "modify", name_a), encoding="utf-8")
        (d / f"{name_b}.el").write_text(_src("strategy", "insert", name_b), encoding="utf-8")
        # batch_upsert_dir.
        results = self.c.batch_upsert_dir("strategy", str(d))
        assert len(results) == 2
        assert all(r.success for r in results), results
        # The pre-existing one (A) should be a modify: ProgId preserved.
        a_result = [r for r in results if r.name == name_a][0]
        assert a_result.prog_id == prog_id_a, "ProgId must be preserved on modify-path upsert"

    def test_e2e_batch_modify_dir(self, tmp_path: Path) -> None:
        """E2E: batch_insert then batch_modify_dir with modified source.

        Both items are pre-inserted via batch_insert, then batch_modify_dir is
        called on a directory containing modified source files.  ProgIds must
        be preserved across the modify.
        """
        name_a = _UNIQUE + "_MDirA"
        name_b = _UNIQUE + "_MDirB"
        d = tmp_path / "modifydir"
        d.mkdir()
        # Pre-insert both via batch_insert.
        ins = self.c.batch_insert(
            [
                BatchItem("strategy", name_a, _src("strategy", "insert", name_a)),
                BatchItem("strategy", name_b, _src("strategy", "insert", name_b)),
            ]
        )
        assert len(ins) == 2 and all(r.success for r in ins), ins
        prog_id_a = ins[0].prog_id
        prog_id_b = ins[1].prog_id
        # Write modified source files.
        (d / f"{name_a}.el").write_text(_src("strategy", "modify", name_a), encoding="utf-8")
        (d / f"{name_b}.el").write_text(_src("strategy", "modify", name_b), encoding="utf-8")
        # batch_modify_dir.
        results = self.c.batch_modify_dir("strategy", str(d))
        assert len(results) == 2
        assert all(r.success for r in results), results
        # ProgIds must be preserved.
        a_result = [r for r in results if r.name == name_a][0]
        b_result = [r for r in results if r.name == name_b][0]
        assert a_result.prog_id == prog_id_a
        assert b_result.prog_id == prog_id_b

    def test_e2e_batch_dir_t_override(self, tmp_path: Path) -> None:
        """E2E: batch_insert_dir with extensions=['txt'] on a .txt-only dir.

        The directory contains only ``.txt`` files (not ``.el``), so the
        default extensions would find nothing.  Passing ``extensions=['txt']``
        overrides the default so the files are picked up.
        """
        name = _UNIQUE + "_TxtA"
        d = tmp_path / "txtdir"
        d.mkdir()
        (d / f"{name}.txt").write_text(_src("strategy", "insert", name), encoding="utf-8")
        results = self.c.batch_insert_dir("strategy", str(d), extensions=["txt"])
        assert len(results) == 1
        assert all(r.success for r in results), results

    # -- Part 7: direct-mode batch on_progress over --batch-progress-control
    #    (TVS C) ------------------------------------------------------------

    def test_e2e_direct_batch_on_progress(self, tmp_path: Path) -> None:
        """TVS C: live direct ``on_progress`` batch through
        ``tsdev_cli --batch-progress-control``.

        Requires an ELEVATED session (``use_serve=False``): the client drives
        one ``tsdev_cli`` child on binary pipes — a framed
        ``TSDEV_BATCH_CONTROL/1`` manifest + ``continue``/``cancel``
        decisions on stdin for manifest batches, decisions-only stdin for
        directory batches, NDJSON progress on stderr, and the authoritative
        final JSON array on stdout.  Covers: continue after a real per-item
        compile failure, cancel after a success, cancel after a not_found
        failure, full-length exit-12 ``cancelled`` markers (no callback for
        markers), and a ``batch_insert_dir`` cancellation.  No broker is
        ever started (``client._broker is None`` throughout).
        """
        if not _detect_elevation():
            pytest.skip("direct batch-progress E2E requires an elevated session")

        import uuid

        # UUID-derived disposable function names: no fixed-name collisions.
        uid = uuid.uuid4().hex[:8].upper()
        name_a = f"TS_C_{uid}_A"      # inserted, then deleted by cancel-after-success
        name_b = f"TS_C_{uid}_B"      # inserted, marker twice, deleted at end
        name_c = f"TS_C_{uid}_C"      # never attempted (cancelled marker)
        name_bad = f"TS_C_{uid}_BAD"  # compile-failure item (never exists)
        name_ghost = f"TS_C_{uid}_GHOST"  # never created (not-found delete)
        dir_a = f"TS_C_{uid}_DA"      # dir-batch: inserted
        dir_b = f"TS_C_{uid}_DB"      # dir-batch: marker
        dir_c = f"TS_C_{uid}_DC"      # dir-batch: marker
        names = [name_a, name_b, name_c, name_bad, name_ghost, dir_a, dir_b, dir_c]

        invalid_source = "{ TVS C broken }\nInputs: ;\n{ syntax error }\n"

        client = TsDevClient(cli_path=e2e_cli, use_serve=False)
        assert client._broker is None

        def _marker(r: TsDevResult, op: str, t: str, n: str) -> None:
            assert r.success is False
            assert r.exit_code == 12
            assert r.error is not None and r.error.code == "cancelled"
            assert r.error.message == "Batch cancelled before this item was attempted"
            assert r.op == op and r.type == t and r.name == n

        def _same_result(a: TsDevResult, b: TsDevResult) -> bool:
            # Event-time nested results carry no captured stdout/stderr while
            # final-array results do — compare the JSON-derived fields only.
            # Close-failure rewriting can legitimately change attempted
            # successes between the event and the final (authoritative) array;
            # every scenario here closes cleanly, so the fields must match.
            return (
                a.success == b.success
                and a.op == b.op
                and a.type == b.type
                and a.name == b.name
                and a.exit_code == b.exit_code
                and a.prog_id == b.prog_id
                and a.ell_path == b.ell_path
                and a.error == b.error
            )

        try:
            # Pre-cleanup: best-effort delete of leftovers from prior runs.
            for n in names:
                try:
                    client.delete("function", n)
                except Exception:
                    pass

            # --- 1. Callback continuation after a real compile failure ----
            events: list = []
            results = client.batch_insert(
                [
                    BatchItem("function", name_a, _src("function", "insert", name_a)),
                    BatchItem("function", name_bad, invalid_source),
                    BatchItem("function", name_b, _src("function", "insert", name_b)),
                ],
                on_progress=events.append,
            )
            assert client._broker is None, "direct path must never start a broker"
            assert len(results) == 3
            assert len(events) == 3
            assert all(isinstance(p, BatchProgress) for p in events)
            assert [p.index for p in events] == [1, 2, 3]
            assert [p.total for p in events] == [3, 3, 3]
            for p, r in zip(events, results):
                assert _same_result(p.result, r), (
                    "callback result fields must match the final array element "
                    f"(stdout/stderr excluded): {p.result} vs {r}"
                )
            assert results[0].success and results[0].prog_id is not None and results[0].prog_id > 0
            assert results[1].success is False and results[1].exit_code == 8
            assert results[1].error is not None and results[1].error.code == "compile_error"
            # The third item was attempted despite the second item's failure.
            assert results[2].success and results[2].name == name_b
            assert client.exists("function", name_a).exists
            assert client.exists("function", name_b).exists
            assert not client.exists("function", name_bad).exists

            # --- 2. Cancel after a success --------------------------------
            events2: list = []

            def _cancel2(p) -> bool:
                events2.append(p)
                return False

            results2 = client.batch_delete(
                [
                    BatchItem("function", name_a),
                    BatchItem("function", name_b),
                    BatchItem("function", name_c),
                ],
                on_progress=_cancel2,
            )
            assert client._broker is None
            assert len(results2) == 3, "cancelled batch must return a full-length list"
            assert len(events2) == 1, "callback must not be invoked for markers"
            assert results2[0].success
            _marker(results2[1], "delete", "function", name_b)
            _marker(results2[2], "delete", "function", name_c)
            assert not client.exists("function", name_a).exists
            assert client.exists("function", name_b).exists
            assert not client.exists("function", name_c).exists

            # --- 3. Cancel after a not_found failure ----------------------
            events3: list = []

            def _cancel3(p) -> bool:
                events3.append(p)
                return False

            results3 = client.batch_delete(
                [BatchItem("function", name_ghost), BatchItem("function", name_b)],
                on_progress=_cancel3,
            )
            assert client._broker is None
            assert len(results3) == 2
            assert len(events3) == 1, "callback must not be invoked for markers"
            assert results3[0].success is False and results3[0].exit_code == 5
            assert results3[0].error is not None and results3[0].error.code == "not_found"
            _marker(results3[1], "delete", "function", name_b)
            assert client.exists("function", name_b).exists

            # --- 4. Decisions-only stdin: native batch_insert_dir cancel --
            d = tmp_path / "ctrl_dir"
            d.mkdir()
            for n in (dir_a, dir_b, dir_c):
                (d / f"{n}.el").write_text(
                    _src("function", "insert", n), encoding="utf-8"
                )
            events4: list = []

            def _cancel4(p) -> bool:
                events4.append(p)
                return False

            results4 = client.batch_insert_dir(
                "function", str(d), on_progress=_cancel4
            )
            assert client._broker is None
            assert len(results4) == 3
            assert len(events4) == 1, "callback must not be invoked for markers"
            # Sorted file order: DA attempted first, DB/DC became markers.
            assert results4[0].success and results4[0].name == dir_a
            _marker(results4[1], "insert", "function", dir_b)
            _marker(results4[2], "insert", "function", dir_c)
            assert client.exists("function", dir_a).exists
            assert not client.exists("function", dir_b).exists
            assert not client.exists("function", dir_c).exists

            # Verified production cleanup of what remains.
            for n in (name_b, dir_a):
                dr = client.delete("function", n)
                assert dr.success, f"cleanup delete for {n} failed: {dr}"
        finally:
            # Exact cleanup: every name this test could have created.
            for n in names:
                try:
                    client.delete("function", n)
                except Exception:
                    pass

    # -- Part 4: orphan sweep (no leftover warehouse entries) ---------------

    def test_no_orphans_after_suite(self) -> None:
        """Final safety-net sweep: attempt to delete every test name across all
        5 types; each must raise NotFoundError, proving no orphaned warehouse
        entries remain. The autouse ``_real_client`` fixture already cleans up
        after every test, so this is not reliant on alphabetical-last ordering
        — it simply re-verifies explicitly so a leftover entry is reported as a
        test failure rather than silently swallowed."""
        for name in self._TEST_NAMES:
            for t in _ALL_TYPES:
                with pytest.raises(NotFoundError):
                    self.c.delete(t, name)
        for i in range(20):
            for t in _ALL_TYPES:
                with pytest.raises(NotFoundError):
                    self.c.delete(t, f"{_UNIQUE}_Batch{i:02d}")
