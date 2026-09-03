# Archive — discarded approach (REFERENCE ONLY)

This directory contains the previous implementation of tsdev_automation,
which interfaced with the TradeStation library **directly** (orlib20.dll
wrapper, signature scanning, in-memory patching) **without TSDev.exe**.

That approach is discarded on this branch. The branch now drives TSDev.exe
itself — see `../AGENTS.md` for the binding requirements and rules.

**Not for production use on this branch.** Do not build, import, or extend
this code, and do not treat anything in here as current truth. It is kept
only as reference for implementing teams (patterns, RE history, test/CI
layout ideas).

Contents:
- `src/` — old C++ CLI (orlib20 wrapper, signature resolver) + old Python
  package `tsdev_automation/`. Note: `tsdev_ui*.py` automate TSDev.exe's UI
  (pywinauto + clipboard) — a banned technique for the product, but they
  record TSDev UI-path knowledge (verify trigger, error-pane reading,
  BCGSoft owner-drawn menus).
- `tests/` — old test infrastructure (NoTs vs elevated-E2E split,
  PowerShell runners compatible with PS 5.1 and pwsh 7).
- `.github/workflows/ci.yml` — old CI (TradeStation-less build, mocked
  Python tests, release-bundle-on-tag pattern).
- `CMakeLists.txt`, `pyproject.toml`, `uv.lock`, `tools/`,
  `third_party/` — old build and packaging.

orlib20-specific RE material (signature sets, sigcodegen, diag_m4 probes,
the orlib20 DLL fixture) was deleted outright; it remains reachable in git
history (commits before the archive restructure).
