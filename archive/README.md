# Archived Experimental Probes

This directory contains **archived experimental probes** from the TSDev Automation project.

## Purpose

These probes were used during the **reverse engineering** and **development** of the
`orlib20.dll` integration that powers the TSDev Automation CLI. They include the
headless init smoke test, parameter-order probes, persistence experiments, DLL
injection / hooking prototypes, and signature-scanning probes that progressively
confirmed the orlib20 primitive set, ABI details, and standalone persistence
mechanism.

## Superseded by production code

All of these probes are **superseded** by the production code in the project:

- `tsdev_lib` — shared static library encapsulating the confirmed orlib20
  operation set (init, lookup, create, save, compile/verify, close).
- `orlib_resolver` — static library encapsulating version-specific orlib20.dll
  knowledge and signature resolution.
- `sigtool` — standalone orlib20.dll signature verification tool.
- `tsdev_cli` — production CLI exposing the `insert`, `delete`, and `modify`
  subcommands that satisfy the project's primary acceptance criteria.

## Status

- These files are kept **for historical reference only**.
- They are **NOT part of the production build**. No `CMakeLists.txt` target
  references them; they are not compiled.
- They should **NOT be modified** or used in production.
