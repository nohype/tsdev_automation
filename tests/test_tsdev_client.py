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

        def fake_run(cmd, **kw):
            manifest = cmd[-1]
            for line in Path(manifest).read_text(encoding="utf-8").splitlines():
                if line.strip() and not line.startswith("#"):
                    parts = line.split(None, 2)
                    if len(parts) == 3:
                        paths.append(parts[2])
            arr = json.dumps([json.loads(_success_json("insert", "strategy", "A"))])
            return _make_completed(arr)

        monkeypatch.setattr(subprocess, "run", fake_run)
        client.batch_insert([BatchItem("strategy", "A", "input(close,1);")])
        for p in paths:
            assert not os.path.isfile(p), f"temp source {p} must be cleaned up"


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
