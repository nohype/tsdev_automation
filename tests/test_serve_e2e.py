"""E2E test for serve mode (TVS-K.6a).

This test MUST run from a NON-elevated Python process. When run elevated,
it skips automatically (serve mode is not used when already elevated).

Run via: tests/run_serve_e2e.ps1 (launches this test unelevated via Task
Scheduler; the serve broker's normal elevation request is handled by Windows
UAC consent).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from tsdev_automation.client import TsDevClient, BatchItem, _detect_elevation
from tsdev_automation.exceptions import TsDevError, CompileError


def _find_cli() -> str:
    """Locate tsdev_cli.exe."""
    p = os.environ.get("TSDEV_CLI_PATH")
    if p and Path(p).is_file():
        return p
    candidate = Path(__file__).resolve().parent.parent / "build" / "Release" / "tsdev_cli.exe"
    if candidate.is_file():
        return str(candidate)
    pytest.skip("tsdev_cli.exe not found")


def _cleanup(client: TsDevClient, name: str) -> None:
    """Best-effort delete + broker shutdown."""
    try:
        client.delete("strategy", name)
    except Exception:
        pass
    if client._broker is not None:
        try:
            client._broker.shutdown()
        except Exception:
            pass


def _assert_cancelled_marker(result, op: str, type_: str, name: str) -> None:
    """Assert the exact synthetic not-run cancellation marker (TVS B)."""
    assert result.op == op, f"marker op must be {op!r}, got {result.op!r}"
    assert result.type == type_, f"marker type must be {type_!r}, got {result.type!r}"
    assert result.name == name, f"marker name must be {name!r}, got {result.name!r}"
    assert result.success is False, f"{name}: expected cancelled marker, got {result}"
    assert result.exit_code == 12, f"{name}: marker exit_code must be 12, got {result.exit_code}"
    assert result.error is not None, f"{name}: marker must carry an error payload"
    assert result.error.code == "cancelled", (
        f"{name}: marker error.code must be 'cancelled', got {result.error.code!r}"
    )
    assert result.error.message == "Batch cancelled before this item was attempted", (
        f"{name}: unexpected marker message {result.error.message!r}"
    )


@pytest.mark.e2e
class TestServeE2E:
    """Full lifecycle E2E test for serve mode (TVS-K.6a).

    Verifies that a non-elevated Python process can use TsDevClient(use_serve=True)
    to auto-start an elevated broker (one ShellExecuteExW "runas") and perform
    insert → exists → get-source → modify → get-source → delete → exists
    against the real TSDev Library, with only ONE broker process for all ops.
    """

    def test_serve_full_lifecycle(self):
        # Skip if elevated — serve mode only activates when NOT elevated
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "ServeE2eTestStrat"
        # Valid EasyLanguage strategy source (compiles successfully).
        # Uses the same pattern as tests/test_directory_batch.ps1.
        test_source = (
            "{ ServeE2eTest strategy }\n"
            "Inputs: FastLen(10), SlowLen(20);\n"
            "Variables: FastMA(0), SlowMA(0);\n"
            "FastMA = AverageFC(Close, FastLen);\n"
            "SlowMA = AverageFC(Close, SlowLen);\n"
            "if FastMA > SlowMA then Buy next bar at Market;\n"
            "if FastMA < SlowMA then SellShort next bar at Market;\n"
        )
        test_source_modified = (
            "{ ServeE2eTest modify }\n"
            "Inputs: FastLen(5), SlowLen(15);\n"
            "Variables: FastMA(0), SlowMA(0);\n"
            "FastMA = AverageFC(Close, FastLen);\n"
            "SlowMA = AverageFC(Close, SlowLen);\n"
            "if FastMA > SlowMA then Buy next bar at Market;\n"
            "if FastMA < SlowMA then SellShort next bar at Market;\n"
        )

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve should be active (not elevated + use_serve=True)"
        )

        broker_handle: int | None = None
        prog_id: int = 0

        try:
            # Pre-cleanup: delete any leftover from a previous failed run
            try:
                client.delete("strategy", test_name)
            except Exception:
                pass  # not found or broker start failure is fine for pre-cleanup

            # 1. Insert
            result = client.insert("strategy", test_name, test_source)
            assert result.success, f"insert failed: {result}"
            assert result.prog_id > 0, f"insert returned prog_id={result.prog_id}"
            prog_id = result.prog_id

            # Broker is lazy-started on first op — capture handle now
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, "broker should be started after first op"

            # 2. Exists (exact match)
            result = client.exists("strategy", test_name)
            assert result.exists, f"exists should find {test_name}"
            assert result.prog_id == prog_id, (
                f"exists prog_id mismatch: {result.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart between ops"
            )

            # 3. Get-source
            result = client.get_source("strategy", test_name)
            assert result.success, f"get-source failed: {result}"
            assert test_source in result.source, (
                f"source mismatch: expected {test_source!r} in {result.source!r}"
            )
            assert client.broker_process_handle() == broker_handle

            # 4. Modify
            result = client.modify("strategy", test_name, test_source_modified)
            assert result.success, f"modify failed: {result}"
            assert result.prog_id == prog_id, (
                f"modify should preserve prog_id: {result.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle

            # 5. Get-source again to verify modify took effect
            result = client.get_source("strategy", test_name)
            assert result.success, f"get-source after modify failed: {result}"
            assert test_source_modified in result.source, (
                f"modified source mismatch: expected {test_source_modified!r} "
                f"in {result.source!r}"
            )
            assert client.broker_process_handle() == broker_handle

            # 6. Delete
            result = client.delete("strategy", test_name)
            assert result.success, f"delete failed: {result}"
            assert client.broker_process_handle() == broker_handle

            # 7. Verify deleted
            result = client.exists("strategy", test_name)
            assert not result.exists, f"{test_name} should be deleted"
            assert client.broker_process_handle() == broker_handle

        finally:
            _cleanup(client, test_name)

    def test_serve_compile_failure_recovery(self):
        """Verify the broker survives a compile failure and create-path cleanup.

        Inserts invalid EL source (compile error triggers create-path cleanup
        of the partial warehouse entry via deleteEntry with flush=false), then
        verifies the broker is still alive and can service a subsequent op.
        This tests the critical K.6a / TVS-L.10 invariant: the create-path
        cleanup owner (doInsert) must use deleteEntry(flush=false) in serve
        mode so the warehouse stays open for the session lifetime.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "ServeE2eCompileFail"
        valid_name = "ServeE2eRecoveryStrat"
        # Invalid EL source — syntax error that will fail compilation
        invalid_source = "{ broken strategy }\nInputs: ;\n{ syntax error }\n"
        valid_source = (
            "{ Recovery strategy }\n"
            "Inputs: FastLen(10), SlowLen(20);\n"
            "Variables: FastMA(0), SlowMA(0);\n"
            "FastMA = AverageFC(Close, FastLen);\n"
            "SlowMA = AverageFC(Close, SlowLen);\n"
            "if FastMA > SlowMA then Buy next bar at Market;\n"
            "if FastMA < SlowMA then SellShort next bar at Market;\n"
        )

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active()

        broker_handle: int | None = None

        try:
            # Pre-cleanup
            for name in (test_name, valid_name):
                try:
                    client.delete("strategy", name)
                except Exception:
                    pass

            # 1. Insert invalid source — should raise CompileError
            with pytest.raises(CompileError):
                client.insert("strategy", test_name, invalid_source)

            # Capture broker handle after the failed insert
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, "broker should be alive after compile failure"

            # 2. Verify the failed item does NOT exist (create-path cleanup worked)
            result = client.exists("strategy", test_name)
            assert not result.exists, (
                f"{test_name} should not exist after compile-failure create-path cleanup"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart after compile failure + exists"
            )

            # 3. Insert valid source — broker should still work
            result = client.insert("strategy", valid_name, valid_source)
            assert result.success, (
                f"insert after compile failure should succeed: {result}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart after recovery insert"
            )

            # 4. Cleanup: delete the valid item
            client.delete("strategy", valid_name)
            assert client.broker_process_handle() == broker_handle

        finally:
            for name in (test_name, valid_name):
                _cleanup(client, name)

    # -- TVS-L.14b: real non-elevated Python batch-over-serve E2E -----------

    def test_serve_batch_over_serve_l14b(self):
        """TVS-L.14b GREEN (test-only): real non-elevated Python
        batch-over-serve E2E.

        Drives ``batch_insert`` -> ``batch_modify`` -> ``batch_upsert``
        (existing -> modify-path) -> ``exists`` -> ``batch_delete`` ->
        ``exists`` (after delete) entirely through the elevated serve broker
        from a NON-elevated Python process, against the real TSDev Library.

        Invariants verified (each is a hard failure if violated):

        1. **Fail if serve is bypassed.** ``client._serve_active()`` must be
           ``True`` (the serve path is active because the session is not
           elevated and ``use_serve`` defaults to ``True``). The pre-cleanup
           loop may start the broker (its ``delete`` calls go through the
           broker), so the handle may already be non-``None`` before the
           first batch op. The real bypass/lifetime proof is that the same
           broker handle, captured once and asserted identical after every
           subsequent batch op (Invariant 2), stays stable across the whole
           batch — proving the broker was actually started and used, not the
           ``subprocess.run`` direct path. If serve were bypassed (e.g.
           already elevated, or ``use_serve=False``), the handle would stay
           ``None`` and the Invariant 2 assertions fail.

        2. **Same broker / TsDevLib lifetime across the batch.** The broker
           process handle is captured once after the first batch op and
           asserted identical after every subsequent batch op (and the
           inter-batch ``exists`` queries). The serve broker holds a single
           long-lived ``TsDevLib`` (``flushPerItem=false``) for the whole
           session, so a stable handle proves no broker restart and no
           per-op warehouse flush — the core batch-over-serve efficiency
           invariant.

        3. **Python results match CLI JSON semantics.** Each per-item
           result is checked against the schema emitted by
           ``cliResultToJson``: ``op``, ``type``, ``name``, ``success``,
           ``exit_code`` (0 on success), ``prog_id`` (>0 on
           insert/modify/upsert), ``ell_path`` (non-empty on
           insert/modify/upsert). On ``batch_upsert`` of pre-existing items,
           ``prog_id`` must be preserved (modify-path), matching the C++
           batch-upsert semantics. On ``batch_delete`` success,
           ``ell_path`` is ``None`` (omitted in JSON, matching the C++
           delete result) and ``prog_id`` equals the former ProgId captured
           at insert (``prog_ids[i]``) — both the serve dispatch path and
           the C++ batch path emit the deleted item's former ProgId. Delete
           correctness is hard-asserted via ``success``, the former-ProgId
           match, and the post-delete ``exists`` (item gone).

        4. **Production-verified cleanup before emergency sanitation.** The
           test body explicitly runs ``batch_delete`` on all created items
           and asserts every delete succeeded — this is the production
           cleanup path, verified end-to-end through the broker. The
           ``finally`` block performs emergency sanitation (best-effort
           individual deletes + broker shutdown) ONLY as a safety net for
           the case where the body's verified cleanup did not run (e.g. an
           assertion failure mid-test).

        5. **Unique disposable names.** All items use the
           ``TS_AUTO_PY_SERVE_BATCH_L14B_<NN>`` prefix, unique to this test
           and disposable across runs.
        """
        # Skip if elevated — serve mode only activates when NOT elevated.
        # run_serve_e2e.ps1 launches this test unelevated via Task Scheduler.
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()

        # Unique disposable names for this test (TVS-L.14b).
        names = [f"TS_AUTO_PY_SERVE_BATCH_L14B_{i:02d}" for i in range(3)]

        # Valid EasyLanguage strategy sources (compile to exit 0). The insert
        # and modify sources differ so get-source/modify round-trips are
        # observable; both use the AverageFC pattern verified across the
        # existing serve E2E and directory-batch suites.
        def _ins_src(n: str) -> str:
            return (
                f"{{ L14b insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        def _mod_src(n: str) -> str:
            return (
                f"{{ L14b modify {n} }}\n"
                "Inputs: FastLen(5), SlowLen(15);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        client = TsDevClient(cli_path=cli_path)

        # --- Invariant 1: fail if serve is bypassed -----------------------
        # The serve path is active ONLY when not elevated and use_serve is
        # True (the default). If this is False, the batch ops below would
        # fall back to direct subprocess.run: the default
        # requireAdministrator manifest uses normal Windows UAC handling and
        # a launch that cannot be elevated may fail with
        # ERROR_ELEVATION_REQUIRED (Windows error 740) before the CLI starts;
        # an asInvoker build reaches the runtime guard and returns exit 11
        # (not_elevated). If elevated, the direct path bypasses the broker —
        # neither is what this test is verifying. Note: the pre-cleanup loop
        # below may already start
        # the broker (its delete calls route through serve), so the handle
        # may be non-None before the first batch op. The real bypass/lifetime
        # proof is the stable same-handle assertion across all batch ops
        # (Invariant 2), not the handle being None-vs-nonNone at this point.
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the batch-over-serve path is not being exercised"
        )

        broker_handle: int | None = None
        prog_ids: list[int] = []

        try:
            # Pre-cleanup: best-effort delete of any leftover from a previous
            # failed run. Done through the broker (the production path) so a
            # leftover item is removed by the same code path that created it.
            for n in names:
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass  # not-found or broker start failure is fine here

            # --- 1. batch_insert (3 strategies) ---------------------------
            ins_items = [BatchItem("strategy", n, _ins_src(n)) for n in names]
            ins_results = client.batch_insert(ins_items)
            assert len(ins_results) == 3, (
                f"batch_insert must return one result per item: {len(ins_results)}"
            )

            # Capture the broker handle now (it may already be non-None from
            # the pre-cleanup deletes, or it may be started by this
            # batch_insert). Either way it must be non-None here — if serve
            # were bypassed the handle would stay None and this fails. The
            # stronger bypass/lifetime proof is the same-handle assertion
            # across all subsequent batch ops (Invariant 2).
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker process handle must be non-None after first batch op "
                "(serve was bypassed -- batch_insert did not route through the broker)"
            )

            for r, n in zip(ins_results, names):
                # CLI JSON semantics (cliResultToJson success schema):
                # op/type/name/success/exit_code always present; prog_id>0
                # and ell_path non-empty on a successful insert.
                assert r.success, f"batch_insert {n} failed: {r}"
                assert r.op == "insert", f"op must be 'insert', got {r.op!r}"
                assert r.type == "strategy", f"type must be 'strategy', got {r.type!r}"
                assert r.name == n, f"name must be {n!r}, got {r.name!r}"
                assert r.exit_code == 0, f"exit_code must be 0, got {r.exit_code}"
                assert r.prog_id is not None and r.prog_id > 0, (
                    f"insert prog_id must be >0, got {r.prog_id}"
                )
                assert r.ell_path, f"insert ell_path must be non-empty, got {r.ell_path!r}"
                prog_ids.append(r.prog_id)

            # --- 2. batch_modify (same 3 strategies) ----------------------
            mod_items = [BatchItem("strategy", n, _mod_src(n)) for n in names]
            mod_results = client.batch_modify(mod_items)
            assert len(mod_results) == 3

            # Invariant 2: same broker / TsDevLib lifetime across the batch.
            assert client.broker_process_handle() == broker_handle, (
                "broker process handle must not change across batch_modify "
                "(broker restarted -- TsDevLib lifetime broken)"
            )

            for r, n, pid in zip(mod_results, names, prog_ids):
                assert r.success, f"batch_modify {n} failed: {r}"
                assert r.op == "modify", f"op must be 'modify', got {r.op!r}"
                assert r.type == "strategy"
                assert r.name == n
                assert r.exit_code == 0
                # ProgId must be preserved across modify (CLI JSON semantics).
                assert r.prog_id == pid, (
                    f"modify prog_id must be preserved ({pid}), got {r.prog_id}"
                )
                assert r.ell_path, f"modify ell_path must be non-empty, got {r.ell_path!r}"

            # --- 3. batch_upsert (all 3 pre-existing -> modify-path) ------
            # All items already exist, so each upsert takes the modify-path
            # and must preserve the ProgId (matches C++ batch-upsert).
            ups_items = [BatchItem("strategy", n, _ins_src(n)) for n in names]
            ups_results = client.batch_upsert(ups_items)
            assert len(ups_results) == 3

            assert client.broker_process_handle() == broker_handle, (
                "broker process handle must not change across batch_upsert"
            )

            for r, n, pid in zip(ups_results, names, prog_ids):
                assert r.success, f"batch_upsert {n} failed: {r}"
                assert r.op == "upsert", f"op must be 'upsert', got {r.op!r}"
                assert r.type == "strategy"
                assert r.name == n
                assert r.exit_code == 0
                # Upsert-existing (modify-path): ProgId preserved.
                assert r.prog_id == pid, (
                    f"upsert-existing prog_id must be preserved ({pid}), got {r.prog_id}"
                )
                assert r.ell_path, f"upsert ell_path must be non-empty, got {r.ell_path!r}"

            # --- 4. exists (inter-batch, through the broker) --------------
            # Verifies a single-item op also shares the same broker and that
            # the warehouse state reflects the batch inserts/modifies.
            ex = client.exists("strategy", names[0])
            assert ex.success, f"exists failed: {ex}"
            assert ex.exists is True, f"{names[0]} should exist after batch_insert"
            assert ex.prog_id == prog_ids[0], (
                f"exists prog_id must match insert ({prog_ids[0]}), got {ex.prog_id}"
            )
            assert ex.stored_name == names[0], (
                f"exists stored_name must be {names[0]!r}, got {ex.stored_name!r}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker process handle must not change across exists query"
            )

            # --- 5. batch_delete (production-verified cleanup) ------------
            # Invariant 4: the test body explicitly runs the production
            # batch_delete path and asserts every delete succeeded, BEFORE
            # any emergency sanitation in the finally block.
            del_items = [BatchItem("strategy", n) for n in names]
            del_results = client.batch_delete(del_items)
            assert len(del_results) == 3

            assert client.broker_process_handle() == broker_handle, (
                "broker process handle must not change across batch_delete"
            )

            for r, n, pid in zip(del_results, names, prog_ids):
                # Hard correctness assertions: the delete must succeed and
                # the result envelope must match the CLI JSON schema for a
                # delete (op/type/name/exit_code). ell_path must be None
                # (cliResultToJson omits it on delete). prog_id must equal
                # the former ProgId captured at insert (prog_ids[i]) — both
                # the serve dispatch path and the C++ batch path emit the
                # deleted item's former ProgId on a successful delete.
                assert r.success, f"batch_delete {n} failed: {r}"
                assert r.op == "delete", f"op must be 'delete', got {r.op!r}"
                assert r.type == "strategy"
                assert r.name == n
                assert r.exit_code == 0
                assert r.ell_path is None, (
                    f"delete ell_path must be None (omitted in JSON), got {r.ell_path!r}"
                )
                assert r.prog_id == pid, (
                    f"delete prog_id must equal former ProgId captured at "
                    f"insert ({pid}) for {n}, got {r.prog_id}"
                )

            # --- 6. exists after delete (through the broker) --------------
            ex2 = client.exists("strategy", names[0])
            assert ex2.success, f"exists after delete failed: {ex2}"
            assert ex2.exists is False, (
                f"{names[0]} should not exist after batch_delete"
            )
            assert ex2.prog_id == 0, (
                f"exists prog_id must be 0 after delete, got {ex2.prog_id}"
            )
            assert ex2.stored_name == "", (
                f"exists stored_name must be empty after delete, got {ex2.stored_name!r}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker process handle must not change across final exists query"
            )

        finally:
            # Invariant 4 (continued): emergency sanitation ONLY. This runs
            # only as a safety net for the case where the body's
            # production-verified batch_delete did not complete (e.g. an
            # assertion failure mid-test). It does best-effort individual
            # deletes of every created name, then shuts the broker down so
            # no elevated broker process is leaked.
            for n in names:
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_batch_mixed_outcome_continue_on_failure(self):
        """TVS-L.14c GREEN (test-only): mixed-outcome batch_insert over serve.

        Serve-mode analog of the DIRECT-mode
        ``test_e2e_batch_mixed_outcome_continue_on_failure`` in
        ``tests/test_tsdev_client.py``. Verifies that ``_batch_serve`` collects
        per-item failures into the result list instead of raising: one
        already-exists (item A), one new valid insert (item B), and one
        compile-error (item C) are submitted in a single ``batch_insert`` call,
        and the call returns normally with three results.

        Hard invariants:

        1. **Serve must not be bypassed.** ``client._serve_active()`` is true
           because the test runs non-elevated and ``use_serve`` defaults to
           ``True``.

        2. **Same broker / TsDevLib lifetime.** The broker handle captured
           after the pre-insert is identical across the mixed-outcome batch,
           the inter-batch ``exists`` queries, and the production-verified
           ``batch_delete`` cleanup.

        3. **Per-item outcomes match CLI JSON semantics.** Item A reports
           ``exit_code==5`` with ``error.code == "already_exists"`` and the
           correct ``existing_prog_id``; item B reports ``exit_code==0`` with a
           new ``prog_id`` and a non-empty ``ell_path``; item C reports
           ``exit_code==8`` with ``error.code == "compile_error"``.

        4. **Persistence and create-path cleanup.** Item B exists with the
           ProgId returned by the batch. Item A is unchanged. Item C was never
           created because the compiler failed and the create-path cleanup
           removed the partial warehouse entry.

        5. **Divergence from the DIRECT test.** The type is ``"strategy"``
           (not ``"function"``), and the compile-error source is the same
           invalid strategy source already proven in this file's
           ``test_serve_compile_failure_recovery``.

        6. **Production-verified cleanup.** The body runs ``batch_delete`` on
           the two created items (A and B) and asserts success before the
           ``finally`` emergency sanitation.
        """
        # Skip if elevated — serve mode only activates when NOT elevated.
        # run_serve_e2e.ps1 launches this test unelevated via Task Scheduler.
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()

        # Unique disposable names for this test (TVS-L.14c).
        name_a = "TS_AUTO_PY_SERVE_MIX_L14C_A"
        name_b = "TS_AUTO_PY_SERVE_MIX_L14C_B"
        name_c = "TS_AUTO_PY_SERVE_MIX_L14C_C"

        # Valid EasyLanguage strategy source (compiles to exit 0). Uses the
        # same AverageFC pattern as the L14b serve batch test.
        def _ins_src(n: str) -> str:
            return (
                f"{{ L14c insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        client = TsDevClient(cli_path=cli_path)

        # Fail if serve is bypassed (non-elevated + use_serve=True).
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the mixed-outcome batch path is not being exercised"
        )

        broker_handle: int | None = None
        prog_id_a: int = 0

        try:
            # Pre-cleanup: best-effort delete of any leftover from a previous
            # failed run. Done through the broker (the production path).
            for n in (name_a, name_b, name_c):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass

            # Pre-insert item A so it is present for the batch's duplicate check.
            r = client.insert("strategy", name_a, _ins_src(name_a))
            assert r.success, f"pre-insert {name_a} failed: {r}"
            prog_id_a = r.prog_id
            assert prog_id_a is not None and prog_id_a > 0

            # Capture the broker handle after the pre-insert. If serve were
            # bypassed the handle would be None here.
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after pre-insert (serve bypassed)"
            )

            # Mixed-outcome batch: A already exists, B is new and valid,
            # C is a compile error. The call must return normally (continue
            # on failure) and the broker handle must remain stable.
            results = client.batch_insert([
                BatchItem("strategy", name_a, _ins_src(name_a)),
                BatchItem("strategy", name_b, _ins_src(name_b)),
                BatchItem("strategy", name_c, "{ broken strategy }\nInputs: ;\n{ syntax error }\n"),
            ])

            assert len(results) == 3
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across the mixed-outcome batch"
            )

            # Item A: duplicate insert.
            assert results[0].success is False
            assert results[0].op == "insert"
            assert results[0].type == "strategy"
            assert results[0].name == name_a
            assert results[0].exit_code == 5
            assert results[0].error is not None and results[0].error.code == "already_exists"
            assert results[0].error.existing_prog_id == prog_id_a, (
                f"existing_prog_id must be {prog_id_a}, "
                f"got {results[0].error.existing_prog_id}"
            )

            # Item B: successful new insert.
            assert results[1].success is True
            assert results[1].op == "insert"
            assert results[1].type == "strategy"
            assert results[1].name == name_b
            assert results[1].exit_code == 0
            assert results[1].prog_id is not None and results[1].prog_id > 0
            assert results[1].ell_path, (
                f"insert ell_path must be non-empty, got {results[1].ell_path!r}"
            )

            # Item C: compile error.
            assert results[2].success is False
            assert results[2].op == "insert"
            assert results[2].type == "strategy"
            assert results[2].name == name_c
            assert results[2].exit_code == 8
            assert results[2].error is not None and results[2].error.code == "compile_error"

            # Post-batch persistence (through the broker, same handle each time).
            ex_b = client.exists("strategy", name_b)
            assert ex_b.exists and ex_b.prog_id == results[1].prog_id
            assert client.broker_process_handle() == broker_handle

            ex_a = client.exists("strategy", name_a)
            assert ex_a.exists and ex_a.prog_id == prog_id_a
            assert client.broker_process_handle() == broker_handle

            ex_c = client.exists("strategy", name_c)
            assert not ex_c.exists
            assert client.broker_process_handle() == broker_handle

            # Production-verified cleanup in the body (C was never created).
            del_results = client.batch_delete([
                BatchItem("strategy", name_a),
                BatchItem("strategy", name_b),
            ])
            assert len(del_results) == 2
            for r in del_results:
                assert r.success
            assert client.broker_process_handle() == broker_handle

            # Post-cleanup verification.
            assert not client.exists("strategy", name_a).exists
            assert not client.exists("strategy", name_b).exists

        finally:
            # Emergency sanitation ONLY. Best-effort individual deletes of every
            # name that could have been created, then broker shutdown so no
            # elevated broker process is leaked.
            for n in (name_a, name_b, name_c):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    # -- TVS-L.14d: serve batch upsert insert-path + modify-path ----------

    def test_serve_batch_upsert_both_branches(self):
        """TVS-L.14d GREEN (test-only): serve batch_upsert exercises both
        insert-path and modify-path in a single batch.

        Closes the serve-mode upsert insert-path gap: the existing L14b test
        only covered ``batch_upsert`` on pre-existing items (modify-path).
        This test submits a mixed batch (two new items + one pre-existing item)
        through ``_batch_serve`` and verifies that new items take the
        insert-path and receive fresh ProgIds while pre-existing items take the
        modify-path and preserve their ProgId. It also asserts same broker
        handle stability across the whole batch and post-batch persistence
        through the broker.

        Mirrors the DIRECT-mode upsert-both-branches coverage but in serve mode.
        """
        # Skip if elevated — serve mode only activates when NOT elevated.
        # run_serve_e2e.ps1 launches this test unelevated via Task Scheduler.
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()

        # Unique disposable names for this test (TVS-L.14d).
        name_new1 = "TS_AUTO_PY_SERVE_UPS_L14D_NEW1"
        name_new2 = "TS_AUTO_PY_SERVE_UPS_L14D_NEW2"
        name_exist = "TS_AUTO_PY_SERVE_UPS_L14D_EXIST"

        # Valid EasyLanguage strategy source. The upsert batch uses the insert
        # source, matching L14b's upsert call.
        def _ins_src(n: str) -> str:
            return (
                f"{{ L14d insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        client = TsDevClient(cli_path=cli_path)

        # Fail if serve is bypassed (non-elevated + use_serve=True).
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the upsert batch path is not being exercised"
        )

        broker_handle: int | None = None
        prog_id_exist: int = 0

        try:
            # Pre-cleanup: best-effort delete of any leftover from a previous
            # failed run. Done through the broker (the production path).
            for n in (name_new1, name_new2, name_exist):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass

            # Pre-insert the existing item so the upsert takes the modify-path.
            r = client.insert("strategy", name_exist, _ins_src(name_exist))
            assert r.success, f"pre-insert {name_exist} failed: {r}"
            prog_id_exist = r.prog_id
            assert prog_id_exist is not None and prog_id_exist > 0

            # Capture the broker handle after the pre-insert.
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after pre-insert (serve was bypassed)"
            )

            # Mixed-branch upsert batch: two new items (insert-path) and one
            # pre-existing item (modify-path) in the same call.
            results = client.batch_upsert([
                BatchItem("strategy", name_new1, _ins_src(name_new1)),
                BatchItem("strategy", name_exist, _ins_src(name_exist)),
                BatchItem("strategy", name_new2, _ins_src(name_new2)),
            ])

            assert len(results) == 3
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across the mixed-branch upsert batch"
            )

            # Item name_new1: new -> insert-path.
            assert results[0].success is True
            assert results[0].op == "upsert"
            assert results[0].type == "strategy"
            assert results[0].name == name_new1
            assert results[0].exit_code == 0
            assert results[0].prog_id is not None and results[0].prog_id > 0
            assert results[0].ell_path, (
                f"upsert ell_path must be non-empty, got {results[0].ell_path!r}"
            )

            # Item name_exist: pre-existing -> modify-path, ProgId preserved.
            assert results[1].success is True
            assert results[1].op == "upsert"
            assert results[1].type == "strategy"
            assert results[1].name == name_exist
            assert results[1].exit_code == 0
            assert results[1].prog_id == prog_id_exist, (
                f"upsert modify-path must preserve prog_id ({prog_id_exist}), got {results[1].prog_id}"
            )
            assert results[1].ell_path, (
                f"upsert ell_path must be non-empty, got {results[1].ell_path!r}"
            )

            # Item name_new2: new -> insert-path.
            assert results[2].success is True
            assert results[2].op == "upsert"
            assert results[2].type == "strategy"
            assert results[2].name == name_new2
            assert results[2].exit_code == 0
            assert results[2].prog_id is not None and results[2].prog_id > 0
            assert results[2].ell_path, (
                f"upsert ell_path must be non-empty, got {results[2].ell_path!r}"
            )

            # The two new items must have ProgIds distinct from each other and
            # from the pre-existing item.
            assert results[0].prog_id != prog_id_exist
            assert results[2].prog_id != prog_id_exist
            assert results[0].prog_id != results[2].prog_id, (
                "two new upsert items must get distinct prog_ids"
            )

            # Post-batch persistence (through the broker, same handle each time).
            ex_new1 = client.exists("strategy", name_new1)
            assert ex_new1.exists and ex_new1.prog_id == results[0].prog_id
            assert client.broker_process_handle() == broker_handle

            ex_exist = client.exists("strategy", name_exist)
            assert ex_exist.exists and ex_exist.prog_id == prog_id_exist
            assert client.broker_process_handle() == broker_handle

            ex_new2 = client.exists("strategy", name_new2)
            assert ex_new2.exists and ex_new2.prog_id == results[2].prog_id
            assert client.broker_process_handle() == broker_handle

            # Production-verified cleanup in the body.
            del_results = client.batch_delete([
                BatchItem("strategy", name_new1),
                BatchItem("strategy", name_exist),
                BatchItem("strategy", name_new2),
            ])
            assert len(del_results) == 3
            for r in del_results:
                assert r.success
            assert client.broker_process_handle() == broker_handle

            # Post-cleanup verification.
            for n in (name_new1, name_exist, name_new2):
                assert not client.exists("strategy", n).exists

        finally:
            # Emergency sanitation ONLY. Best-effort individual deletes of every
            # name that could have been created, then broker shutdown so no
            # elevated broker process is leaked.
            for n in (name_new1, name_new2, name_exist):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    # -- TVS-L.16: serve directory-batch E2E -------------------------------

    def test_serve_batch_insert_dir_then_delete_dir(self) -> None:
        """TVS-L.16 GREEN (test-only): serve batch_insert_dir then
        batch_delete_dir on a temp directory of .el files.

        Serve-mode analog of the DIRECT-mode
        ``test_e2e_batch_insert_dir_then_delete_dir`` in
        ``tests/test_tsdev_client.py``. Verifies that ``_batch_dir_serve``
        routes insert and delete directory batches through a single broker,
        returns per-item results, and preserves the same broker handle across
        both operations.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")
        cli_path = _find_cli()

        names = [
            "TS_AUTO_PY_SERVE_DIR_L16_A",
            "TS_AUTO_PY_SERVE_DIR_L16_B",
            "TS_AUTO_PY_SERVE_DIR_L16_C",
        ]

        def _ins_src(n: str) -> str:
            return (
                f"{{ L16 insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        d = Path(tempfile.mkdtemp(prefix="tsdev_serve_l16_"))
        for n in names:
            (d / f"{n}.el").write_text(_ins_src(n), encoding="utf-8")

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the batch-over-serve path is not being exercised"
        )

        broker_handle: int | None = None

        try:
            for n in names:
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass

            results = client.batch_insert_dir("strategy", str(d))
            assert len(results) == 3, (
                f"batch_insert_dir must return one result per file: {results}"
            )
            assert all(r.success for r in results), results

            for r in results:
                assert r.prog_id is not None and r.prog_id > 0, (
                    f"insert_dir prog_id must be >0, got {r.prog_id}"
                )
                assert r.ell_path, (
                    f"insert_dir ell_path must be non-empty, got {r.ell_path!r}"
                )

            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after batch_insert_dir (serve bypassed)"
            )

            for r in results:
                ex = client.exists("strategy", r.name)
                assert ex.exists, f"{r.name} should exist after batch_insert_dir"
                assert ex.prog_id == r.prog_id, (
                    f"exists prog_id mismatch for {r.name}: "
                    f"{ex.prog_id} vs {r.prog_id}"
                )
                assert client.broker_process_handle() == broker_handle, (
                    "broker handle must not change across post-insert exists"
                )

            del_results = client.batch_delete_dir("strategy", str(d))
            assert len(del_results) == 3, del_results
            assert all(r.success for r in del_results), del_results
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across batch_delete_dir"
            )

            for n in names:
                assert not client.exists("strategy", n).exists, (
                    f"{n} should not exist after batch_delete_dir"
                )
                assert client.broker_process_handle() == broker_handle

        finally:
            shutil.rmtree(d, ignore_errors=True)
            for n in names:
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_batch_upsert_dir_mixed(self) -> None:
        """TVS-L.16 GREEN (test-only): serve batch_upsert_dir with one
        pre-existing (modify-path) and one new (insert-path) .el file.

        Serve-mode analog of the DIRECT-mode
        ``test_e2e_batch_upsert_dir_mixed`` in ``tests/test_tsdev_client.py``.
        Verifies that the pre-existing item preserves its ProgId and the new
        item receives a fresh ProgId, with the same broker handle across the
        whole directory upsert.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")
        cli_path = _find_cli()

        name_a = "TS_AUTO_PY_SERVE_DIR_L16_UA"
        name_b = "TS_AUTO_PY_SERVE_DIR_L16_UB"

        def _ins_src(n: str) -> str:
            return (
                f"{{ L16 insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        def _mod_src(n: str) -> str:
            return (
                f"{{ L16 modify {n} }}\n"
                "Inputs: FastLen(20), SlowLen(40);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then SellShort next bar at Market;\n"
                "if FastMA < SlowMA then Buy next bar at Market;\n"
            )

        d = Path(tempfile.mkdtemp(prefix="tsdev_serve_l16_"))
        (d / f"{name_a}.el").write_text(_mod_src(name_a), encoding="utf-8")
        (d / f"{name_b}.el").write_text(_ins_src(name_b), encoding="utf-8")

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the batch-over-serve path is not being exercised"
        )

        broker_handle: int | None = None
        prog_id_a: int = 0

        try:
            for n in (name_a, name_b):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass

            r = client.insert("strategy", name_a, _ins_src(name_a))
            assert r.success, f"pre-insert {name_a} failed: {r}"
            prog_id_a = r.prog_id
            assert prog_id_a is not None and prog_id_a > 0

            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after pre-insert (serve bypassed)"
            )

            results = client.batch_upsert_dir("strategy", str(d))
            assert len(results) == 2, results
            assert all(r.success for r in results), results
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across batch_upsert_dir"
            )

            a_result = [r for r in results if r.name == name_a][0]
            b_result = [r for r in results if r.name == name_b][0]

            assert a_result.prog_id == prog_id_a, (
                f"upsert modify-path must preserve prog_id ({prog_id_a}), "
                f"got {a_result.prog_id}"
            )
            assert a_result.ell_path, (
                f"upsert modify-path ell_path must be non-empty, "
                f"got {a_result.ell_path!r}"
            )
            assert b_result.prog_id is not None and b_result.prog_id > 0, (
                f"upsert insert-path prog_id must be >0, got {b_result.prog_id}"
            )
            assert b_result.ell_path, (
                f"upsert insert-path ell_path must be non-empty, "
                f"got {b_result.ell_path!r}"
            )
            assert b_result.prog_id != prog_id_a

            ex_a = client.exists("strategy", name_a)
            assert ex_a.exists and ex_a.prog_id == prog_id_a
            assert client.broker_process_handle() == broker_handle

            ex_b = client.exists("strategy", name_b)
            assert ex_b.exists and ex_b.prog_id == b_result.prog_id
            assert client.broker_process_handle() == broker_handle

            del_results = client.batch_delete_dir("strategy", str(d))
            assert len(del_results) == 2, del_results
            assert all(r.success for r in del_results), del_results
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across batch_delete_dir"
            )

            for n in (name_a, name_b):
                assert not client.exists("strategy", n).exists
                assert client.broker_process_handle() == broker_handle

        finally:
            shutil.rmtree(d, ignore_errors=True)
            for n in (name_a, name_b):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_batch_modify_dir(self) -> None:
        """TVS-L.16 GREEN (test-only): serve batch_insert then
        batch_modify_dir with modified source.

        Both items are pre-inserted, then batch_modify_dir is called on a
        directory containing modified source files. ProgIds must be preserved
        across the modify. Serve-mode analog of the DIRECT-mode
        ``test_e2e_batch_modify_dir`` in ``tests/test_tsdev_client.py``.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")
        cli_path = _find_cli()

        name_a = "TS_AUTO_PY_SERVE_DIR_L16_MA"
        name_b = "TS_AUTO_PY_SERVE_DIR_L16_MB"

        def _ins_src(n: str) -> str:
            return (
                f"{{ L16 insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        def _mod_src(n: str) -> str:
            return (
                f"{{ L16 modify {n} }}\n"
                "Inputs: FastLen(20), SlowLen(40);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then SellShort next bar at Market;\n"
                "if FastMA < SlowMA then Buy next bar at Market;\n"
            )

        d = Path(tempfile.mkdtemp(prefix="tsdev_serve_l16_"))
        (d / f"{name_a}.el").write_text(_mod_src(name_a), encoding="utf-8")
        (d / f"{name_b}.el").write_text(_mod_src(name_b), encoding="utf-8")

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the batch-over-serve path is not being exercised"
        )

        broker_handle: int | None = None
        prog_id_a: int = 0
        prog_id_b: int = 0

        try:
            for n in (name_a, name_b):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass

            ins = client.batch_insert([
                BatchItem("strategy", name_a, _ins_src(name_a)),
                BatchItem("strategy", name_b, _ins_src(name_b)),
            ])
            assert len(ins) == 2, ins
            assert all(r.success for r in ins), ins
            prog_id_a = ins[0].prog_id
            prog_id_b = ins[1].prog_id
            assert prog_id_a is not None and prog_id_a > 0
            assert prog_id_b is not None and prog_id_b > 0

            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after batch_insert (serve bypassed)"
            )

            results = client.batch_modify_dir("strategy", str(d))
            assert len(results) == 2, results
            assert all(r.success for r in results), results
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across batch_modify_dir"
            )

            a_result = [r for r in results if r.name == name_a][0]
            b_result = [r for r in results if r.name == name_b][0]
            assert a_result.prog_id == prog_id_a, (
                f"modify {name_a} must preserve prog_id ({prog_id_a}), "
                f"got {a_result.prog_id}"
            )
            assert b_result.prog_id == prog_id_b, (
                f"modify {name_b} must preserve prog_id ({prog_id_b}), "
                f"got {b_result.prog_id}"
            )

            ex_a = client.exists("strategy", name_a)
            assert ex_a.exists and ex_a.prog_id == prog_id_a
            assert client.broker_process_handle() == broker_handle

            ex_b = client.exists("strategy", name_b)
            assert ex_b.exists and ex_b.prog_id == prog_id_b
            assert client.broker_process_handle() == broker_handle

            del_results = client.batch_delete_dir("strategy", str(d))
            assert len(del_results) == 2, del_results
            assert all(r.success for r in del_results), del_results
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across batch_delete_dir"
            )

            for n in (name_a, name_b):
                assert not client.exists("strategy", n).exists
                assert client.broker_process_handle() == broker_handle

        finally:
            shutil.rmtree(d, ignore_errors=True)
            for n in (name_a, name_b):
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_batch_insert_dir_t_override(self) -> None:
        """TVS-L.16 GREEN (test-only): serve batch_insert_dir with
        extensions=['txt'] on a .txt-only directory.

        Serve-mode analog of the DIRECT-mode
        ``test_e2e_batch_dir_t_override`` in ``tests/test_tsdev_client.py``.
        Verifies that passing ``extensions=['txt']`` overrides the default
        ``.el`` + ``.txt`` filter so the .txt file is picked up and inserted
        through the serve broker.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")
        cli_path = _find_cli()

        name = "TS_AUTO_PY_SERVE_DIR_L16_TXT"

        def _ins_src(n: str) -> str:
            return (
                f"{{ L16 insert {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        d = Path(tempfile.mkdtemp(prefix="tsdev_serve_l16_"))
        (d / f"{name}.txt").write_text(_ins_src(name), encoding="utf-8")

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the batch-over-serve path is not being exercised"
        )

        broker_handle: int | None = None

        try:
            try:
                client.delete("strategy", name)
            except Exception:
                pass

            results = client.batch_insert_dir(
                "strategy", str(d), extensions=["txt"]
            )
            assert len(results) == 1, results
            assert all(r.success for r in results), results

            r = results[0]
            assert r.prog_id is not None and r.prog_id > 0, (
                f"insert_dir prog_id must be >0, got {r.prog_id}"
            )
            assert r.ell_path, (
                f"insert_dir ell_path must be non-empty, got {r.ell_path!r}"
            )

            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after batch_insert_dir (serve bypassed)"
            )

            ex = client.exists("strategy", name)
            assert ex.exists and ex.prog_id == r.prog_id
            assert client.broker_process_handle() == broker_handle

            del_results = client.batch_delete_dir(
                "strategy", str(d), extensions=["txt"]
            )
            assert len(del_results) == 1, del_results
            assert all(r.success for r in del_results), del_results
            assert client.broker_process_handle() == broker_handle, (
                "broker handle must not change across batch_delete_dir"
            )

            ex2 = client.exists("strategy", name)
            assert not ex2.exists
            assert client.broker_process_handle() == broker_handle

        finally:
            shutil.rmtree(d, ignore_errors=True)
            try:
                client.delete("strategy", name)
            except Exception:
                pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    # -- TVS B: serve on_progress callback E2E -----------------------------

    def test_serve_batch_on_progress_tvsb(self):
        """TVS B live: ``on_progress`` callbacks over the real serve broker.

        Non-elevated auto-broker E2E proving the serve batch progress
        contract end-to-end:

        1. ``BatchProgress`` callbacks fire synchronously in input order
           after every attempted result, and a per-item failure does not
           stop the batch (callback-driven continuation).
        2. ``return False`` cancels before the next item — after a success
           AND after a failure — and the returned list stays full length:
           unattempted items become exit-12 ``cancelled`` markers that the
           callback never sees.
        3. One stable broker process serves every batch call and remains
           usable after cancellations.
        4. UUID-derived disposable names; exact cleanup in ``finally``.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        import uuid

        from tsdev_automation import BatchProgress

        cli_path = _find_cli()

        # UUID-derived disposable names: no fixed-name collisions across runs.
        uid = uuid.uuid4().hex[:8].upper()
        name_a = f"TS_B_{uid}_A"      # inserted, deleted by cancel-after-success
        name_b = f"TS_B_{uid}_B"      # inserted, marker twice, deleted singly
        name_c = f"TS_B_{uid}_C"      # never attempted (cancelled marker)
        name_bad = f"TS_B_{uid}_BAD"  # compile-failure item (never exists)
        name_ghost = f"TS_B_{uid}_GHOST"  # never created (not-found delete)
        names = [name_a, name_b, name_c, name_bad, name_ghost]

        def _ins_src(n: str) -> str:
            return (
                f"{{ TVS B serve progress {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        invalid_source = "{ TVS B broken }\nInputs: ;\n{ syntax error }\n"

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True); "
            "if this fails the batch-over-serve path is not being exercised"
        )

        broker_handle: int | None = None

        try:
            # Pre-cleanup: best-effort delete of leftovers through the broker.
            for n in names:
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass

            # --- 1. Callback continuation after an item failure -----------
            events: list = []
            results = client.batch_insert(
                [
                    BatchItem("strategy", name_a, _ins_src(name_a)),
                    BatchItem("strategy", name_bad, invalid_source),
                    BatchItem("strategy", name_b, _ins_src(name_b)),
                ],
                on_progress=events.append,
            )
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker must be started after batch_insert (serve bypassed)"
            )
            assert len(results) == 3
            assert len(events) == 3
            assert all(isinstance(p, BatchProgress) for p in events)
            assert [p.index for p in events] == [1, 2, 3]
            assert [p.total for p in events] == [3, 3, 3]
            for p, r in zip(events, results):
                assert p.result == r
            assert results[0].success and results[0].name == name_a
            assert results[1].success is False and results[1].exit_code == 8
            assert results[1].error is not None and results[1].error.code == "compile_error"
            # The third item was attempted despite the second item's failure.
            assert results[2].success and results[2].name == name_b

            assert client.broker_process_handle() == broker_handle
            assert client.exists("strategy", name_a).exists
            assert client.exists("strategy", name_b).exists
            assert not client.exists("strategy", name_bad).exists
            assert client.broker_process_handle() == broker_handle

            # --- 2. Cancel after a success --------------------------------
            events2: list = []

            def _cancel(p) -> bool:
                events2.append(p)
                return False

            results2 = client.batch_delete(
                [
                    BatchItem("strategy", name_a),
                    BatchItem("strategy", name_b),
                    BatchItem("strategy", name_c),
                ],
                on_progress=_cancel,
            )
            assert client.broker_process_handle() == broker_handle
            assert len(results2) == 3, "cancelled batch must return a full-length list"
            assert len(events2) == 1, "callback must not be invoked for markers"
            assert results2[0].success
            _assert_cancelled_marker(results2[1], "delete", "strategy", name_b)
            _assert_cancelled_marker(results2[2], "delete", "strategy", name_c)
            assert not client.exists("strategy", name_a).exists
            assert client.exists("strategy", name_b).exists
            assert not client.exists("strategy", name_c).exists
            assert client.broker_process_handle() == broker_handle

            # --- 3. Cancel after a failure --------------------------------
            events3: list = []

            def _cancel3(p) -> bool:
                events3.append(p)
                return False

            results3 = client.batch_delete(
                [BatchItem("strategy", name_ghost), BatchItem("strategy", name_b)],
                on_progress=_cancel3,
            )
            assert client.broker_process_handle() == broker_handle
            assert len(results3) == 2
            assert len(events3) == 1, "callback must not be invoked for markers"
            assert results3[0].success is False
            assert results3[0].exit_code == 5
            assert results3[0].error is not None
            assert results3[0].error.code == "not_found"
            _assert_cancelled_marker(results3[1], "delete", "strategy", name_b)
            assert client.exists("strategy", name_b).exists
            assert client.broker_process_handle() == broker_handle

            # --- 4. Broker still usable after cancellations ---------------
            d = client.delete("strategy", name_b)
            assert d.success, f"delete after cancelled batches failed: {d}"
            assert not client.exists("strategy", name_b).exists
            assert client.broker_process_handle() == broker_handle

        finally:
            # Exact cleanup: every name this test could have created, then
            # broker shutdown so no elevated broker process is leaked.
            for n in names:
                try:
                    client.delete("strategy", n)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    # -- TVS-L.17: serve single-op upsert ----------------------------------

    def test_serve_single_op_upsert(self):
        """TVS-L.17 GREEN (test-only): serve single-op upsert covers both
        the insert-path and modify-path through one elevated broker.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "TS_AUTO_PY_SERVE_UPSERT_L17_001"

        # Valid EasyLanguage strategy source (compiles successfully).
        # Uses the same minimal one-line strategy as TestE2E.test_strategy_lifecycle.
        test_source = (
            "Inputs: Length(10); If Close > Average(Close, Length) Then Buy next bar at Market;\n"
        )
        test_source_modified = (
            "Inputs: Length(20); If Close > Average(Close, Length) Then Sell next bar at Market;\n"
        )

        client = TsDevClient(cli_path=cli_path, use_serve=True)
        assert client._serve_active(), (
            "serve should be active (not elevated + use_serve=True)"
        )

        broker_handle: int | None = None
        prog_id: int = 0

        try:
            # Pre-cleanup: delete any leftover from a previous failed run
            try:
                client.delete("strategy", test_name)
            except Exception:
                pass

            # 1. Upsert on a new name -> insert-path
            result = client.upsert("strategy", test_name, test_source)
            assert result.success, f"upsert insert-path failed: {result}"
            assert result.prog_id is not None and result.prog_id > 0, (
                f"upsert insert-path returned prog_id={result.prog_id}"
            )
            assert result.ell_path, (
                f"upsert insert-path ell_path must be non-empty, got {result.ell_path!r}"
            )
            prog_id = result.prog_id

            # Broker is lazy-started on first op — capture handle now
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, (
                "broker should be started after first upsert"
            )

            # 2. Upsert on the same name with modified source -> modify-path
            result = client.upsert("strategy", test_name, test_source_modified)
            assert result.success, f"upsert modify-path failed: {result}"
            assert result.prog_id == prog_id, (
                f"upsert modify-path must preserve prog_id: {result.prog_id} vs {prog_id}"
            )
            assert result.ell_path, (
                f"upsert modify-path ell_path must be non-empty, got {result.ell_path!r}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart between upsert calls"
            )

            # 3. Verify existence and ProgId match
            ex = client.exists("strategy", test_name)
            assert ex.success, f"exists failed: {ex}"
            assert ex.exists, f"{test_name} should exist after upsert"
            assert ex.prog_id == prog_id, (
                f"exists prog_id mismatch: {ex.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle

            # 4. Production-verified cleanup
            d = client.delete("strategy", test_name)
            assert d.success, f"delete failed: {d}"
            assert client.broker_process_handle() == broker_handle

            ex2 = client.exists("strategy", test_name)
            assert not ex2.exists, f"{test_name} should be deleted after cleanup"
            assert client.broker_process_handle() == broker_handle

        finally:
            # Emergency sanitation: best-effort delete and broker shutdown
            try:
                client.delete("strategy", test_name)
            except Exception:
                pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    # -- TVS-L.18: serve per-type single-op lifecycle ---------------------

    def test_serve_strategy_lifecycle_l18(self):
        """TVS-L.18 GREEN (test-only): serve single-op lifecycle for strategy
        (insert -> modify -> delete) through one elevated broker.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "TS_AUTO_PY_SERVE_LIFE_L18_001"

        test_source = (
            "Inputs: Length(10); If Close > Average(Close, Length) Then Buy next bar at Market;\n"
        )
        test_source_modified = (
            "Inputs: Length(20); If Close > Average(Close, Length) Then Sell next bar at Market;\n"
        )

        client = TsDevClient(cli_path=cli_path, use_serve=True)
        assert client._serve_active(), (
            "serve should be active (not elevated + use_serve=True)"
        )

        broker_handle: int | None = None
        prog_id: int = 0

        try:
            # Pre-cleanup: delete any leftover from a previous failed run
            try:
                client.delete("strategy", test_name)
            except Exception:
                pass

            # 1. Insert
            r = client.insert("strategy", test_name, test_source)
            assert r.success, f"insert failed: {r}"
            assert r.prog_id is not None and r.prog_id > 0, (
                f"insert returned prog_id={r.prog_id}"
            )
            assert r.ell_path, (
                f"insert ell_path must be non-empty, got {r.ell_path!r}"
            )
            prog_id = r.prog_id

            # Broker is lazy-started on first op — capture handle now
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, "broker should be started after first op"

            # 2. Modify
            m = client.modify("strategy", test_name, test_source_modified)
            assert m.success, f"modify failed: {m}"
            assert m.prog_id == prog_id, (
                f"modify should preserve prog_id: {m.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart between ops"
            )

            # 3. Delete
            d = client.delete("strategy", test_name)
            assert d.success, f"delete failed: {d}"
            assert client.broker_process_handle() == broker_handle

        finally:
            # Emergency sanitation: best-effort delete and broker shutdown
            try:
                client.delete("strategy", test_name)
            except Exception:
                pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_function_lifecycle_l18(self):
        """TVS-L.18 GREEN (test-only): serve single-op lifecycle for function
        (insert -> modify -> delete) through one elevated broker.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "TS_AUTO_PY_SERVE_LIFE_L18_002"

        test_source = test_name + " = 1;\n"
        test_source_modified = test_name + " = 42;\n"

        client = TsDevClient(cli_path=cli_path, use_serve=True)
        assert client._serve_active(), (
            "serve should be active (not elevated + use_serve=True)"
        )

        broker_handle: int | None = None
        prog_id: int = 0

        try:
            # Pre-cleanup: delete any leftover from a previous failed run
            try:
                client.delete("function", test_name)
            except Exception:
                pass

            # 1. Insert
            r = client.insert("function", test_name, test_source)
            assert r.success, f"insert failed: {r}"
            assert r.prog_id is not None and r.prog_id > 0, (
                f"insert returned prog_id={r.prog_id}"
            )
            assert r.ell_path, (
                f"insert ell_path must be non-empty, got {r.ell_path!r}"
            )
            prog_id = r.prog_id

            # Broker is lazy-started on first op — capture handle now
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, "broker should be started after first op"

            # 2. Modify
            m = client.modify("function", test_name, test_source_modified)
            assert m.success, f"modify failed: {m}"
            assert m.prog_id == prog_id, (
                f"modify should preserve prog_id: {m.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart between ops"
            )

            # 3. Delete
            d = client.delete("function", test_name)
            assert d.success, f"delete failed: {d}"
            assert client.broker_process_handle() == broker_handle

        finally:
            # Emergency sanitation: best-effort delete and broker shutdown
            try:
                client.delete("function", test_name)
            except Exception:
                pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_indicator_lifecycle_l18(self):
        """TVS-L.18 GREEN (test-only): serve single-op lifecycle for indicator
        (insert -> modify -> delete) through one elevated broker.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "TS_AUTO_PY_SERVE_LIFE_L18_003"

        test_source = 'Plot1(Close, "Close");\n'
        test_source_modified = 'Plot1(Close * 2, "Close2");\n'

        client = TsDevClient(cli_path=cli_path, use_serve=True)
        assert client._serve_active(), (
            "serve should be active (not elevated + use_serve=True)"
        )

        broker_handle: int | None = None
        prog_id: int = 0

        try:
            # Pre-cleanup: delete any leftover from a previous failed run
            try:
                client.delete("indicator", test_name)
            except Exception:
                pass

            # 1. Insert
            r = client.insert("indicator", test_name, test_source)
            assert r.success, f"insert failed: {r}"
            assert r.prog_id is not None and r.prog_id > 0, (
                f"insert returned prog_id={r.prog_id}"
            )
            assert r.ell_path, (
                f"insert ell_path must be non-empty, got {r.ell_path!r}"
            )
            prog_id = r.prog_id

            # Broker is lazy-started on first op — capture handle now
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, "broker should be started after first op"

            # 2. Modify
            m = client.modify("indicator", test_name, test_source_modified)
            assert m.success, f"modify failed: {m}"
            assert m.prog_id == prog_id, (
                f"modify should preserve prog_id: {m.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart between ops"
            )

            # 3. Delete
            d = client.delete("indicator", test_name)
            assert d.success, f"delete failed: {d}"
            assert client.broker_process_handle() == broker_handle

        finally:
            # Emergency sanitation: best-effort delete and broker shutdown
            try:
                client.delete("indicator", test_name)
            except Exception:
                pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_tradingapp_lifecycle_l18(self):
        """TVS-L.18 GREEN (test-only): serve single-op lifecycle for tradingapp
        (insert -> modify -> delete) through one elevated broker.
        """
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        test_name = "TS_AUTO_PY_SERVE_LIFE_L18_004"

        test_source = "using elsystem;\n"
        test_source_modified = "using elsystem;\nusing elsystem.windows.forms;\n"

        client = TsDevClient(cli_path=cli_path, use_serve=True)
        assert client._serve_active(), (
            "serve should be active (not elevated + use_serve=True)"
        )

        broker_handle: int | None = None
        prog_id: int = 0

        try:
            # Pre-cleanup: delete any leftover from a previous failed run
            try:
                client.delete("tradingapp", test_name)
            except Exception:
                pass

            # 1. Insert
            r = client.insert("tradingapp", test_name, test_source)
            assert r.success, f"insert failed: {r}"
            assert r.prog_id is not None and r.prog_id > 0, (
                f"insert returned prog_id={r.prog_id}"
            )
            assert r.ell_path, (
                f"insert ell_path must be non-empty, got {r.ell_path!r}"
            )
            prog_id = r.prog_id

            # Broker is lazy-started on first op — capture handle now
            broker_handle = client.broker_process_handle()
            assert broker_handle is not None, "broker should be started after first op"

            # 2. Modify
            m = client.modify("tradingapp", test_name, test_source_modified)
            assert m.success, f"modify failed: {m}"
            assert m.prog_id == prog_id, (
                f"modify should preserve prog_id: {m.prog_id} vs {prog_id}"
            )
            assert client.broker_process_handle() == broker_handle, (
                "broker should not restart between ops"
            )

            # 3. Delete
            d = client.delete("tradingapp", test_name)
            assert d.success, f"delete failed: {d}"
            assert client.broker_process_handle() == broker_handle

        finally:
            # Emergency sanitation: best-effort delete and broker shutdown
            try:
                client.delete("tradingapp", test_name)
            except Exception:
                pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    # -- TVS-L.15: thin opt-in serve long-lived stress ---------------------

    def test_serve_long_lived_stress_l15(self):
        """TVS-L.15 GREEN (test-only): thin opt-in serve long-lived stress.

        Drives tens-to-low-hundreds of operations through ONE elevated serve
        broker (one ``TsDevLib`` instance, ``flushPerItem=false`` for the whole
        session), asserts the broker process handle never changes (no restart),
        injects ONE compile failure mid-run then a recovery op on the same
        broker, and asserts no leftover disposable names at the end.

        Opt-in: skipped unless ``TS_SERVE_STRESS=1`` is set, so it does not
        bloat the default serve E2E suite. The op count defaults to 40
        (=> ~160 ops across insert/exists/modify/delete) and is overridable
        via ``TS_SERVE_STRESS_N`` (keep it in the tens-to-low-hundreds range).

        Crash-reporter attribution (TVS-L.15):
          Pre-existing ``WerFault.exe`` / ``TSCrashReport.exe`` PIDs are
          snapshotted BEFORE the run. After the run, only crash reporters with
          PIDs NOT in the pre-existing snapshot are attributed to this test.
          Pre-existing crash reporters are NEVER killed — we only assert that
          this test did not SPAWN new ones. This avoids the false positive of
          blaming an unrelated pre-existing crash reporter on the serve path,
          and avoids killing arbitrary user/system crash reporters.

        No production code changes: this is a test-only consumer of the
        existing serve broker. The broker handle stability is observed via
        ``client.broker_process_handle()`` (an existing test seam).
        """
        # Opt-in gate.
        if os.environ.get("TS_SERVE_STRESS") != "1":
            pytest.skip("opt-in: set TS_SERVE_STRESS=1 to run the serve long-lived stress")
        # Skip if elevated — serve mode only activates when NOT elevated.
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()

        # Op count (tens-to-low-hundreds). Default 40 => ~160 ops total.
        try:
            n_ops = int(os.environ.get("TS_SERVE_STRESS_N", "40"))
        except ValueError:
            n_ops = 40
        if n_ops < 4:
            n_ops = 4
        # Hard ceiling to keep it "low hundreds" per the TVS-L.15 brief.
        if n_ops > 300:
            n_ops = 300

        # Unique disposable names for this test (TVS-L.15).
        names = [f"TS_AUTO_PY_SERVE_STRESS_L15_{i:03d}" for i in range(n_ops)]

        def _ins_src(nm: str) -> str:
            return (
                f"{{ L15 serve stress {nm} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        def _mod_src(nm: str) -> str:
            return (
                f"{{ L15 serve stress modify {nm} }}\n"
                "Inputs: FastLen(5), SlowLen(15);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
                "if FastMA < SlowMA then SellShort next bar at Market;\n"
            )

        # Invalid source guaranteed to fail compilation (used for the mid-run
        # compile-failure injection on a disposable fault name).
        fault_name = "TS_AUTO_PY_SERVE_STRESS_L15_FAULT"
        invalid_source = "{ L15 serve stress fault }\nInputs: ;\n{ syntax error }\n"

        client = TsDevClient(cli_path=cli_path)
        assert client._serve_active(), (
            "serve must be active (non-elevated session + use_serve=True)"
        )

        # --- Snapshot pre-existing crash-reporter PIDs (TVS-L.15) ----------
        # Only NEW crash reporters (PIDs not in this set) are attributed to
        # this test. Pre-existing ones are never killed.
        pre_crash_pids = _snapshot_crash_reporter_pids()

        broker_handle: int | None = None
        prog_ids: list[int] = []

        try:
            # Pre-cleanup: best-effort delete of any leftover from a prior
            # failed run, through the broker (the production path).
            for nm in names + [fault_name]:
                try:
                    client.delete("strategy", nm)
                except Exception:
                    pass

            # --- 1. INSERT n_ops strategies (one broker) ------------------
            for i, nm in enumerate(names):
                r = client.insert("strategy", nm, _ins_src(nm))
                assert r.success, f"insert {nm} failed: {r}"
                assert r.prog_id is not None and r.prog_id > 0, (
                    f"insert prog_id must be >0 for {nm}, got {r.prog_id}"
                )
                prog_ids.append(r.prog_id)
                # Capture the broker handle on the first op and assert it
                # never changes for the rest of the run.
                if broker_handle is None:
                    broker_handle = client.broker_process_handle()
                    assert broker_handle is not None, (
                        "broker handle must be non-None after first op "
                        "(serve was bypassed)"
                    )
                else:
                    assert client.broker_process_handle() == broker_handle, (
                        f"broker handle changed at insert {i} -- broker restarted"
                    )

                # --- Mid-run compile-failure + recovery (ONE injection) ---
                # Inject at the midpoint of the insert phase, on the SAME
                # broker. The fault name is disposable and cleaned up by the
                # create-path cleanup (doInsert routes compile failures
                # through cleanupCreatePathFailure). The recovery is the next
                # normal insert (i+1), proving the broker survived.
                if i == n_ops // 2:
                    with pytest.raises(CompileError):
                        client.insert("strategy", fault_name, invalid_source)
                    # Broker must still be alive with the SAME handle.
                    assert client.broker_process_handle() == broker_handle, (
                        "broker handle changed after compile failure -- "
                        "broker restarted on compile error"
                    )
                    # The fault item must NOT exist (create-path cleanup).
                    ex = client.exists("strategy", fault_name)
                    assert not ex.exists, (
                        f"{fault_name} should not exist after compile-failure "
                        "create-path cleanup"
                    )
                    assert client.broker_process_handle() == broker_handle

            # --- 2. EXISTS n_ops (through the broker) ---------------------
            for i, nm in enumerate(names):
                ex = client.exists("strategy", nm)
                assert ex.exists, f"{nm} should exist after insert"
                assert ex.prog_id == prog_ids[i], (
                    f"exists prog_id mismatch for {nm}: {ex.prog_id} vs {prog_ids[i]}"
                )
                assert client.broker_process_handle() == broker_handle, (
                    f"broker handle changed at exists {i}"
                )

            # --- 3. MODIFY n_ops (through the broker) ---------------------
            for i, nm in enumerate(names):
                r = client.modify("strategy", nm, _mod_src(nm))
                assert r.success, f"modify {nm} failed: {r}"
                assert r.prog_id == prog_ids[i], (
                    f"modify prog_id must be preserved for {nm}: "
                    f"{r.prog_id} vs {prog_ids[i]}"
                )
                assert client.broker_process_handle() == broker_handle, (
                    f"broker handle changed at modify {i}"
                )

            # --- 4. DELETE n_ops (production-verified cleanup) ------------
            for i, nm in enumerate(names):
                r = client.delete("strategy", nm)
                assert r.success, f"delete {nm} failed: {r}"
                assert client.broker_process_handle() == broker_handle, (
                    f"broker handle changed at delete {i}"
                )

            # --- 5. Assert no leftover disposable names -------------------
            for nm in names + [fault_name]:
                ex = client.exists("strategy", nm)
                assert not ex.exists, (
                    f"leftover disposable name {nm} still exists after cleanup"
                )
                assert client.broker_process_handle() == broker_handle

            # Final same-handle assertion.
            assert client.broker_process_handle() == broker_handle, (
                "broker handle changed before final crash-reporter check"
            )

            # --- 6. Crash-reporter attribution (TVS-L.15) -----------------
            # Only NEW crash reporters (PIDs not in the pre-existing snapshot)
            # are attributed to this test. Pre-existing ones are NOT killed.
            post_crash = _snapshot_crash_reporter_pids()
            new_crash = {
                pid: name for pid, name in post_crash.items()
                if pid not in pre_crash_pids
            }
            assert not new_crash, (
                f"this test spawned new crash reporter(s): {new_crash} "
                f"(pre-existing PIDs were {sorted(pre_crash_pids)})"
            )

        finally:
            # Emergency sanitation ONLY (safety net for mid-test assertion
            # failures): best-effort delete every disposable name + shutdown
            # the broker so no elevated broker process is leaked.
            for nm in names + [fault_name]:
                try:
                    client.delete("strategy", nm)
                except Exception:
                    pass
            if client._broker is not None:
                try:
                    client._broker.shutdown()
                except Exception:
                    pass

    def test_serve_vas_guard_abort(self, monkeypatch):
        """TVS-1c GREEN (test-only): WHServer VAS-headroom guard on the
        serve path, end to end through the real elevated broker.

        Two-threshold policy (``vasGuardAction``, ``whserver_health.h``):
        the 256/64 MB pair is the RECYCLE TRIGGER, the 64/32 MB pair
        (``--whserver-abort-free-mb`` / ``--whserver-abort-largest-free-mb``)
        is the hard ABORT FLOOR. A trigger breach still above the floor
        with recycling unauthorized CONTINUES -- the request dispatches;
        only a FLOOR breach aborts.

        Phase 1 (default thresholds): a 2-item ``batch_upsert`` succeeds --
        the guard passes with normal WHServer headroom.
        Phase 2 (floor breach): ``TSDEV_WHSERVER_FREE_MB=6000`` +
        ``TSDEV_WHSERVER_ABORT_FREE_MB=5000`` (both forwarded to the broker
        as explicit ``--whserver-*`` flags because ``runas`` does not
        inherit env; the trigger is raised together with the floor because
        ``vasThresholdsInverted`` rejects a trigger below the floor with
        exit 1 before the pipe file is written). Every mutating request is
        refused with ``exit_code=14`` / ``error.code ==
        "whserver_vas_abort"`` without executing the op; the client aborts
        the batch -- refusal recorded, remaining items become exit-12
        markers carrying the abort message, no further requests are sent.
        Read-only ``exists`` is NOT gated and still answers; nothing is
        written.
        Phase 3 (trigger-only breach): ``TSDEV_WHSERVER_FREE_MB=5000`` with
        the floor at its default -- above the floor, recycling unauthorized
        -> CONTINUE: both requests dispatch and succeed (exit 0) and the
        items are really written (``exists`` answers true), so this phase
        carries its own delete cleanup. The broker's one-shot
        ``whserver_vas_continue`` notice rides the BROKER's stderr, which
        the hidden ``runas`` spawn discards (the response frame carries no
        diagnostics lane), so it is not client-observable here; its JSON
        shape (event name + ``action:"continue"``) is pinned by
        ``tests/test_recycle_resume.cpp`` check15 and the direct path's
        captured stderr carries it (live-probed 2026-10-28).
        """
        # Skip if elevated — serve mode only activates when NOT elevated.
        if _detect_elevation():
            pytest.skip("serve E2E must run non-elevated; use run_serve_e2e.ps1")

        cli_path = _find_cli()
        names = ["TS_AUTO_PY_SERVE_VAS_01", "TS_AUTO_PY_SERVE_VAS_02"]

        def _src(n: str) -> str:
            return (
                f"{{ TVS-1c serve VAS guard {n} }}\n"
                "Inputs: FastLen(10), SlowLen(20);\n"
                "Variables: FastMA(0), SlowMA(0);\n"
                "FastMA = AverageFC(Close, FastLen);\n"
                "SlowMA = AverageFC(Close, SlowLen);\n"
                "if FastMA > SlowMA then Buy next bar at Market;\n"
            )

        items = [BatchItem(type="strategy", name=n, source=_src(n))
                 for n in names]

        # --- Phase 1: default thresholds -> guard passes, batch succeeds ---
        client1 = TsDevClient(cli_path=cli_path)
        try:
            for nm in names:
                try:
                    client1.delete("strategy", nm)
                except Exception:
                    pass
            results = client1.batch_upsert(items)
            assert len(results) == 2
            assert all(r.success for r in results), (
                f"default-threshold batch_upsert must succeed: {results!r}"
            )
            exists_r = client1.exists("strategy", names[0])
            assert exists_r.exists is True
        finally:
            # Production cleanup through the same (default-guard) broker:
            # deletes are mutating ops but pass the normal thresholds.
            for nm in names:
                _cleanup(client1, nm)

        # --- Phase 2: floor breach -> broker refuses mutating requests ---
        # ShellExecuteExW runas does NOT inherit the caller's environment
        # block, so _build_serve_parameters forwards TSDEV_WHSERVER_* to the
        # broker as explicit --whserver-* flags. The FLOOR pair is the
        # hard-abort threshold: TSDEV_WHSERVER_ABORT_FREE_MB=5000 is above
        # any possible non-LAA 32-bit free VAS (~2 GB ceiling), so the guard
        # ALWAYS breaches while WHServer.exe is running. The trigger is
        # raised with it (6000 >= 5000): vasThresholdsInverted rejects a
        # trigger below the floor outright (exit 1 before the pipe file is
        # written), so the floor cannot be breached alone.
        monkeypatch.setenv("TSDEV_WHSERVER_FREE_MB", "6000")
        monkeypatch.setenv("TSDEV_WHSERVER_ABORT_FREE_MB", "5000")
        client2 = TsDevClient(cli_path=cli_path)
        try:
            assert client2._serve_active()
            # Warm-up: a read-only exists is NOT gated, but it is a
            # warehouse dispatch, so WHServer.exe is guaranteed running
            # (and measured) when the mutating request's guard runs --
            # without it a self-reaped server would make the guard pass
            # trivially and dispatch the upserts.
            warm = client2.exists("strategy", names[0])
            assert warm.exit_code == 0, f"warm-up exists failed: {warm!r}"
            results = client2.batch_upsert(items)
            assert len(results) == 2
            # Item 1: the broker refused before dispatch.
            assert results[0].success is False
            assert results[0].exit_code == 14
            assert results[0].error is not None
            assert results[0].error.code == "whserver_vas_abort"
            assert "WHServer VAS headroom" in results[0].error.message
            # Item 2: never attempted — exit-12 marker with the abort
            # message (verbatim kVasAbortMarkerMessage).
            assert results[1].success is False
            assert results[1].exit_code == 12
            assert results[1].error is not None
            assert results[1].error.message == (
                "Batch aborted (WHServer VAS headroom) "
                "before this item was attempted"
            )
            # Nothing was written by phase 2 (items were deleted in phase 1
            # cleanup and the refused upserts never dispatched).
            r = client2.exists("strategy", names[0])
            assert r.exit_code == 0 and r.exists is False
        finally:
            if client2._broker is not None:
                try:
                    client2._broker.shutdown()
                except Exception:
                    pass

        # --- Phase 3: trigger-only breach -> CONTINUE, request dispatches ---
        # New two-threshold policy: TSDEV_WHSERVER_FREE_MB=5000 breaches the
        # RECYCLE TRIGGER (any non-LAA 32-bit free VAS is below it) while the
        # abort floor stays at its 64/32 default -- a survivable headroom.
        # Recycling is unauthorized (no --recycle-whserver), so vasGuardAction
        # returns Continue: the requests DISPATCH instead of being refused.
        # The phase-2 abort env vars are dropped first (monkeypatch keeps
        # them otherwise).
        monkeypatch.delenv("TSDEV_WHSERVER_ABORT_FREE_MB", raising=False)
        monkeypatch.delenv("TSDEV_WHSERVER_ABORT_LARGEST_FREE_MB", raising=False)
        monkeypatch.setenv("TSDEV_WHSERVER_FREE_MB", "5000")
        client3 = TsDevClient(cli_path=cli_path)
        leftovers: list[str] = []
        try:
            assert client3._serve_active()
            # Same warm-up as phase 2: guarantee WHServer.exe is running so
            # the guard really takes the Continue arm (with a self-reaped
            # server it would pass trivially -- same observable outcome,
            # but the policy under test would go unexercised).
            warm = client3.exists("strategy", names[0])
            assert warm.exit_code == 0, f"warm-up exists failed: {warm!r}"
            results = client3.batch_upsert(items)
            assert len(results) == 2
            assert all(r.success for r in results), (
                f"trigger-only breach must CONTINUE (dispatch), got: {results!r}"
            )
            assert all(r.exit_code == 0 for r in results), (
                f"continued upserts must exit 0, got: {results!r}"
            )
            # The continued upserts EXECUTED -- the old phase-2 "nothing
            # written" assumption does not hold on this arm.
            for nm in names:
                r = client3.exists("strategy", nm)
                assert r.exit_code == 0 and r.exists is True, (
                    f"continued upsert must have written {nm}: {r!r}"
                )
        finally:
            # The continued upserts really wrote the items: delete them
            # while this (trigger-breach) broker is still up -- the guard
            # CONTINUEs mutating deletes too, so the cleanup dispatches.
            for nm in names:
                try:
                    client3.delete("strategy", nm)
                except Exception:
                    pass
                try:
                    r = client3.exists("strategy", nm)
                    if r.exit_code != 0 or r.exists:
                        leftovers.append(nm)
                except Exception:
                    leftovers.append(f"{nm} (exists check failed)")
            if client3._broker is not None:
                try:
                    client3._broker.shutdown()
                except Exception:
                    pass
        # Leak guard (success path only -- never masks a body failure).
        assert not leftovers, f"phase 3 cleanup left items behind: {leftovers}"


def _snapshot_crash_reporter_pids() -> dict[int, str]:
    """Return a mapping of {pid: process_name} for WerFault.exe and
    TSCrashReport.exe processes currently running.

    Uses ``tasklist`` (no psutil dependency). Returns an empty dict if
    tasklist is unavailable. This is used to snapshot PRE-EXISTING crash
    reporters so a test can attribute only NEW ones to itself and never
    kill arbitrary pre-existing crash reporters (TVS-L.15).
    """
    result: dict[int, str] = {}
    for image in ("WerFault.exe", "TSCrashReport.exe"):
        try:
            out = subprocess.run(
                ["tasklist", "/FI", f"IMAGENAME eq {image}",
                 "/FO", "CSV", "/NH"],
                capture_output=True, text=True, timeout=10,
                check=False,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
        if out.returncode != 0:
            continue
        for line in out.stdout.splitlines():
            line = line.strip()
            if not line or line.lower().startswith("info:"):
                continue
            # CSV format: "Image Name","PID","Session Name","Session#","Mem Usage"
            parts = [c.strip().strip('"') for c in line.split('","')]
            if len(parts) < 2:
                continue
            try:
                pid = int(parts[1])
            except ValueError:
                continue
            result[pid] = parts[0]
    return result
