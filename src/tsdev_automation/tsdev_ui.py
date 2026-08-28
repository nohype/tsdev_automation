"""TSDev.exe UI automation via pywinauto.

This module provides a reusable ``TSDevUI`` class for driving the
TradeStation Development Environment GUI without manual interaction.
It is used for reverse-engineering (Frida tracing of UI-triggered
actions) and for any operation that must go through the real TSDev
client path.

Dependencies (installed in the project venv via ``uv add``):
    pywinauto, pyperclip, pywin32, comtypes, six

Usage::

    from tsdev_automation.tsdev_ui import TSDevUI, ComponentType

    ui = TSDevUI()                       # connects to running TSDev.exe
    ui.open_item(ComponentType.FUNCTION, "Average")
    ui.verify()                          # F3
    errors = ui.get_error_lines()
    ui.close_current_tab()

Reference: docs/reference_code_tsdev_pywinauto/ts_dev_handler.py
"""

from __future__ import annotations

import re
import time
from pathlib import Path

import pyperclip
from pywinauto.application import Application
from pywinauto.keyboard import send_keys

__all__ = ["TSDevUI", "ComponentType", "TSDevNotRunningError"]


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class TSDevNotRunningError(RuntimeError):
    """Raised when TSDev.exe is not running or cannot be connected to."""


# ---------------------------------------------------------------------------
# Component type → TSDev dialog label mapping
# ---------------------------------------------------------------------------

class ComponentType:
    """Maps to the strings used in TSDev's Open/New dialogs."""
    STRATEGY = "Strategy"
    INDICATOR = "Indicator"
    FUNCTION = "Function"
    SHOWME = "ShowMe"
    TRADINGAPP = "TradingApp"
    PAINTBAR = "PaintBar"
    ACTIVITYBAR = "ActivityBar"
    PROBABILITYMAP = "ProbabilityMap"

    _ALL = (
        STRATEGY, INDICATOR, FUNCTION, SHOWME, TRADINGAPP,
        PAINTBAR, ACTIVITYBAR, PROBABILITYMAP,
    )


# ---------------------------------------------------------------------------
# TSDev UI handler
# ---------------------------------------------------------------------------

class TSDevUI:
    """Drive TSDev.exe via pywinauto (win32 backend).

    Prerequisites:
        - TSDev.exe must be running (start it manually or via
          ``TSDevUI.start_tsdev()``).
        - The script must run elevated (administrator) for COM/warehouse
          access to work correctly alongside TSDev.exe.
    """

    MAIN_WINDOW_TITLE_RE = r"TradeStation Development Environment( \- .*)?"
    MAIN_WINDOW_CLASS = None  # TSDev main window has no stable class name
    OPEN_DLG_CLASS = "TS_IDD_OPENDLG"
    OPEN_DLG_TITLE = "Open EasyLanguage Document"
    NEW_DLG_CLASS_PREFIX = "TS_IDD_COMMONCREATE"
    CONFIRM_DELETE_CLASS = "TS_IDD_CONFIRM_DELETE"
    CONFIRM_DELETE_TITLE = "Confirm file delete"
    DEFAULT_TIMEOUT = 10  # seconds

    def __init__(
        self,
        tsdev_path: str | None = None,
        timeout: int = DEFAULT_TIMEOUT,
    ) -> None:
        """Connect to a running TSDev.exe.

        Args:
            tsdev_path: Optional path to TSDev.exe for ``connect(path=...)``.
                        If None, connects by process name ``TSDev.exe``.
            timeout: Default wait timeout in seconds for window operations.
        """
        self._timeout = timeout
        try:
            if tsdev_path:
                self._app = Application(backend="win32").connect(path=tsdev_path)
            else:
                self._app = Application(backend="win32").connect(path="TSDev.exe")
        except Exception as exc:
            raise TSDevNotRunningError(
                "TSDev.exe is not running or could not be connected to"
            ) from exc

        self._window = self._app.window(title_re=self.MAIN_WINDOW_TITLE_RE)

    # ------------------------------------------------------------------
    # Static helpers
    # ------------------------------------------------------------------

    @staticmethod
    def start_tsdev(
        ts_path: str = r"C:\Program Files (x86)\TradeStation 9.5\Program\TSDev.exe",
    ) -> None:
        """Launch TSDev.exe (non-blocking)."""
        import subprocess
        subprocess.Popen([ts_path])

    @staticmethod
    def is_running() -> bool:
        """Check if TSDev.exe is running."""
        import subprocess
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq TSDev.exe", "/NH"],
            capture_output=True, text=True,
        )
        return "TSDev.exe" in r.stdout

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _wait_ready(self, timeout: int | None = None) -> None:
        self._window.wait("ready", timeout=timeout or self._timeout)

    def _set_focus(self) -> None:
        self._window.set_focus()
        self._window.wait("active ready", timeout=self._timeout)

    def _open_dialog(self) -> "Application.window":
        """Open the 'Open EasyLanguage Document' dialog (Ctrl+O) and return it."""
        candidates = self._app.windows(
            class_name=self.OPEN_DLG_CLASS,
            title=self.OPEN_DLG_TITLE,
        )
        if len(candidates) < 1:
            self._wait_ready()
            self._set_focus()
            self._window.type_keys("^o")

        dlg = self._app.window(
            class_name=self.OPEN_DLG_CLASS,
            title=self.OPEN_DLG_TITLE,
        )
        dlg.wait("ready", timeout=self._timeout)
        dlg.set_focus()
        assert dlg.exists(), "Open EasyLanguage Document dialog not found"
        return dlg

    # ------------------------------------------------------------------
    # Public API: open / close / verify
    # ------------------------------------------------------------------

    def open_item(self, component_type: str, name: str) -> None:
        """Open an existing item in TSDev by type and name.

        Args:
            component_type: One of :class:`ComponentType` constants.
            name: The stored name of the item.
        """
        dlg = self._open_dialog()
        dlg.ComboBox1.select(component_type)

        item_list = dlg.SysListView312
        item_list.wait("ready", timeout=self._timeout)
        assert item_list.exists()

        for item in item_list.items():
            if item.text().strip() == name:
                item.select()
                dlg.type_keys("{VK_RETURN}")
                self._set_focus()
                return

        # Not found — close dialog
        dlg.type_keys("{VK_ESCAPE}")
        raise ValueError(f"{component_type} '{name}' not found in TSDev")

    def verify(self) -> bool:
        """Trigger Verify (F3) on the currently open item.

        Returns:
            True if no errors, False if errors were detected.
        """
        self._wait_ready()
        self._set_focus()
        self._window.type_keys("{VK_F3}")
        # Wait for verify to complete (TSDev becomes ready again)
        time.sleep(1)
        self._wait_ready()
        return not self.has_errors()

    def close_current_tab(self) -> None:
        """Close the currently open editor tab (Ctrl+F4)."""
        self._wait_ready()
        self._window.type_keys("{VK_CONTROL down}{VK_F4}{VK_CONTROL up}")
        self._wait_ready()

    def minimize(self) -> None:
        """Minimize the TSDev main window."""
        self._window.minimize()

    # ------------------------------------------------------------------
    # Public API: get source
    # ------------------------------------------------------------------

    def get_source(self, component_type: str, name: str) -> str:
        """Open an item, copy its source to clipboard, close it, return source."""
        self.open_item(component_type, name)
        self._set_focus()
        self._window.type_keys("^a")
        self._window.type_keys("^c")
        source = pyperclip.paste()
        self.close_current_tab()
        return source

    # ------------------------------------------------------------------
    # Public API: list items
    # ------------------------------------------------------------------

    def list_items(self, component_type: str) -> list[str]:
        """Return the list of stored item names for the given type."""
        dlg = self._open_dialog()
        dlg.ComboBox1.select(component_type)

        item_list = dlg.SysListView312
        item_list.wait("ready", timeout=self._timeout)
        names = [
            item.text().strip()
            for item in item_list.items()
            if item.text().strip()
        ]
        dlg.type_keys("{VK_ESCAPE}")
        return names

    # ------------------------------------------------------------------
    # Public API: insert new item
    # ------------------------------------------------------------------

    # Dialog class names for "New <Type>" dialogs.
    # Strategy uses TS_IDD_COMMONCREATE (from reference code);
    # Function uses TS_IDD_FUNCCREATE (confirmed via UI inspection).
    # Other types likely follow a pattern; we map known ones and fall
    # back to title-based search.
    NEW_DLG_CLASSES = {
        ComponentType.STRATEGY: "TS_IDD_COMMONCREATE",
        ComponentType.FUNCTION: "TS_IDD_FUNCCREATE",
        ComponentType.INDICATOR: "TS_IDD_INDICATORCREATE",
        ComponentType.SHOWME: "TS_IDD_SHOWMECREATE",
        ComponentType.TRADINGAPP: "TS_IDD_TRADINGAPPCREATE",
        ComponentType.PAINTBAR: "TS_IDD_PAINTBARCREATE",
        ComponentType.ACTIVITYBAR: "TS_IDD_ACTIVITYBARCREATE",
        ComponentType.PROBABILITYMAP: "TS_IDD_PROBMAPCREATE",
    }

    # Menu accelerator keys for File → New → <type> submenu.
    NEW_DLG_ACCEL = {
        ComponentType.STRATEGY: "s",
        ComponentType.INDICATOR: "i",
        ComponentType.FUNCTION: "f",
        ComponentType.SHOWME: "m",
        ComponentType.TRADINGAPP: "t",
        ComponentType.PAINTBAR: "p",
        ComponentType.ACTIVITYBAR: "a",
        ComponentType.PROBABILITYMAP: "r",
    }

    def _find_new_dialog(self, component_type: str):
        """Find the 'New <Type>' dialog, trying known class names then title."""
        new_title = f"New {component_type}"
        # Try known class name first
        cls = self.NEW_DLG_CLASSES.get(component_type)
        if cls:
            try:
                dlg = self._app.window(class_name=cls, title=new_title)
                dlg.wait("ready", timeout=self._timeout)
                if dlg.exists():
                    return dlg
            except Exception:
                pass
        # Fallback: search by title only
        dlg = self._app.window(title=new_title)
        dlg.wait("ready", timeout=self._timeout)
        return dlg

    def insert_item(
        self,
        component_type: str,
        name: str,
        source_file: str,
        minimize_when_finished: bool = True,
    ) -> bool:
        """Insert a new item via the TSDev UI.

        Uses Alt+F → N → <type> menu sequence, pastes source from file,
        and verifies (F3).

        Args:
            component_type: One of :class:`ComponentType` constants.
            name: Name for the new item.
            source_file: Path to the EasyLanguage source file.
            minimize_when_finished: Whether to minimize TSDev after.

        Returns:
            True if verify succeeded (no errors), False otherwise.
        """
        self._wait_ready()
        self._set_focus()

        # Alt+F → N → <type accelerator>
        accel = self.NEW_DLG_ACCEL.get(component_type, "s")
        send_keys(
            f"%{{f down}}{{f up}}{{n down}}{{n up}}"
            f"{{{accel} down}}{{{accel} up}}",
            pause=0.2,
        )

        new_dlg = self._find_new_dialog(component_type)
        assert new_dlg.exists(), f"New {component_type} dialog not found"

        # Set the name. Use control_id for reliability (Edit control 10515
        # for Function; the reference code uses EditName for Strategy).
        # Try EditName first (works for Strategy/TS_IDD_COMMONCREATE),
        # then fall back to control_id-based lookup.
        try:
            name_edit = new_dlg.EditName
            name_edit.set_text(name)
        except Exception:
            # Function dialog uses control_id=10515 for the Name field
            name_edit = new_dlg.child_window(class_name="Edit", control_id=10515)
            name_edit.set_text(name)

        assert len(name_edit.text_block().strip()) > 0, "Name field is empty"

        # For Function, a Return Type dropdown is present (control_id=10762).
        # Default is "double (numeric)" which is correct for numeric functions.
        # No action needed unless the caller wants a different return type.

        new_dlg.child_window(class_name="Button", control_id=1).click()

        self._set_focus()

        # Clear existing content and paste new source
        self._window.type_keys("^a")
        self._window.type_keys("{VK_DELETE}")

        encoding = "utf-8-sig"
        source = Path(source_file).read_text(encoding=encoding)
        pyperclip.copy(source)
        self._window.wait("active ready", timeout=self._timeout)
        self._window.type_keys("+{INS}")  # Shift+Insert = paste

        # F3 = verify
        self._window.wait("ready active")
        self._window.type_keys("{VK_F3}")

        self._window.wait("ready active")
        success = not self.has_errors()

        # Close the editor tab so the item can be deleted later if needed.
        # An open editor tab locks the item and prevents deletion.
        self.close_current_tab()

        if minimize_when_finished:
            self.minimize()

        return success

    # ------------------------------------------------------------------
    # Public API: delete items
    # ------------------------------------------------------------------

    def delete_items(self, component_type: str, names: list[str]) -> None:
        """Delete one or more items via the Open dialog + Delete key.

        Closes any open editor tab first, since an open tab locks the
        item and prevents deletion.

        Args:
            component_type: One of :class:`ComponentType` constants.
            names: List of item names to delete.
        """
        # Close any open editor tab to release the lock on the item.
        self.close_current_tab()

        dlg = self._open_dialog()
        dlg.ComboBox1.select(component_type)

        item_list = dlg.SysListView312
        item_list.wait("ready", timeout=self._timeout)

        item_map = {
            item.text().strip(): item
            for item in item_list.items()
            if item.text().strip()
        }

        selected_any = False
        for name in names:
            if name not in item_map:
                continue
            item_map[name].select()
            selected_any = True

        if selected_any:
            dlg.type_keys("{VK_DELETE}")
            confirm = self._app.window(
                class_name=self.CONFIRM_DELETE_CLASS,
                title=self.CONFIRM_DELETE_TITLE,
            )
            confirm.wait("ready", timeout=self._timeout)
            confirm.set_focus()
            # Click "Yes to all" (ctrl_id=6) to confirm deletion without
            # per-item prompts. Falls back to "Yes" (ctrl_id=1) if not found.
            try:
                confirm.child_window(class_name="Button", control_id=6).click()
            except Exception:
                confirm.child_window(class_name="Button", control_id=1).click()

        dlg.type_keys("{VK_ESCAPE}")
        self.minimize()

    # ------------------------------------------------------------------
    # Error checking
    # ------------------------------------------------------------------

    def has_errors(self) -> bool:
        """Check if the Output window shows any error lines."""
        return len(self.get_error_lines()) > 0

    def get_error_lines(self) -> list[str]:
        """Return error lines from the Output window, or empty list if none."""
        try:
            output_window = self._window.child_window(
                title="Output",
                class_name_re=r"BCGPControlBar\:.*",
                ctrl_index=0,
            )
            output_window.wait("exists", timeout=self._timeout)
        except Exception:
            return []

        output_list = self._window.Output.ListView2
        if not output_list.exists():
            return []

        errors = []
        for row in range(output_list.item_count()):
            description = output_list.item(row, 0).text()
            if re.match(
                r"^(.*\s)?[1-9]+[0-9]* error(\(s\))?.*$",
                description,
                re.MULTILINE | re.IGNORECASE,
            ):
                row_text = " | ".join(
                    output_list.item(row, col).text()
                    for col in range(output_list.column_count())
                )
                errors.append(row_text)
        return errors

    def get_output_lines(self) -> list[str]:
        """Return all lines from the Output window."""
        try:
            output_window = self._window.child_window(
                title="Output",
                class_name_re=r"BCGPControlBar\:.*",
                ctrl_index=0,
            )
            output_window.wait("exists", timeout=self._timeout)
        except Exception:
            return []

        output_list = self._window.Output.ListView2
        if not output_list.exists():
            return []

        lines = []
        for row in range(output_list.item_count()):
            row_text = " | ".join(
                output_list.item(row, col).text()
                for col in range(output_list.column_count())
            )
            lines.append(row_text)
        return lines
