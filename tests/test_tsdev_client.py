# SPDX-License-Identifier: MIT
"""Unit + E2E tests for the :class:`tsdev_automation.TsDevClient` wrapper.

Unit tests (default, fast) mock ``subprocess.run`` so they never touch the real
``tsdev_cli.exe`` or the TradeStation warehouse.  E2E tests (marked
``@pytest.mark.e2e``) drive the real CLI and are skipped unless the CLI binary
is discoverable (``TSDEV_CLI_PATH`` env var or the default ``build/Release``
path exists).
"""

from __future__ import annotations

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
    TsDevError,
    TsDevResult,
)
from tsdev_automation import exceptions as exc


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
    return TsDevClient(cli_path=str(fake_cli))


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
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS")
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
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS")
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
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS")
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
        c = TsDevClient(cli_path=str(fake_cli), ts_path="C:/TS")
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
        ]:
            cls = getattr(exc, name)
            assert issubclass(cls, TsDevError)
            assert issubclass(cls, Exception)

    def test_exception_carries_result(self) -> None:
        r = TsDevResult(op="insert", type="strategy", name="X", success=False, exit_code=8)
        e = CompileError(r)
        assert e.result is r


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
