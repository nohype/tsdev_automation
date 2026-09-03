"""CLI entry point for TSDev UI automation.

Usage::

    uv run python -m tsdev_automation.tsdev_ui_cli <command> [options]

Commands::

    verify <type> <name>          Open item, F3 verify, report errors, close.
    open <type> <name>            Open item and leave it open.
    close                         Close current tab.
    list <type>                   List items of the given type.
    source <type> <name>          Get source of an item via clipboard.
    insert <type> <name> <file>   Insert a new item from source file.
    delete <type> <name> [...]    Delete one or more items.
    is-running                    Check if TSDev.exe is running.
    start                         Launch TSDev.exe.

Types: Strategy, Indicator, Function, ShowMe, TradingApp, PaintBar,
       ActivityBar, ProbabilityMap

Examples::

    # Verify an existing function and check for errors
    uv run python -m tsdev_automation.tsdev_ui_cli verify Function "Average"

    # List all strategies
    uv run python -m tsdev_automation.tsdev_ui_cli list Strategy

    # Insert a new indicator from a source file
    uv run python -m tsdev_automation.tsdev_ui_cli insert Indicator MyInd source.el
"""

from __future__ import annotations

import json
import sys

from .tsdev_ui import TSDevUI, TSDevNotRunningError, ComponentType

__all__ = ["main"]


def _print_json(obj: dict) -> None:
    print(json.dumps(obj, indent=2))


def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if not args:
        print(__doc__)
        return 1

    cmd = args[0]
    rest = args[1:]

    if cmd == "is-running":
        running = TSDevUI.is_running()
        _print_json({"running": running})
        return 0 if running else 1

    if cmd == "start":
        TSDevUI.start_tsdev()
        print("TSDev.exe launch requested.")
        return 0

    # All other commands require a connection to TSDev.exe
    try:
        ui = TSDevUI()
    except TSDevNotRunningError as e:
        _print_json({"error": str(e), "running": False})
        return 2

    if cmd == "verify":
        if len(rest) < 2:
            print("Usage: verify <type> <name>")
            return 1
        ctype, name = rest[0], rest[1]
        ui.open_item(ctype, name)
        ok = ui.verify()
        errors = ui.get_error_lines()
        ui.close_current_tab()
        _print_json({
            "type": ctype,
            "name": name,
            "verified": ok,
            "errors": errors,
        })
        return 0 if ok else 1

    if cmd == "open":
        if len(rest) < 2:
            print("Usage: open <type> <name>")
            return 1
        ctype, name = rest[0], rest[1]
        ui.open_item(ctype, name)
        _print_json({"type": ctype, "name": name, "opened": True})
        return 0

    if cmd == "close":
        ui.close_current_tab()
        _print_json({"closed": True})
        return 0

    if cmd == "list":
        if len(rest) < 1:
            print("Usage: list <type>")
            return 1
        ctype = rest[0]
        names = ui.list_items(ctype)
        _print_json({"type": ctype, "count": len(names), "items": names})
        return 0

    if cmd == "source":
        if len(rest) < 2:
            print("Usage: source <type> <name>")
            return 1
        ctype, name = rest[0], rest[1]
        source = ui.get_source(ctype, name)
        print(source)
        return 0

    if cmd == "insert":
        if len(rest) < 3:
            print("Usage: insert <type> <name> <file>")
            return 1
        ctype, name, file = rest[0], rest[1], rest[2]
        ok = ui.insert_item(ctype, name, file)
        _print_json({
            "type": ctype,
            "name": name,
            "inserted": ok,
            "errors": ui.get_error_lines() if not ok else [],
        })
        return 0 if ok else 1

    if cmd == "delete":
        if len(rest) < 2:
            print("Usage: delete <type> <name> [name2 ...]")
            return 1
        ctype = rest[0]
        names = rest[1:]
        ui.delete_items(ctype, names)
        _print_json({"type": ctype, "deleted": names})
        return 0

    print(f"Unknown command: {cmd}")
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
