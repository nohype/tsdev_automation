# SPDX-License-Identifier: MIT
"""Minimal argparse CLI for :class:`tsdev_automation.TsDevClient`.

This is a convenience entry point (``tsdev-automation`` console script and
``python -m tsdev_automation``).  The primary interface is the Python API.

Usage::

    tsdev-automation insert  strategy MyName ./source.el
    tsdev-automation modify  strategy MyName ./source.el
    tsdev-automation delete  strategy MyName
    tsdev-automation exists  strategy MyName
    tsdev-automation get-source  strategy MyName [--output ./out.el]
    tsdev-automation upsert  strategy MyName ./source.el
    tsdev-automation batch-insert  ./manifest.txt
    tsdev-automation batch-modify  ./manifest.txt
    tsdev-automation batch-delete  ./manifest.txt
    tsdev-automation batch-upsert  ./manifest.txt
    tsdev-automation batch-insert-dir  strategy ./mydir [-t el]...
    tsdev-automation batch-modify-dir  strategy ./mydir [-t el]...
    tsdev-automation batch-delete-dir  strategy ./mydir [-t el]...
    tsdev-automation batch-upsert-dir  strategy ./mydir [-t el]...
    tsdev-automation --backup-cache [--cache-backup-policy <mode>]
    tsdev-automation --restore-cache
    tsdev-automation --backup-cache insert strategy MyName ./source.el
        (the cache flags are global: with an operation they run the CLI's
        cache phase first, fixed order backup -> restore -> op)

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
    p.add_argument(
        "-v", "--verbose",
        action="count",
        default=0,
        help="Increase tsdev_cli log verbosity (repeatable): default warnings+errors; -v info; -vv debug.",
    )
    p.add_argument(
        "--backup-cache",
        action="store_true",
        default=False,
        help="back up <TS>\\Program\\Cache to %%LOCALAPPDATA%%\\tsdev_cli\\cache-backup "
        "before the operation (standalone when no operation is given).",
    )
    p.add_argument(
        "--restore-cache",
        action="store_true",
        default=False,
        help="restore the cache backup into the Cache dir before the operation "
        "(standalone when no operation is given).",
    )
    p.add_argument(
        "--cache-backup-policy",
        choices=("prompt", "overwrite", "skip", "abort"),
        default=None,
        metavar="{prompt,overwrite,skip,abort}",
        help="what --backup-cache does when a backup already exists "
        "(Python-side default abort; 'prompt' degrades to keep-existing "
        "on a pipe and raises ValueError on the serve path). "
        "Requires --backup-cache.",
    )
    sub = p.add_subparsers(dest="command", required=False)

    for op in ("insert", "modify"):
        sp = sub.add_parser(op, help=f"{op} a component.")
        sp.add_argument("type", help="strategy | function | indicator | tradingapp | showme")
        sp.add_argument("name", help="component name")
        sp.add_argument("source", help="source file path OR inline EasyLanguage source text")

    sp = sub.add_parser("delete", help="delete a component.")
    sp.add_argument("type", help="strategy | function | indicator | tradingapp | showme")
    sp.add_argument("name", help="component name")

    sp = sub.add_parser("exists", help="query whether a component exists.")
    sp.add_argument("type", help="strategy | function | indicator | tradingapp | showme")
    sp.add_argument("name", help="component name")
    sp.add_argument(
        "--case-insensitive",
        action="store_true",
        default=False,
        help="match the name case-insensitively (default: exact match)",
    )

    sp = sub.add_parser("get-source", help="retrieve a component's EasyLanguage source.")
    sp.add_argument("type", help="strategy | function | indicator | tradingapp | showme")
    sp.add_argument("name", help="component name")
    sp.add_argument("--output", default=None, metavar="<file>", help="write source to <file> instead of stdout")

    sp = sub.add_parser("upsert", help="insert a component, or modify it if it already exists.")
    sp.add_argument("type", help="strategy | function | indicator | tradingapp | showme")
    sp.add_argument("name", help="component name")
    sp.add_argument("source", help="source file path OR inline EasyLanguage source text")

    for op in ("batch-insert", "batch-modify", "batch-delete", "batch-upsert"):
        sp = sub.add_parser(op, help=f"{op} from a manifest file.")
        sp.add_argument("manifest", help="path to manifest file (one item per line)")

    for op in ("batch-insert-dir", "batch-modify-dir", "batch-delete-dir", "batch-upsert-dir"):
        sp = sub.add_parser(op, help=f"{op} from a directory of .el files.")
        sp.add_argument("type", help="strategy | function | indicator | tradingapp | showme")
        sp.add_argument("dir_path", help="path to directory of .el files")
        sp.add_argument("-t", action="append", dest="extensions", default=None,
                        help="file extension filter (repeatable; first -t overrides defaults .el+.txt)")

    return p


def _main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    # Mirror the C++ CLI rule: --cache-backup-policy requires --backup-cache.
    if args.cache_backup_policy is not None and not args.backup_cache:
        parser.error("--cache-backup-policy requires --backup-cache")
    if args.command is None and not (args.backup_cache or args.restore_cache):
        parser.error("the following arguments are required: command")
    client = TsDevClient(cli_path=args.cli_path, ts_path=args.ts_path, timeout=args.timeout, verbose=args.verbose)

    # Cache-phase kwargs forwarded to op methods; empty dict keeps the
    # signatures of simple fakes used by tests unchanged.
    cache_kwargs: dict = {}
    if args.backup_cache:
        cache_kwargs["backup_cache"] = True
    if args.restore_cache:
        cache_kwargs["restore_cache"] = True
    if args.cache_backup_policy is not None:
        cache_kwargs["cache_backup_policy"] = args.cache_backup_policy

    try:
        if args.command is None:
            # Standalone cache maintenance: fixed order backup -> restore.
            # Direct mode runs ONE tsdev_cli process carrying both flags;
            # serve mode sends the two broker requests sequentially. Phase
            # failures raise (handled below), so every returned result is a
            # success.
            results = client.cache_maintenance(
                backup=args.backup_cache,
                restore=args.restore_cache,
                on_existing=args.cache_backup_policy or "abort",
            )
            for r in results:
                print(f"[OK] {r.op} cache={r.cache}")
            return 0
        if args.command in ("insert", "modify"):
            r = getattr(client, args.command)(args.type, args.name, args.source, **cache_kwargs)
            _print_result(r)
            return 0 if r.success else 1
        if args.command == "delete":
            r = client.delete(args.type, args.name, **cache_kwargs)
            _print_result(r)
            return 0 if r.success else 1
        if args.command == "exists":
            r = client.exists(args.type, args.name, case_insensitive=args.case_insensitive, **cache_kwargs)
            if r.exists:
                print(f"[OK] exists {r.type} {r.name!r} -> ProgId={r.prog_id} stored_name={r.stored_name!r}")
            else:
                print(f"[OK] exists {r.type} {r.name!r} -> not found")
            return 0
        if args.command == "get-source":
            r = client.get_source(args.type, args.name, output_file=args.output, **cache_kwargs)
            if args.output is None:
                print(r.source, end="")
            else:
                print(f"Source written to {args.output}")
            return 0
        if args.command == "upsert":
            r = client.upsert(args.type, args.name, args.source, **cache_kwargs)
            _print_result(r)
            return 0 if r.success else 1
        if args.command.endswith("-dir"):
            # batch-insert-dir, batch-modify-dir, batch-delete-dir, batch-upsert-dir
            op = args.command[len("batch-"):-len("-dir")]  # "insert", "modify", "delete", "upsert"
            results = getattr(client, f"batch_{op}_dir")(args.type, args.dir_path, args.extensions, **cache_kwargs)
            for r in results:
                _print_result(r)
            return 0 if all(r.success for r in results) else 1
        elif args.command.startswith("batch-"):
            op = args.command.split("-", 1)[1]
            items = _read_manifest(Path(args.manifest), needs_source=(op != "delete"))
            results = getattr(client, f"batch_{op}")(items, **cache_kwargs)
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
