"""E2E test for serve mode (TVS-K.6a).

This test MUST run from a NON-elevated Python process. When run elevated,
it skips automatically (serve mode is not used when already elevated).

Run via: tests/run_serve_e2e.ps1 (launches this test unelevated via Task
Scheduler with ConsentPromptBehaviorAdmin=0 for auto-elevation without
UAC prompts).
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest

from tsdev_automation.client import TsDevClient, _detect_elevation
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
        """Verify the broker survives a compile failure and auto-cleanup.

        Inserts invalid EL source (compile error triggers auto-cleanup of the
        partial warehouse entry via deleteEntry with flush=false), then
        verifies the broker is still alive and can service a subsequent op.
        This tests the critical K.6a fix: the auto-cleanup path in
        verifyModule() must NOT call closeWarehouse() in serve mode.
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

            # 2. Verify the failed item does NOT exist (auto-cleanup worked)
            result = client.exists("strategy", test_name)
            assert not result.exists, (
                f"{test_name} should not exist after compile-failure auto-cleanup"
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
