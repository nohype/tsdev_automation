# SPDX-License-Identifier: MIT
"""Result dataclasses mirroring the ``tsdev_cli`` JSON schema.

The C++ CLI (``src/tsdev_cli.cpp``) emits, for every operation, a JSON object
with this schema (nlohmann/json, UTF-8):

    success: {"op","type","name","success":true,"exit_code":0,
              "prog_id":N,"ell_path":"..."}
    failure: {"op","type","name","success":false,"exit_code":N,
              "error":{"code":"...","message":"...",
                       "prog_id"?,          # exit 8 (compile error)
                       "compile_errors"?,   # exit 8 (compile error)
                       "existing_prog_id"?}} # exit 5 (insert, already exists)

These dataclasses are the Python-side representation.  ``op``/``type``/``name``
may be absent from a usage-error object, hence ``None`` defaults.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class TsDevErrorInfo:
    """Structured error payload from a failed CLI operation."""

    code: str
    """Machine-readable error code (e.g. ``"not_found"``, ``"compile_error"``)."""

    message: str
    """Human-readable error message from the CLI."""

    prog_id: int | None = None
    """ProgId allocated before a compile failure (exit 8)."""

    compile_errors: str | None = None
    """Raw compiler error text (exit 8)."""

    existing_prog_id: int | None = None
    """ProgId of the pre-existing component (exit 5, insert)."""


@dataclass
class TsDevResult:
    """Result of a single CLI operation (insert / modify / delete)."""

    op: str
    type: str | None
    name: str | None
    success: bool
    exit_code: int
    prog_id: int | None = None
    ell_path: str | None = None
    error: TsDevErrorInfo | None = None
    # Raw subprocess output retained for debugging.
    stdout: str = ""
    stderr: str = ""

    @classmethod
    def from_json(cls, obj: dict, stdout: str = "", stderr: str = "") -> "TsDevResult":
        """Build a :class:`TsDevResult` from a parsed CLI JSON object."""
        err_obj = obj.get("error")
        error: TsDevErrorInfo | None = None
        if isinstance(err_obj, dict):
            error = TsDevErrorInfo(
                code=str(err_obj.get("code", "")),
                message=str(err_obj.get("message", "")),
                prog_id=err_obj.get("prog_id"),
                compile_errors=err_obj.get("compile_errors"),
                existing_prog_id=err_obj.get("existing_prog_id"),
            )
        return cls(
            op=str(obj.get("op", "")),
            type=obj.get("type"),
            name=obj.get("name"),
            success=bool(obj.get("success", False)),
            exit_code=int(obj.get("exit_code", 0)),
            prog_id=obj.get("prog_id"),
            ell_path=obj.get("ell_path"),
            error=error,
            stdout=stdout,
            stderr=stderr,
        )


@dataclass
class TsDevBatchResult:
    """Aggregate view of a batch operation (convenience wrapper).

    The CLI's batch subcommands return a JSON *array* of per-item results.
    :meth:`TsDevClient.batch_*` return ``list[TsDevResult]`` directly; this
    class is provided for callers who want a summarized view.
    """

    results: list[TsDevResult] = field(default_factory=list)

    @property
    def all_success(self) -> bool:
        return all(r.success for r in self.results)

    @property
    def failures(self) -> list[TsDevResult]:
        return [r for r in self.results if not r.success]

    @property
    def successes(self) -> list[TsDevResult]:
        return [r for r in self.results if r.success]
