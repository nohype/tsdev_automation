"""E2E test for serve mode (TVS-K.6a).

This test MUST run from a NON-elevated Python process. When run elevated,
it skips automatically (serve mode is not used when already elevated).

Run via: tests/run_serve_e2e.ps1 (launches this test unelevated via Task
Scheduler; the serve broker's normal elevation request is handled by Windows
UAC consent).
"""
from __future__ import annotations

import os
import subprocess
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
                "(serve was bypassed — batch_insert did not route through the broker)"
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
                "(broker restarted — TsDevLib lifetime broken)"
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
                        f"broker handle changed at insert {i} — broker restarted"
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
                        "broker handle changed after compile failure — "
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
