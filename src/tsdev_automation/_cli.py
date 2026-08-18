# SPDX-License-Identifier: MIT
"""Minimal argparse CLI for :class:`tsdev_automation.TsDevClient`.

This is a convenience entry point (``tsdev-automation`` console script and
``python -m tsdev_automation``).  The primary interface is the Python API.

Usage::

    tsdev-automation insert  strategy MyName ./source.el
    tsdev-automation modify  strategy MyName ./source.el
    tsdev-automation delete  strategy MyName
    tsdev-automation batch-insert  ./manifest.txt
    tsdev-automation batch-modify  ./manifest.txt
    tsdev-automation batch-delete  ./manifest.txt

For insert/modify the source argument is interpreted as a file path if it
exists, otherwise as inline EasyLanguage source text (matching the Python API).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .client import BatchItem, TsDevClient
from .exceptions import TsDevError
from .results import TsDevResult


def _print_result(r: TsDevResult) -> None:
    if r.success:
        print(f"[OK] {r.op} {r.type} {r.name!r} -> ProgId={r.prog_id} ell={r.ell_path}")
    else:
        err = r.error
        print(f"[FAIL] {r.op} {r.type} {r.name!r} exit={r.exit_code} code={err.code if err else '?'}")
        if err and err.message:
            print(f"       {err.message}")
        if err and err.compile_errors:
            print(f"       compile_errors:\n{err.compile_errors}")


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="tsdev-automation",
        description="Drive tsdev_cli (insert/modify/delete TradeStation components).",
    )
    p.add_argument("--cli-path", help="Path to tsdev_cli.exe (default: auto-discover).")
    p.add_argument("--ts-path", help="TradeStation install dir (default: auto-discover).")
    p.add_argument("--timeout", type=float, default=None, help="Subprocess timeout in seconds.")
    sub = p.add_subparsers(dest="command", required=True)

    for op in ("insert", "modify"):
        sp = sub.add_parser(op, help=f"{op} a component.")
        sp.add_argument("type", help="strategy | function | indicator | tradingapp")
        sp.add_argument("name", help="component name")
        sp.add_argument("source", help="source file path OR inline EasyLanguage source text")

    sp = sub.add_parser("delete", help="delete a component.")
    sp.add_argument("type", help="strategy | function | indicator | tradingapp")
    sp.add_argument("name", help="component name")

    for op in ("batch-insert", "batch-modify", "batch-delete"):
        sp = sub.add_parser(op, help=f"{op} from a manifest file.")
        sp.add_argument("manifest", help="path to manifest file (one item per line)")

    return p


def _main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    client = TsDevClient(cli_path=args.cli_path, ts_path=args.ts_path, timeout=args.timeout)

    try:
        if args.command in ("insert", "modify"):
            r = getattr(client, args.command)(args.type, args.name, args.source)
            _print_result(r)
            return 0 if r.success else 1
        if args.command == "delete":
            r = client.delete(args.type, args.name)
            _print_result(r)
            return 0 if r.success else 1
        if args.command.startswith("batch-"):
            op = args.command.split("-", 1)[1]
            items = _read_manifest(Path(args.manifest), needs_source=(op != "delete"))
            results = getattr(client, f"batch_{op}")(items)
            for r in results:
                _print_result(r)
            return 0 if all(r.success for r in results) else 1
    except TsDevError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    except (ValueError, FileNotFoundError, TimeoutError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 3
    return 0


def _read_manifest(path: Path, needs_source: bool) -> list[BatchItem]:
    items: list[BatchItem] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if needs_source and len(parts) == 3:
            items.append(BatchItem(parts[0], parts[1], parts[2]))
        elif not needs_source and len(parts) >= 2:
            items.append(BatchItem(parts[0], parts[1]))
        else:
            raise ValueError(f"malformed manifest line: {raw!r}")
    return items


if __name__ == "__main__":
    raise SystemExit(_main())
