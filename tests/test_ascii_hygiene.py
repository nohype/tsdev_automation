"""Source-hygiene gate: no non-ASCII characters in source comments.

Enforces RC-15: every *.cpp/*.h/*.py/*.ps1/*.psm1 file under src/ and
tests/ must be pure ASCII outside string/char literals (comments,
docstrings, and code). String literal *contents* are exempt: a few
test-data literals hold non-ASCII runtime values (written with ASCII
\\u escapes), and the CLI's pinned output contract forbids touching
message literals.

Stdlib only, no TS dependency, no elevation -- safe for CI and -NoTs.
"""

import importlib.util
import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKER = REPO_ROOT / "tools" / "check_ascii.py"


def _load_checker():
    spec = importlib.util.spec_from_file_location("check_ascii", CHECKER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_no_non_ascii_in_comments():
    checker = _load_checker()
    findings = []
    for path in checker.iter_targets():
        text = path.read_bytes().decode("utf-8")
        for line, col, char in checker.CHECKERS[path.suffix.lower()](text):
            findings.append(f"{path.relative_to(checker.ROOT)}:{line}:{col}: U+{ord(char):04X}")
    assert not findings, (
        f"{len(findings)} non-ASCII comment findings (see tools/check_ascii.py):\n"
        + "\n".join(findings[:20])
    )


def test_checker_covers_all_sources():
    checker = _load_checker()
    targets = list(checker.iter_targets())
    assert len(targets) > 100, f"checker found only {len(targets)} files"
