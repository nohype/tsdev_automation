# SPDX-License-Identifier: MIT
"""Framed-JSON named pipe I/O for ``tsdev_cli`` serve mode.

The serve broker (``tsdev_cli.exe serve``) listens on a Windows named pipe and
services JSON requests.  Each request/response is a length-prefixed (4-byte
little-endian uint32) UTF-8 JSON payload::

    +----+----+----+----+----+----+ ... ----+
    | length (uint32 LE)  | payload (bytes) |
    +----+----+----+----+----+----+ ... ----+

This module is **stdlib-only** (``ctypes`` for ``CreateFileW``/``WriteFile``/
``ReadFile``/``CloseHandle``, ``struct`` for the length prefix, ``json`` for
the payload).  It is Windows-specific by design (the broker only runs on
Windows where ``orlib20.dll`` lives).
"""

from __future__ import annotations

import ctypes
import json
import struct
from ctypes import wintypes

# --- Windows API bindings via ctypes ---------------------------------------

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

_kernel32 = ctypes.windll.kernel32
_kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
    ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
_kernel32.CreateFileW.restype = wintypes.HANDLE
_kernel32.CreateFileW.use_last_error = True
_kernel32.WriteFile.argtypes = [
    wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
]
_kernel32.WriteFile.restype = wintypes.BOOL
_kernel32.ReadFile.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
]
_kernel32.ReadFile.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.restype = wintypes.BOOL
_kernel32.GetLastError.argtypes = []
_kernel32.GetLastError.restype = wintypes.DWORD


class PipeError(Exception):
    """Raised on a named-pipe I/O failure (connect / read / write)."""


def connect(pipe_name: str, timeout_ms: int = 5000) -> int:
    """Connect to a named pipe.

    ``pipe_name`` is the full pipe path (e.g. ``\\\\.\\pipe\\tsdev_cli_1234``).
    Returns the pipe handle (as an int) on success.  Raises :class:`PipeError`
    on failure.

    The broker may not have called ``ConnectNamedPipe`` yet by the time we
    first try to connect, so we retry for up to ``timeout_ms`` milliseconds
    (polling every 100 ms) before giving up.
    """
    deadline_step = 100  # ms between retries
    elapsed = 0
    handle = INVALID_HANDLE_VALUE
    while elapsed < timeout_ms:
        handle = _kernel32.CreateFileW(
            pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        if handle != INVALID_HANDLE_VALUE:
            return int(handle)
        # ERROR_PIPE_BUSY (232) or ERROR_FILE_NOT_FOUND (2): broker not ready
        # yet — sleep and retry.  Any other error: bail out immediately.
        err = _kernel32.GetLastError()
        if err not in (2, 232):
            raise PipeError(f"CreateFileW failed for {pipe_name!r} (err={err})")
        _sleep_ms(deadline_step)
        elapsed += deadline_step
    raise PipeError(f"timed out connecting to pipe {pipe_name!r} after {timeout_ms}ms")


def send_request(handle: int, obj: dict) -> None:
    """Send a framed JSON request on the pipe.

    Serializes ``obj`` to UTF-8 JSON, prefixes a 4-byte little-endian uint32
    length, and writes the whole frame.  Raises :class:`PipeError` on a
    write failure.
    """
    payload = json.dumps(obj).encode("utf-8")
    hdr = struct.pack("<I", len(payload))
    _write_all(handle, hdr)
    _write_all(handle, payload)


def recv_response(handle: int) -> dict:
    """Receive a framed JSON response from the pipe.

    Reads the 4-byte length prefix, then the payload, and parses it as JSON.
    Raises :class:`PipeError` on a read failure or :class:`json.JSONDecodeError`
    on a malformed payload.
    """
    hdr = _read_exact(handle, 4)
    (n,) = struct.unpack("<I", hdr)
    if n > 67108864:  # 64 MiB cap
        raise PipeError(f"frame size {n} exceeds 64MiB cap")
    if n == 0:
        return {}
    payload = _read_exact(handle, n)
    return json.loads(payload.decode("utf-8"))


def close(handle: int) -> None:
    """Close the pipe handle.  Safe to call multiple times."""
    if handle is None or handle == 0 or handle == INVALID_HANDLE_VALUE:
        return
    try:
        _kernel32.CloseHandle(wintypes.HANDLE(handle))
    except OSError:
        pass


# --- internals -------------------------------------------------------------

def _write_all(handle: int, data: bytes) -> None:
    """Write all bytes, looping on partial writes."""
    written = wintypes.DWORD(0)
    total = 0
    while total < len(data):
        ok = _kernel32.WriteFile(
            wintypes.HANDLE(handle),
            data[total:],
            wintypes.DWORD(len(data) - total),
            ctypes.byref(written),
            None,
        )
        if not ok or written.value == 0:
            err = ctypes.windll.kernel32.GetLastError()
            raise PipeError(f"WriteFile failed (err={err})")
        total += written.value


def _read_exact(handle: int, n: int) -> bytes:
    """Read exactly ``n`` bytes, looping on partial reads."""
    buf = (ctypes.c_ubyte * n)()
    got = wintypes.DWORD(0)
    total = 0
    while total < n:
        ok = _kernel32.ReadFile(
            wintypes.HANDLE(handle),
            ctypes.addressof(buf) + total,
            wintypes.DWORD(n - total),
            ctypes.byref(got),
            None,
        )
        if not ok or got.value == 0:
            err = ctypes.windll.kernel32.GetLastError()
            raise PipeError(f"ReadFile failed (err={err})")
        total += got.value
    return bytes(buf[:n])


def _sleep_ms(ms: int) -> None:
    """Sleep for ``ms`` milliseconds via kernel32.Sleep (avoids time import)."""
    _kernel32.Sleep(wintypes.DWORD(ms))
