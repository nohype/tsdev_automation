"""Fail-loud ASCII hygiene check for source comments.

Scans every *.cpp/*.h/*.py/*.ps1/*.psm1 file under src/ and tests/
(skipping __pycache__) and reports non-ASCII characters that appear
outside string/char literals -- i.e. in comments, docstrings, and code
(where they never belong). String literal *contents* are ignored: a few
test-data literals legitimately hold non-ASCII runtime values (written
with ASCII \\u escapes), and the CLI's pinned output contract forbids
touching message literals.

Exit 0 when clean, exit 1 with a file:line:col listing otherwise.
Stdlib only, no TS dependency, safe for CI on any machine.
"""

import ast
import io
import pathlib
import sys
import tokenize

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET_DIRS = ("src", "tests")
EXTENSIONS = (".cpp", ".h", ".py", ".ps1", ".psm1")


def _is_ascii(ch):
    return ord(ch) < 128


def check_cpp(text):
    """Yield (line, col, char) for non-ASCII outside C++ literals."""
    findings = []
    lines = text.split("\n")
    in_block = False
    in_raw_delim = None
    in_str = False  # continued "..." via backslash-newline
    for ln, line in enumerate(lines, 1):
        i = 0
        n = len(line)
        if in_raw_delim is not None:
            end = line.find(in_raw_delim)
            if end == -1:
                continue
            i = end + len(in_raw_delim)
            in_raw_delim = None
        if in_block:
            end = line.find("*/")
            if end == -1:
                for col, ch in enumerate(line):
                    if not _is_ascii(ch):
                        findings.append((ln, col + 1, ch))
                continue
            for col in range(end + 2):
                if not _is_ascii(line[col]):
                    findings.append((ln, col + 1, line[col]))
            i = end + 2
            in_block = False
        quote = '"' if in_str else None
        in_str = False
        while i < n:
            c = line[i]
            if quote is not None:
                if c == "\\":
                    i += 2
                    continue
                if c == quote:
                    quote = None
                i += 1
                continue
            if c == "/" and i + 1 < n and line[i + 1] == "/":
                for j in range(i + 2, n):
                    if not _is_ascii(line[j]):
                        findings.append((ln, j + 1, line[j]))
                break
            if c == "/" and i + 1 < n and line[i + 1] == "*":
                end = line.find("*/", i + 2)
                seg = line if end == -1 else line[:end]
                for j in range(i + 2, len(seg)):
                    if not _is_ascii(seg[j]):
                        findings.append((ln, j + 1, seg[j]))
                if end == -1:
                    in_block = True
                    break
                i = end + 2
                continue
            if c == "R" and i + 1 < n and line[i + 1] == '"':
                d = i + 2
                while d < n and line[d] not in "( \t":
                    d += 1
                if d < n and line[d] == "(":
                    delim = ")" + line[i + 2:d] + '"'
                    end = line.find(delim, d + 1)
                    if end == -1:
                        in_raw_delim = delim
                        break
                    i = end + len(delim)
                    continue
            if c in ("'", '"'):
                quote = c
                i += 1
                continue
            if not _is_ascii(c):
                # Outside any literal: comments, asm ";" remarks, or code.
                findings.append((ln, i + 1, c))
            i += 1
        if quote == '"' and line.endswith("\\"):
            in_str = True
    return findings


def check_python(text):
    """Yield (line, col, char) for non-ASCII in comments/docstrings."""
    findings = []
    try:
        tree = ast.parse(text)
    except SyntaxError:
        tree = None
    doc_spans = set()
    if tree is not None:
        for node in ast.walk(tree):
            if isinstance(
                node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
            ):
                body = node.body
                if (
                    body
                    and isinstance(body[0], ast.Expr)
                    and isinstance(body[0].value, ast.Constant)
                    and isinstance(body[0].value.value, str)
                ):
                    doc_spans.add((body[0].lineno, body[0].end_lineno))
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, SyntaxError):
        for ln, line in enumerate(text.split("\n"), 1):
            for col, ch in enumerate(line):
                if not _is_ascii(ch):
                    findings.append((ln, col + 1, ch))
        return findings
    tok_types = {tokenize.COMMENT}
    for tok in tokens:
        if tok.type == tokenize.COMMENT:
            for lno, line in enumerate(
                text.split("\n")[tok.start[0] - 1 : tok.end[0]], tok.start[0]
            ):
                s = tok.start[1] if lno == tok.start[0] else 0
                e = tok.end[1] if lno == tok.end[0] else len(line)
                for col in range(s, min(e, len(line))):
                    if not _is_ascii(line[col]):
                        findings.append((lno, col + 1, line[col]))
        elif tok.type == tokenize.STRING:
            if (tok.start[0], tok.end[0]) in doc_spans:
                for lno, line in enumerate(
                    text.split("\n")[tok.start[0] - 1 : tok.end[0]], tok.start[0]
                ):
                    s = tok.start[1] if lno == tok.start[0] else 0
                    e = tok.end[1] if lno == tok.end[0] else len(line)
                    for col in range(s, min(e, len(line))):
                        if not _is_ascii(line[col]):
                            findings.append((lno, col + 1, line[col]))
    return findings


def check_ps1(text):
    """Yield (line, col, char) for non-ASCII outside PS1 strings."""
    findings = []
    lines = text.split("\n")
    in_block = False
    in_here = None  # '"@' or "'@"
    for ln, line in enumerate(lines, 1):
        i = 0
        n = len(line)
        if in_here is not None:
            if line.strip() in (in_here, in_here + ";"):
                in_here = None
            continue
        stripped = line.lstrip()
        if stripped.endswith('@"') or stripped.endswith("@'"):
            in_here = '"@' if stripped.endswith('@"') else "'@"
            continue
        if in_block:
            end = line.find("#>")
            seg = line if end == -1 else line[:end]
            for col, ch in enumerate(seg):
                if not _is_ascii(ch):
                    findings.append((ln, col + 1, ch))
            if end != -1:
                in_block = False
            continue
        quote = None
        while i < n:
            c = line[i]
            if quote is not None:
                if c == "`" and quote == '"':
                    i += 2
                    continue
                if c == quote:
                    if quote == "'" and i + 1 < n and line[i + 1] == "'":
                        i += 2
                        continue
                    quote = None
                i += 1
                continue
            if c == "#":
                for j in range(i, n):
                    if not _is_ascii(line[j]):
                        findings.append((ln, j + 1, line[j]))
                break
            if c == "<" and i + 1 < n and line[i + 1] == "#":
                in_block = True
                i += 2
                rest = line[i:]
                end = rest.find("#>")
                seg = rest if end == -1 else rest[:end]
                for k, ch in enumerate(seg):
                    if not _is_ascii(ch):
                        findings.append((ln, i + k + 1, ch))
                if end != -1:
                    in_block = False
                    i += end + 2
                    continue
                break
            if c in ("'", '"'):
                quote = c
                i += 1
                continue
            if not _is_ascii(c):
                findings.append((ln, i + 1, c))
            i += 1
    return findings


CHECKERS = {".cpp": check_cpp, ".h": check_cpp, ".py": check_python,
            ".ps1": check_ps1, ".psm1": check_ps1}


def iter_targets():
    for d in TARGET_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or "__pycache__" in str(p):
                continue
            if p.suffix.lower() in CHECKERS:
                yield p


def main():
    total = 0
    checked = 0
    for path in iter_targets():
        checked += 1
        try:
            text = path.read_bytes().decode("utf-8")
        except UnicodeDecodeError as exc:
            print(f"{path}: undecodable as UTF-8: {exc}")
            total += 1
            continue
        for ln, col, ch in CHECKERS[path.suffix.lower()](text):
            print(f"{path.relative_to(ROOT)}:{ln}:{col}: U+{ord(ch):04X}")
            total += 1
    print(f"checked {checked} files, {total} non-ASCII comment findings")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
