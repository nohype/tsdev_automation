# RE-F5: BST Node Structure & Verbatim Name Storage in orlib20.dll

## Executive Summary

**The BST node does NOT store the verbatim (original-case) name.** It stores only the lowercase-normalized name as its key. However, the BST node's value is a pointer to a **CELProgram** data node, which **does** store the verbatim name — but **lazily**, fetched from the server on first access via `FUN_101c36d0` (RVA `0x1c36d0`, server command `0x34`).

There is an existing orlib20 function — `FUN_101a42d0` (RVA `0x1a42d0`) — that combines BST lookup + verbatim name retrieval in a single call. This is the official TSDev mechanism for obtaining the verbatim stored name.

---

## 1. BST Node Structure (Field Offsets)

The BST is a **two-level tree** (both levels are MSVC `std::map` red-black trees):

### Outer Tree: `std::map<int ProgId, InnerTree>`

Traversed by `FUN_101e30f0` (RVA `0x1e30f0`), keyed by ProgId (int).

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| +0x00 | `void*` | `_Left` | Left child |
| +0x04 | `void*` | `_Parent` | Parent |
| +0x08 | `void*` | `_Right` | Right child |
| +0x0C | `byte` | `_Color` | RB color |
| +0x0D | `byte` | `_Isnil` | Sentinel flag |
| +0x10 | `int` | `key` (ProgId) | `puVar3[4]` in traversal |
| +0x14 | `CELProgram*` | value (data node) | Set by `FUN_101e35a0` |
| +0x1C | `InnerTree` | inner tree root | Root of the name-keyed subtree |

### Inner Tree: `std::map<std::string, CELProgram*>`

Traversed by `FUN_100269b0` (RVA `0x100269b0`), keyed by lowercase-normalized name string. The inner tree is rooted at `outer_node + 0x1C`.

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| +0x00 | `void*` | `_Left` | Left child |
| +0x04 | `void*` | `_Parent` | Parent |
| +0x08 | `void*` | `_Right` | Right child |
| +0x0C | `byte` | `_Color` | RB color |
| +0x0D | `byte` | `_Isnil` | Sentinel flag |
| +0x10 | `char[16]` | `_Myval.first` (std::string SSO buffer) | Inline if capacity ≤ 0xF, else heap pointer |
| +0x20 | `uint` | string length | `puVar7[8]` |
| +0x24 | `uint` | string capacity | `puVar7[9]`; if > 0xF, +0x10 is a pointer |
| +0x28 | `CELProgram*` | `_Myval.second` (value) | **The data node pointer** |

**The inner tree key at +0x10 is the LOWERCASE-normalized name** — confirmed by `FUN_101e3db0` which calls `FUN_1002b700` (lowercase normalization) on the search name before comparing it against node keys via `FUN_100269b0`.

---

## 2. CELProgram Data Node Structure (0x241 bytes)

The BST value at inner node `+0x28` points to a **CELProgram** object. Constructor: `FUN_101bfb00` (RVA `0x1bfb00`), sets `*this = CELProgram::vftable`.

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| +0x00 | `void*` | vftable | `CELProgram::vftable` |
| +0x04 | `int` | unknown | Set to 0 in ctor |
| +0x08 | `int` | unknown | Set to 0 in ctor |
| +0x0C | `int` | warehouse handle | Used for server requests |
| +0x10 | `int` | cached ProgId | Lazy-init by `FUN_101c2e90` / `FUN_101c4990` |
| +0x14 | `byte` | type-init flag | Set to 1 by `FUN_101c4ae0` |
| +0x15 | `short` | type (whType) | Set by `FUN_101c4ae0`; returned by `FUN_101c4990` |
| +0x17 | `byte` | progid-init flag | Set to 1 by `FUN_101c2e90` / `FUN_101c4ae0` |
| +0x18 | `int` | ProgId | Lazy-init by `FUN_101c2e90`; returned at end |
| +0x1C | `byte` | **name-init flag** | Checked by `FUN_101c36d0`; 0 = not yet fetched |
| **+0x1D** | **`char*`** | **verbatim name (ANSI)** | **Lazily allocated by `FUN_101c36d0`** |
| **+0x21** | **`wchar_t*`** | **verbatim name (Wide)** | **Lazily allocated by `FUN_101c36d0`** |
| +0x25 | `short` | unknown | Set to 0 in ctor |
| +0x27 | `std::string` | unknown string field | SSO, capacity 0xF initially |
| ... | ... | ... | Various fields |
| +0x210 | `int` | **refcount** | Atomic inc/dec; `param_2 + 0x84` (×4 = +0x210) in FindExistPos |
| +0x218 | `void*` | `CSyncObject*` | Lock for thread-safe lazy init |

---

## 3. Is the Verbatim Name Stored in the BST Node?

### NO — the BST node stores only the lowercase key.

The BST inner node key (`+0x10`, std::string) is the **lowercase-normalized** name. This is confirmed by:

1. **`FUN_101e3db0`** (BST lookup, RVA `0x1e3db0`): Calls `FUN_1002b700` (lowercase normalizer) on the search name **before** comparing it against BST node keys via `FUN_100269b0`. The comparison is byte-for-byte (memcmp-style) against the already-lowercased node key.

2. **`FUN_101e35a0`** (BST cache-fill insert, RVA `0x1e35a0`): Calls `FUN_101e3180` to insert the node. The key passed is the lowercase-normalized name (the same normalization is applied before insertion).

3. **`FUN_1002b700`** (name normalization, RVA `0x1002b700`): Creates a **new** lowercase string via `FUN_1002b680` (which applies `tolower` character-by-character). It does **not** modify the original name in-place — the original is preserved in the caller's buffer.

### YES — the verbatim name IS stored in the CELProgram data node (at +0x1D).

The BST node's value (`+0x28`) is a `CELProgram*` pointer. The CELProgram object stores the verbatim name at:
- **`+0x1D`**: `char*` (ANSI verbatim name)
- **`+0x21`**: `wchar_t*` (Wide verbatim name)

However, these fields are **NOT populated at creation time**. They are **lazily initialized** by `FUN_101c36d0` (RVA `0x1c36d0`) on first access, which fetches the name from the server via command `0x34`.

---

## 4. How to Access the Verbatim Name

### Method A: `FUN_101c36d0` (RVA `0x1c36d0`) — Verbatim Name Getter

**Signature (fastcall):**
```cpp
char* __fastcall FUN_101c36d0(CELProgram* this);  // ECX = data node
```

**Behavior:**
1. Checks `this->name_init_flag` (`+0x1C`). If already initialized, returns `this->verbatim_name_ansi` (`+0x1D`) immediately.
2. If not initialized:
   - Acquires lock (`CSingleLock` on `this->lock` at `+0x218`)
   - Sets `name_init_flag = 1`
   - Calls `FUN_101c82b0()` (some setup)
   - Lazy-inits ProgId if needed (`FUN_101cfce0`)
   - Sets `warehouse->field_0x5E = this->cached_ProgId`
   - **Sends server command `0x34`** via `FUN_101cf5b0(0x34)` — this is a blocking server round-trip
   - If server response OK (`warehouse->field_0x4E == 0`):
     - Gets the name string from `warehouse->field_0x46`
     - Creates a `_com` object, calls `FUN_10009dc0` with code `0xFDE9`
     - Converts to `CString` (from 128-byte local buffer)
     - **Allocates heap memory and copies** the name to `this->+0x1D` (ANSI) and `this->+0x21` (Wide)
   - Releases lock
3. Returns `this->verbatim_name_ansi` (`+0x1D`)

**Key insight:** This function performs a **server round-trip** (command `0x34`) on first call. Subsequent calls return the cached pointer directly.

### Method B: `FUN_101a42d0` (RVA `0x1a42d0`) — Combined BST Search + Verbatim Name

**Signature (fastcall):**
```cpp
undefined4 __fastcall FUN_101a42d0(
    char*    searchName,     // EDX — name to search for
    undefined4 type,         // [stack] — component type
    undefined4 warehouse,    // [stack] — warehouse handle
    char**   outVerbatimName // [stack] — output: verbatim name pointer
);
```

**Behavior:**
1. Validates search name is non-empty
2. If `DAT_1023aa8f == 1` (server mode flag):
   - Calls `FUN_101d04f0(type, warehouse, &dataNode)` — BST search, returns data node in `local_8`
   - Calls `FUN_101c36d0()` with `ECX = dataNode` — gets verbatim name `char*`
   - Sets `*outVerbatimName = verbatimName`
   - Compares first 8 chars via `_strnicmp(verbatimName, searchName, 8)`
   - Releases data node (refcount decrement)
   - Returns `warehouse` (success) or `0xFFFFFFFF` (failure)
3. If not server mode:
   - Uses `FUN_101d05e0` (BST search) + `FUN_101c2e90` (ProgId extraction)
   - Returns ProgId

**This is the official TSDev function for "find item + get verbatim name".** It is called from 33+ locations throughout orlib20.dll.

---

## 5. Where the Verbatim Name Is Stored for In-Session Creates

### Creation Flow (`_CreateEntryEX@24`, RVA `0x159610`):

1. `FUN_101d43a0(type, &local_44)` — gets warehouse handle
2. `FUN_101d0300(local_44[0])` — gets/creates ProgId (`iVar1`)
3. **`FUN_10158fb0(entryRecord, param_4, iVar1)`** — fills the entry record:
   - **`strcpy_s((char*)(entryRecord + 0x17), 0x3D, param_4)`** — stores **verbatim** name in entry record at offset `+0x17` (max 0x3D = 61 bytes)
   - Sets various flags and default values
   - **No lowercasing is applied** — the name is stored verbatim
4. `FUN_101d4280(type, param_2)` — converts to power type
5. **`FUN_101cf990(powerType, param_3)`** — sends create request (command `0x16`) to server thread:
   - Server thread creates the CELProgram data node (via `FUN_101bfb00` constructor)
   - Server thread creates the BST entry (lowercase key + CELProgram* value)
   - Returns the CELProgram data node pointer
6. `FUN_101c2e90(dataNode)` — lazy-inits the data node's ProgId (does NOT copy the name)
7. **`FUN_101d3c00(powerType, progId, entryRecord)`** — stores the entry record in the warehouse (command `0x0E`):
   - Creates a `SafeArray` of 0x10A (266) shorts
   - Copies the entry record (including verbatim name at `+0x17`) into the SafeArray
   - Sends to server thread for persistence

### Key finding: The verbatim name flows as follows:

```
User input (verbatim)
  → entryRecord + 0x17 (strcpy_s, verbatim)     [FUN_10158fb0]
  → SafeArray → server thread → warehouse        [FUN_101d3c00, cmd 0x0E]
  → BST key (lowercase) + CELProgram* value      [server thread creates BST entry]
  → CELProgram + 0x1D (lazy fetch from server)   [FUN_101c36d0, cmd 0x34]
```

**The verbatim name is NOT copied into the CELProgram at creation time.** The CELProgram's `+0x1D` field is left as 0 (uninitialized). It is only populated when `FUN_101c36d0` is called, which fetches it from the server via command `0x34`.

**The entry record (`local_150` in `_CreateEntryEX`) is a stack-local buffer** — it does not persist beyond the function call. The verbatim name survives only in:
1. The warehouse (persistent storage, via command `0x0E`)
2. The server's BST entry key (but as lowercase only)
3. The CELProgram's `+0x1D` field (when lazily fetched via command `0x34`)

---

## 6. Name Normalization (`FUN_1002b700`, RVA `0x1002b700`)

**Signature:**
```cpp
undefined1* __thiscall FUN_1002b700(undefined1* outString, undefined4* inString);
```

**Behavior:**
- Creates a **new** string by applying `tolower` to each character of the input
- Does **NOT** modify the input string in-place
- The original (verbatim) name in the caller's buffer is preserved
- The output is a std::string with SSO (Small String Optimization, capacity 0xF)

**Conclusion:** The original name is always preserved by the caller. Normalization only creates a temporary lowercase copy for BST comparison purposes.

---

## 7. Recommended Approach for Getting the Verbatim Stored Name

### Option 1 (BEST): Call `FUN_101c36d0` directly on the BST-found data node

After `FindExistPos` (or the BST search) returns a ProgId, we can:
1. Re-search the BST to get the CELProgram data node pointer
2. Call `FUN_101c36d0(dataNode)` to get the verbatim name `char*`
3. Read the ANSI string at the returned pointer

**Pros:** Official TSDev mechanism, no source parsing needed, works for all items
**Cons:** Requires a server round-trip (command `0x34`) on first access for each item

### Option 2 (SIMPLEST): Call `FUN_101a42d0` — combined search + verbatim name

This function does BST search + `FUN_101c36d0` in one call, returning the verbatim name via an output pointer.

**Pros:** Single function call, handles all the lookup + lazy-fetch logic
**Cons:** Calling convention is `__fastcall` with stack args; need to match the exact ABI

### Option 3 (CURRENT): Parse `{ name }` from source code

The current implementation uses `getSource` + regex parsing of `{ name }` comment blocks.

**Pros:** No additional RE needed, already implemented
**Cons:**
- Violates the AGENTS.md rule: "Item source text, comments, naming conventions inside source code, etc. are ARBITRARY user-controlled content"
- Fails if the `{ name }` block is missing or doesn't match the stored name
- Requires fetching the full source code just to get the name

### Recommendation

**Switch to Option 1 or Option 2** for production use. The current source-parsing approach (Option 3) violates the project's own rules about not relying on user-controlled source content. `FUN_101c36d0` is the official, reliable mechanism that TSDev itself uses to display item names in its UI.

**Implementation note:** `FUN_101c36d0` is a `__thiscall` function taking the CELProgram data node in `ECX`. The data node is obtained from the BST search (inner node `+0x28`). The function returns a `char*` to the verbatim ANSI name. Remember to decrement the data node's refcount (`+0x210`) after use, following the pattern in `FindExistPos` / `FUN_101a42d0`.

---

## Appendix: Function Cross-Reference

| RVA | Address | Name | Role |
|-----|---------|------|------|
| `0x159610` | `10159610` | `_CreateEntryEX@24` | Creates warehouse entry, updates BST via server |
| `0x1a4230` | `101a4230` | `FindExistPos` | BST search, returns ProgId only |
| `0x1a42d0` | `101a42d0` | (unnamed) | BST search + verbatim name (via `FUN_101c36d0`) |
| `0x1c36d0` | `101c36d0` | (unnamed) | **Verbatim name getter** (lazy, server cmd 0x34) |
| `0x1c2e90` | `101c2e90` | (unnamed) | ProgId getter (lazy, server cmd 0x33) |
| `0x1c4990` | `101c4990` | (unnamed) | Type getter (lazy, server cmd 0x32) |
| `0x1e3db0` | `101e3db0` | (unnamed) | BST lookup (server mode), 2-level tree search |
| `0x1e30f0` | `101e30f0` | (unnamed) | Outer BST traversal (by ProgId) |
| `0x100269b0` | `100269b0` | (unnamed) | Inner BST traversal (by lowercase name) |
| `0x1e35a0` | `101e35a0` | (unnamed) | BST cache-fill insert (after server response) |
| `0x1e3180` | `101e3180` | (unnamed) | Inner BST insert/find (by lowercase name) |
| `0x1002b700` | `1002b700` | (unnamed) | Name normalization (tolower, creates copy) |
| `0x1bfb00` | `101bfb00` | (unnamed) | CELProgram constructor (0x241 bytes) |
| `0x1c4ae0` | `101c4ae0` | (unnamed) | CELProgram initializer (sets ProgId, type, flags) |
| `0x158fb0` | `10158fb0` | (unnamed) | Entry record filler (stores verbatim name at +0x17) |
| `0x1d05e0` | `101d05e0` | (unnamed) | BST search dispatcher (server vs local mode) |
| `0x1d04f0` | `101d04f0` | (unnamed) | BST search dispatcher (used by verbatim-name path) |
| `0x1d3c00` | `101d3c00` | (unnamed) | Warehouse store (sends entry record, cmd 0x0E) |
| `0x1cf990` | `101cf990` | (unnamed) | Warehouse create request (cmd 0x16) |
| `0x1cf5b0` | `101cf5b0` | (unnamed) | Server request sender (blocking, generic) |
