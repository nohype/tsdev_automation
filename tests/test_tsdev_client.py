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
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import pytest

from tsdev_automation import (
    AlreadyExistsError,
    BatchItem,
    CompileError,
    NotFoundError,
    TsDevClient,
    TsDevCrashError,
    TsDevError,
    TsDevResult,
)
from tsdev_automation import exceptions as exc
from tsdev_automation import _pipe
from tsdev_automation.client import _ServeBroker, _detect_elevation, _is_crash_exit, _SHELLEXECUTEINFOW
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
    return TsDevClient(cli_path=str(fake_cli), use_serve=False)


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
                    "'Nope' not found or source unavailable (may be password-protected).",
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
        assert cmd == [str(client.cli_path), "batch-upsert", "--batch-stdin"]
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
        assert cmd == [str(client.cli_path), "batch-insert-dir", "strategy", "C:/mydir"]
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
        assert cmd == [str(client.cli_path), "batch-modify-dir", "strategy", "C:/mydir"]
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
        assert cmd == [str(client.cli_path), "batch-delete-dir", "strategy", "C:/mydir"]
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
        assert cmd == [str(client.cli_path), "batch-upsert-dir", "strategy", "C:/mydir"]
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


class TestCliMain:
    def test_main_insert_dispatch(self, monkeypatch, fake_cli: Path, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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

    The subcommands themselves are implemented in a later subtask; these tests
    are expected to FAIL (red) until the dispatch lands in :func:`_main` and
    :func:`_build_parser`.
    """

    def test_cli_exists_found(self, monkeypatch, capsys) -> None:
        calls: dict = {}

        class FakeClient:
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            def __init__(self, cli_path=None, ts_path=None, timeout=None):
                calls["init"] = {"cli_path": cli_path, "ts_path": ts_path, "timeout": timeout}

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
            "EllMissingError",
            "ThreadFailure",
            "TsDevCrashError",
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

    def test_start_raises_permission_error_on_uac_decline(self, monkeypatch, tmp_path: Path) -> None:
        """When ShellExecuteExW returns False, ``PermissionError`` is raised."""
        cli_exe = tmp_path / "tsdev_cli.exe"
        cli_exe.write_bytes(b"")  # start() validates the CLI path exists first.
        broker = _ServeBroker(cli_exe)
        monkeypatch.setattr(
            ctypes.windll.shell32, "ShellExecuteExW", lambda sei: False
        )
        monkeypatch.setattr(
            "tsdev_automation.client.tempfile.mkstemp",
            lambda *a, **k: (999, str(tmp_path / "x.pipe")),
        )
        monkeypatch.setattr("tsdev_automation.client.os.close", lambda fd: None)
        monkeypatch.setattr("tsdev_automation.client.os.unlink", lambda p: None)
        with pytest.raises(PermissionError):
            broker.start()

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


class TestServeRouting:
    """Unit tests for :class:`TsDevClient` serve-broker routing decisions."""

    def test_uses_subprocess_when_use_serve_false(self, fake_cli: Path, monkeypatch) -> None:
        """When ``use_serve=False`` is passed explicitly, the subprocess.run path is used."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        # Force _is_elevated to False so the only thing preventing serve is use_serve.
        c._is_elevated = False
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


class TestServeBatchNotImplemented:
    """Batch ops raise ``NotImplementedError`` when serve is active and not elevated."""

    def test_batch_insert_raises_when_serve_active(self, fake_cli: Path) -> None:
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        with pytest.raises(NotImplementedError, match="batch ops require an elevated session"):
            c.batch_insert([BatchItem("strategy", "A", "a;")])

    def test_batch_delete_raises_when_serve_active(self, fake_cli: Path) -> None:
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        with pytest.raises(NotImplementedError):
            c.batch_delete([BatchItem("strategy", "A")])

    def test_batch_upsert_raises_when_serve_active(self, fake_cli: Path) -> None:
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        with pytest.raises(NotImplementedError):
            c.batch_upsert([BatchItem("strategy", "A", "a;")])

    def test_batch_modify_raises_when_serve_active(self, fake_cli: Path) -> None:
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        with pytest.raises(NotImplementedError):
            c.batch_modify([BatchItem("strategy", "A", "a;")])

    def test_batch_insert_dir_raises_when_serve_active(self, fake_cli: Path) -> None:
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        with pytest.raises(NotImplementedError):
            c.batch_insert_dir("strategy", "C:/mydir")

    def test_batch_delete_dir_raises_when_serve_active(self, fake_cli: Path) -> None:
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = False
        with pytest.raises(NotImplementedError):
            c.batch_delete_dir("strategy", "C:/mydir")

    def test_batch_works_when_elevated_even_with_use_serve(self, fake_cli: Path, monkeypatch) -> None:
        """When elevated, batch ops use subprocess.run even if use_serve=True."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=True)
        c._is_elevated = True
        arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr))
        results = c.batch_insert([BatchItem("strategy", "A", "a;")])
        assert len(results) == 1 and results[0].success

    def test_batch_works_when_use_serve_false(self, fake_cli: Path, monkeypatch) -> None:
        """When use_serve=False, batch ops use subprocess.run regardless of elevation."""
        c = TsDevClient(cli_path=str(fake_cli), use_serve=False)
        c._is_elevated = False
        arr = json.dumps([json.loads(_success_json("delete", "strategy", "A"))])
        monkeypatch.setattr(subprocess, "run", lambda *a, **k: _make_completed(arr))
        results = c.batch_delete([BatchItem("strategy", "A")])
        assert len(results) == 1 and results[0].success


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
# EasyLanguage compiler via tsdev_cli -- all four compile to exit 0).
# Function source MUST assign to a variable named exactly like the component.
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
}

# All four supported component types.
_ALL_TYPES = ["strategy", "indicator", "function", "tradingapp"]


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
    ``_cleanup_all`` fixture, which sweeps all known test names across all four
    types so no orphans remain even if a test fails mid-way.
    """

    # Names used across the E2E suite.  The cleanup fixture sweeps ALL of these
    # across ALL four types so a failure mid-test cannot leak warehouse entries.
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
    ]

    @pytest.fixture(autouse=True)
    def _real_client(self):
        self.c = TsDevClient()
        yield
        # Sweep: best-effort delete of every test name across all 4 types.
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

        The source includes a ``{ name }`` EasyLanguage comment block (the
        convention used by ``enumerateRecords`` Phase 2 to extract the verbatim
        stored name when the warehouse record is not yet visible to the
        enumeration API).
        """
        name = "zzz_py_ci_probe"
        # Source with the { name } comment block so enumerateRecords Phase 2
        # can extract the verbatim stored name.
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

    # -- Part 4: orphan sweep (no leftover warehouse entries) ---------------

    def test_no_orphans_after_suite(self) -> None:
        """Final safety-net sweep: attempt to delete every test name across all
        4 types; each must raise NotFoundError, proving no orphaned warehouse
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
