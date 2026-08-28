import re
from pathlib import Path

import pyperclip
from pywinauto.application import Application
from pywinauto.keyboard import send_keys


class TSDevHandler:
    def __init__(self) -> None:
        try:
            self._ts_dev_app = Application(backend="win32").connect(path="TSDev.exe")
        except Exception as exc:
            raise RuntimeError("TSDev.exe is not running or could not be connected to") from exc

        self._ts_dev_window = self._ts_dev_app.window(title_re=r"TradeStation Development Environment( \- .*)?")
        self._main_window_timeout = 10

    def _insert_strategy(
        self,
        file_path: str,
        strategy_name: str,
        minimize_when_finished: bool = True,
    ) -> bool:
        self._ts_dev_window.wait("ready", timeout=self._main_window_timeout)

        # Alt+F > N > S: create new strategy
        send_keys("%{f down}{f up}{n down}{n up}{s down}{s up}", pause=0.2)

        new_strategy_window = self._ts_dev_app.window(class_name="TS_IDD_COMMONCREATE", title="New Strategy")
        new_strategy_window.wait("ready", timeout=self._main_window_timeout)
        assert new_strategy_window.exists()

        new_strategy_window.EditName.set_text(strategy_name)

        strategy_name = str(new_strategy_window.EditName.text_block())
        print("[I] ACTUAL STRAT NAME:", strategy_name)
        assert len(strategy_name.strip()) > 0

        new_strategy_window.ButtonOK.click()

        self._ts_dev_window.set_focus()
        self._ts_dev_window.wait("active ready", timeout=self._main_window_timeout)

        self._ts_dev_window.type_keys("^a")
        self._ts_dev_window.type_keys("{VK_DELETE}")

        encoding = "utf-8-sig"
        strategy_code = Path(file_path).read_text(encoding=encoding)
        pyperclip.copy(strategy_code)
        self._ts_dev_window.wait("active ready", timeout=self._main_window_timeout)
        self._ts_dev_window.type_keys("+{INS}")

        # F3 = verify
        self._ts_dev_window.wait("ready active")
        self._ts_dev_window.type_keys("{VK_F3}")

        self._ts_dev_window.wait("ready active")
        self._ts_dev_window.type_keys("{VK_CONTROL down}{VK_F4}{VK_CONTROL up}")

        success = not self.has_error_output()

        if minimize_when_finished:
            self._ts_dev_window.minimize()

        return success

    def _open_strategy_list_dialog(self):
        """Open the EasyLanguage Document dialog and select Strategy."""
        open_doc_window_candidates = self._ts_dev_app.windows(
            class_name="TS_IDD_OPENDLG", title="Open EasyLanguage Document"
        )
        if len(open_doc_window_candidates) < 1:
            self._ts_dev_window.wait("ready", timeout=self._main_window_timeout)
            self._ts_dev_window.set_focus()
            self._ts_dev_window.type_keys("^o")

        open_doc_window = self._ts_dev_app.window(class_name="TS_IDD_OPENDLG", title="Open EasyLanguage Document")
        open_doc_window.wait("ready", timeout=self._main_window_timeout)
        open_doc_window.set_focus()
        assert open_doc_window.exists()

        open_doc_window.ComboBox1.select("Strategy")

        strategy_list = open_doc_window.SysListView312
        strategy_list.wait("ready", timeout=self._main_window_timeout)
        assert strategy_list.exists()

        return open_doc_window, strategy_list

    def _open_function_list_dialog(self):
        open_doc_window_candidates = self._ts_dev_app.windows(
            class_name="TS_IDD_OPENDLG", title="Open EasyLanguage Document"
        )
        if len(open_doc_window_candidates) < 1:
            self._ts_dev_window.wait("ready", timeout=self._main_window_timeout)
            self._ts_dev_window.set_focus()
            self._ts_dev_window.type_keys("^o")

        open_doc_window = self._ts_dev_app.window(class_name="TS_IDD_OPENDLG", title="Open EasyLanguage Document")
        open_doc_window.wait("ready", timeout=self._main_window_timeout)
        open_doc_window.set_focus()
        assert open_doc_window.exists()

        open_doc_window.ComboBox1.select("Function")

        strategy_list = open_doc_window.SysListView312
        strategy_list.wait("ready", timeout=self._main_window_timeout)
        assert strategy_list.exists()

        return open_doc_window, strategy_list

    def get_existing_strategies(self) -> list[str]:
        open_doc_window, strategy_list = self._open_strategy_list_dialog()

        strategy_names = [item.text().strip() for item in strategy_list.items() if len(item.text().strip()) > 0]

        open_doc_window.type_keys("{VK_ESCAPE}")

        return strategy_names

    def get_existing_functions(self) -> list[str]:
        open_doc_window, function_list = self._open_function_list_dialog()

        function_names = [item.text().strip() for item in function_list.items() if len(item.text().strip()) > 0]

        open_doc_window.type_keys("{VK_ESCAPE}")

        return function_names

    def export_function(self, name: str) -> str:
        open_doc_window, function_list = self._open_function_list_dialog()

        for item in function_list.items():
            if item.text().strip() != name:
                continue

            item.select()
            open_doc_window.type_keys("{VK_RETURN}")
            break

        self._ts_dev_window.set_focus()
        self._ts_dev_window.wait("active ready", timeout=self._main_window_timeout)

        self._ts_dev_window.type_keys("^a")
        self._ts_dev_window.type_keys("^c")
        function_code = pyperclip.paste()

        self._ts_dev_window.wait("ready active")
        self._ts_dev_window.type_keys("{VK_CONTROL down}{VK_F4}{VK_CONTROL up}")

        self._ts_dev_window.wait("active ready", timeout=self._main_window_timeout)

        return function_code

    def export_all_functions(self) -> dict[str, str]:
        open_doc_window, function_list = self._open_function_list_dialog()

        function_names = [item.text().strip() for item in function_list.items() if len(item.text().strip()) > 0]
        open_doc_window.type_keys("{VK_ESCAPE}")

        function_code_map = {}
        for name in function_names:
            function_code = self.export_function(name)
            function_code_map[name] = function_code

        return function_code_map

    def delete_strategies(self, strategy_names: list[str]):
        open_doc_window, strategy_list = self._open_strategy_list_dialog()

        item_name_map = {item.text().strip(): item for item in strategy_list.items() if len(item.text().strip()) > 0}

        def has_history_version(strategy_name: str):
            return any([re.match(rf"^{re.escape(strategy_name)}_HIST(O|OR|ORY)?$", x) for x in item_name_map.keys()])

        def get_history_version(strategy_name: str):
            for name, item in item_name_map.items():
                if re.match(rf"^{re.escape(strategy_name)}_HIST(O|OR|ORY)?$", name):
                    return item

            return None

        def is_history_version(strategy_name: str):
            return re.match(r"^.+_HIST(O|OR|ORY)?$", strategy_name)

        selected_some = False
        for strat_name in strategy_names:
            if strat_name not in item_name_map:
                print(f"[!] Not deleting {strat_name}: Not existing.")
                continue

            strat_item = item_name_map[strat_name]
            strat_item.select()
            selected_some = True

            # select matching HISTORY version if present
            if not is_history_version(strat_name):
                history_strat_item = get_history_version(strat_name)
                if history_strat_item is not None:
                    history_strat_item.select()

        if selected_some:
            open_doc_window.type_keys("{VK_DELETE}")

            confirm_removal_window = self._ts_dev_app.window(
                class_name="TS_IDD_CONFIRM_DELETE", title="Confirm file removal"
            )
            confirm_removal_window.wait("ready", timeout=self._main_window_timeout)
            confirm_removal_window.set_focus()
            confirm_removal_window.type_keys("%{a}")

        open_doc_window.type_keys("{VK_ESCAPE}")

        self._ts_dev_window.minimize()

    def insert_strategy_files_into_ts_dev(self, file_paths: list[str]) -> list[str]:
        path_name_map = {fp: Path(fp).stem.strip() for fp in file_paths}
        existing_strategies = self.get_existing_strategies()

        inserted_strategies = []
        count = 0
        for file_path, strategy_name in path_name_map.items():
            if strategy_name in existing_strategies:
                print(f"[!] Not inserting {strategy_name}: Already existing.")
                continue

            count += 1
            print(f"{count}/{len(path_name_map)}", end=" ")
            success = self._insert_strategy(
                file_path=file_path,
                strategy_name=strategy_name,
                minimize_when_finished=False,
            )
            if not success:
                print(f"[!] Failed to insert '{strategy_name}'. Aborting.")
                break

            inserted_strategies.append(strategy_name)

        self._ts_dev_window.minimize()

        return inserted_strategies

    def has_error_output(self) -> bool:
        output_window = self._ts_dev_window.child_window(
            title="Output", class_name_re=r"BCGPControlBar\:.*", ctrl_index=0
        )
        output_window.wait("exists", timeout=self._main_window_timeout)
        assert output_window.exists()

        output_list_view = self._ts_dev_window.Output.ListView2
        if output_list_view.exists():
            for row in range(output_list_view.item_count()):
                description = output_list_view.item(row, 0).text()
                if re.match(
                    r"^(.*\s)?[1-9]+[0-9]* error(\(s\))?.*$",
                    description,
                    re.MULTILINE | re.IGNORECASE,
                ):
                    print(
                        f"[!] ERROR: {'|'.join([output_list_view.item(row, col).text() for col in range(output_list_view.column_count())])}"
                    )
                    return True

        return False
