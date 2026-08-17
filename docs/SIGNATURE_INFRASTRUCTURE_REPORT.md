# Signature Management Infrastructure — Research & Architecture Report

| Field | Value |
|---|---|
| **Date** | 2026-08-17 |
| **Purpose** | Research professional patterns for managing binary signatures in RE-dependent projects, assess Ghidra headless capabilities, and propose a production-grade tooling architecture for keeping orlib20.dll signatures up to date with minimal manual effort. |
| **Scope** | Architecture proposal only — no code changes. Drives a future TVS for signature management infrastructure. |

> **STATUS (2026-08-17):** The recommendations in this report have been ACCEPTED. An architecture design document has been produced at `docs/ARCHITECTURE_REDESIGN.md` (~1250 lines, adversarially APPROVED after 3 review rounds). The 7 TVSs (TVS-E.1 through TVS-E.7) defined in §9 of the design document implement these recommendations. Execution has NOT yet begun — TVS-E.1 is the next TVS to execute. References to `b1_signatures.h` as the "current" approach below describe the pre-E state; the target state is JSON + codegen per the design document.

---

## Section 1: Professional Patterns Survey

### 1.1 SourceMod / Source Engine Gamedata System (Gold Standard for This Use Case)

SourceMod is the most directly relevant professional pattern. It hooks internal (non-exported) functions in Valve's Source Engine binaries across game updates, using a system called **"gamedata"** (game configuration files).

**Key design elements:**

- **Data-file-driven, not code-driven.** Signatures live in `.txt` files under `gamedata/`, not in C++ source. The format is a Valve KeyValues text format with per-platform signature strings. Example from the AlliedMods wiki ([source](https://wiki.alliedmods.net/SDKTools_(SourceMod_Development))):

```
"Games"
{
  "cstrike"
  {
    "Signatures"
    {
      "RoundRespawn"
      {
        "library" "server"
        "windows" "\x56\x8B\xF1\x8B\x06\xFF\x90*\x04\x00\x00\x8B\x86*\x0D\x00"
        "linux"   "@_ZN9CCSPlayer12RoundRespawnEv"
      }
    }
  }
}
```

- **`\x2A` (asterisk) as single-byte wildcard.** Directly analogous to the project's `??` notation. Wildcards cover address-dependent bytes (offsets, relative displacements).
- **Symbol fallback via `@` prefix.** On Linux, functions are exported symbols, so the gamedata file can use `@_ZN9CCSPlayer12RoundRespawnEv` instead of a byte pattern. This dual-mode (signature OR symbol) is a key resilience pattern.
- **Per-game-version specialization via CRC32.** Gamedata files can contain a `"CRC"` section that selects different signature sets based on the binary's CRC32, directly mapping binary versions to signature sets ([source](https://wiki.alliedmods.net/SDKTools_(SourceMod_Development))).
- **Inheritance via `#supported` sections.** A `#default` block with a `#supported` sub-block lists games/engines that inherit the same signatures, avoiding duplication.
- **Update workflow:** When a game updates, the community updates the gamedata `.txt` files — no recompilation of SourceMod itself needed. Plugins reference signatures by key name (e.g., `"RoundRespawn"`), decoupled from the actual byte pattern.

**Relevance to this project:** SourceMod's pattern is almost exactly the problem we face — locating internal functions by byte signature across binary updates, with per-version signature sets. The key takeaways are: (a) store signatures in data files, not headers; (b) key signatures by logical name; (c) support per-version signature sets; (d) allow symbol/export fallback where available.

**Tools built around this pattern:**
- **`makesig.py`** ([nosoop/ghidra_scripts](https://github.com/nosoop/ghidra_scripts/blob/master/makesig.py)) — A Ghidra Python script that generates SourceMod-ready signatures with automatic operand wildcarding. This is the single most relevant existing tool: it uses Ghidra's `InstructionPrototype.getOperandValueMask()` to determine which bytes belong to address/dynamic operands and automatically wildcards them.
- **`makesig7.idc`** — The IDA equivalent, widely used in the SourceMod community.
- **IDA-GameDataTracker** ([K4ryuu/IDA-GameDataTracker](https://github.com/K4ryuu/IDA-GameDataTracker)) — An IDA Pro plugin that "tracks game functions across binary updates — relocate signatures, vtable offsets, and mid-function patterns automatically." Built specifically for Source Engine modding.

### 1.2 IDA SigMaker / Sigdrift (Cross-Binary Signature Verification)

**SigMaker** ([mahmoudimus/ida-sigmaker](https://github.com/mahmoudimus/ida-sigmaker)) is an IDA Pro 9.0+ plugin for generating unique binary pattern signatures. Key features relevant to us:

- **Processor-aware operand wildcarding** for x86, x64, ARM, MIPS — automatically wildcards operand bytes (addresses, displacements) while keeping opcode bytes fixed.
- **Nibble-level wildcarding** — can wildcard individual nibbles, not just full bytes, for finer-grained patterns.
- **Batch search** — "Signatures are usually maintained as a collection. After a binary update, you need to know which patterns still match, which became ambiguous, and which no longer match. Batch search accepts the collection once and returns one result per pattern in input order." ([PyPI](https://pypi.org/project/sigmaker/))
- **Uniqueness verification** built into generation — the signature is only accepted if it matches exactly once in the binary.
- **XRef-based signature generation** — can generate signatures from cross-references (data refs, code refs), finding the shortest unique pattern.

**Sigdrift** ([koyzdev/sigdrift](https://github.com/koyzdev/sigdrift)) takes this further with **cross-binary verification**:

- Downloads multiple builds of the same Windows binary from Winbindex, generates signatures via IDA, and produces **minimal, cross-binary-verified signature sets**.
- For each function, finds byte patterns that uniquely identify it **across all builds** — not just one build.
- Supports four selection strategies: `greedy` (set-cover, no FP guarantee), `cover` (deploy-safe with false-positive guarantees), etc.
- Emits C headers directly.
- **Offset signatures** — for struct member offsets, finds byte patterns whose displacement slot encodes the member's runtime offset, PDB-grounded and cross-binary verified.

**Relevance:** Sigdrift's cross-binary verification is the gold standard for signature resilience. If we ever obtain multiple versions of orlib20.dll, running a cross-binary verification pass would produce signatures that are maximally stable across versions. Even with a single version, the SigMaker-style automatic operand wildcarding is directly applicable.

### 1.3 REFramework / kananlib (Runtime Pattern Scanning + Composition)

**kananlib** ([cursey/kananlib](https://github.com/cursey/kananlib)) is a C++ binary analysis library built for REFramework and UEVR — runtime modding projects that discover hook targets in stripped game binaries at injection time.

**Key patterns:**

- **Multi-pattern fallback arrays.** For each target function, REFramework defines an array of candidate patterns and tries them in order. Example from `REManagedObject.cpp` ([source](https://github.com/praydog/REFramework/blob/af1495a5/shared/sdk/REManagedObject.cpp)):

```cpp
constexpr std::array<std::string_view, 4> possible_patterns{
    "40 ? 48 83 EC ? 8B 41 ? 48 8B ? 85 C0 0F ? ? ? ? ? 0F ? ? 0E", // RE2+
    "40 ? 48 83 EC ? 8B 41 ? 48 8B ? 85 C0 0F ? ? ? ? ? 80 ? 0E 00", // TDB73+/DD2+
    "48 89 ? ? ? 57 48 83 EC ? 0F ? ? 0E", // RE7 TDB49
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC ? 48 89 CE 8B 41 08 85 C0", // MHWILDS+
};
```

- **String-reference-based function discovery.** `find_function_with_string_reference` locates functions by finding string literals in `.rdata`, then walking displacement references back to the enclosing function. This is an alternative to pure byte-pattern scanning and is more resilient to code shifts.
- **Composable primitives.** `find_function_with_string_refs(A, B)` filters to functions referencing both string A and string B. `find_landmark_sequence` chains patterns across the CFG.
- **CI verification pattern.** kananlib explicitly documents: "CI verification that signatures still resolve uniquely after a game patch (`find_pattern --all`, fail the build if `> 1`)." This is the exact verification gate we need.
- **CLI for iteration.** `kananlib-cli` exposes scanners as one-shot commands: "type a pattern, get an RVA, sub-second on multi-megabyte binaries." This enables rapid iteration during signature updates.

**Relevance:** The multi-pattern fallback array pattern is directly applicable — for each of our 12 targets, we could maintain a list of candidate signatures (current + alternatives), trying them in order. The CI verification pattern (`find_pattern --all`, fail if >1 match) should be our regression test gate.

### 1.4 Cheat Engine AOB Community Practices

The Cheat Engine community has developed mature practices for AOB (Array of Bytes) signature management over many years ([sources](https://www.cheatengine.org/forum/viewtopic.php?t=621624), [CE Wiki](https://wiki.cheatengine.org/index.php?title=Tutorials%3AAOBs)):

- **Wildcard all potentially-dynamic bytes.** "Use wildcard entries for all potentially-dynamic bytes in your AOB signature (there are tools/plugins and even custom templates that can automatically create large signatures with wildcard entries already included)."
- **Include extended context.** "Use a template that provides a large block of original code (commented-out), or make a habit of including an extended AOB signature (again, commented-out), so that you have something to work with in case your original signature fails."
- **`assert` for pre-patch validation.** CE's auto assembler uses `assert(injStep8WrtHook, step8WrtBytes)` to verify the bytes at the injection point match expectations before patching — directly analogous to our `validatePatchSite` logic.
- **Update workflow:** "If the script included the original code (usually near the bottom): then it's usually not too hard to find the original injection point... Start scanning for bytes around the injection point and gradually get more generic with the search pattern until something is found. Replace offsets with wildcards first, then start removing operand information, check different instruction sizes."
- **Keep instruction context in comments.** The CE community standard is to include the disassembly context around each AOB in comments, so when a signature breaks, the updater has surrounding code to search for.

**Relevance:** The "include extended context" and "keep disassembly in comments" practices are directly applicable to our signature catalog. The `assert` pattern maps to our existing `validatePatchSite` — we should keep and strengthen it.

### 1.5 YARA Rule Management (Metadata & Versioning Practices)

YARA is the malware-analysis industry standard for byte-pattern signature rules. While YARA's use case (file identification, not function location) differs from ours, its **rule management practices** are directly relevant:

- **Mandatory metadata fields** ([YARA Style Guide](https://neo23x0.github.io/YARA-Style-Guide/)): `description`, `author`, `date`, `reference` — every rule must carry provenance metadata.
- **Structured naming convention:** `RuleName = Category_Description_Detail` — e.g., `MA_Excel4Macro_AutoOpen`. Categories are embedded in the rule name for straightforward identification.
- **Metadata standards enforcement** ([YARA-X Language Server](https://virustotal.github.io/yara-x/blog/enforcing-yara-metadata-standards/)): Teams define `metadataValidation` configs in VS Code that enforce required metadata fields and types at authoring time — automated consistency checking.
- **Versioned rule repositories** ([Creating a YARA Repository](https://brkalbyrk.github.io/posts/Creating-a-YARA-Repository/)): Rules are managed in a dedicated repository with CI testing against sample corpora. Each rule is tested against known-positive and known-negative samples.
- **Rule structure:** `meta:` section (provenance), `strings:` section (the byte patterns), `condition:` section (matching logic). This three-part structure (metadata + pattern + logic) is a clean separation.

**Relevance:** YARA's metadata discipline (every signature carries provenance: author, date, reference, description) should be adopted in our signature catalog. The CI-testing-against-corpus pattern maps to our regression test suite.

### 1.6 Ghidra Function ID (FidDB) — Masked Byte Hashing

Ghidra itself has a built-in function identification system based on **masked instruction bytes** ([blog by Joren Vrancken](https://blog.nietaanraken.nl/posts/ghidra-function-id/)):

- Ghidra computes two hashes per function: a **full hash** (instruction bytes with operand bytes masked out + register operands) and a **specific hash** (adds immediate values and address operands).
- The masking is done via SLEIGH specifications — `getOperandValueMask(operandIndex)` returns a bit mask describing which bits of the instruction encode the operand value. Masking those bits to zero leaves only the opcode/structural bits.
- Example: `89 4d 10` (`mov dword ptr [rbp + 0x10], ecx`) and `89 46 25` (`mov dword ptr [rsi + 0x25], eax`) both mask to `89 40 00` — same opcode, different operands.

**Relevance:** This is exactly the wildcard automation mechanism we need. Ghidra's `InstructionPrototype.getOperandValueMask()` API can determine which bytes are operand bytes (addresses, displacements, immediates) and which are opcode bytes. A Ghidra script can automatically wildcard operand bytes while keeping opcode bytes fixed — this is the core of the `makesig.py` script (§1.1) and the Sigga script (§2.3).

### 1.7 Rizzo (Cross-Binary Function Matching)

**Rizzo** ([dynacylabs/ghidra-rizzo](https://github.com/dynacylabs/ghidra-rizzo)) is a Ghidra port of the IDA Rizzo plugin. It generates multiple signature types per function:

| Signature Type | Description |
|---|---|
| **Formal** | Exact hash of instruction mnemonics and operands |
| **Fuzzy** | Hash that abstracts away addresses — useful across builds |
| **String** | Based on string literals referenced by the function |
| **Immediate** | Based on immediate (constant) values used in the function |
| **Function** | Based on the names of functions called by the function |

Signatures are saved to `.riz` files and applied to other binaries to automatically rename `FUN_XXXXXXXX` placeholders.

**Relevance:** The multi-signature-type approach (fuzzy hash + string refs + immediate constants) provides additional resilience beyond pure byte patterns. If a byte signature fails, a string-reference or fuzzy-hash match could locate the function. This is a potential future enhancement.

### 1.8 Summary of Professional Patterns

| Pattern | Source | Key Takeaway for This Project |
|---|---|---|
| Data-file-driven signatures | SourceMod gamedata | Store signatures in data files (JSON/TOML), not C++ headers; key by logical name |
| Per-version signature sets | SourceMod CRC sections, sigdrift | Map DLL versions to signature sets via a version manifest |
| Automatic operand wildcarding | makesig.py, SigMaker, Sigga | Use Ghidra's `getOperandValueMask()` to auto-wildcard address/dynamic operand bytes |
| Cross-binary verification | sigdrift | Verify signatures against multiple builds for maximum stability |
| Multi-pattern fallback arrays | REFramework | Try multiple candidate patterns per target, in priority order |
| CI uniqueness verification | kananlib | `find_pattern --all`, fail build if >1 match — regression test gate |
| Pre-patch byte assertion | Cheat Engine `assert` | Validate original bytes before patching (already implemented as `validatePatchSite`) |
| Mandatory provenance metadata | YARA Style Guide | Every signature carries author, date, reference, description |
| Extended context in comments | CE community | Include surrounding disassembly for manual fallback |
| Multi-strategy function location | Rizzo, kananlib | Byte pattern + string refs + fuzzy hash as complementary strategies |

---

## Section 2: Ghidra Headless Capabilities Assessment

### 2.1 Can `analyzeHeadless` Run Analysis Scripts Automatically?

**Yes.** Ghidra's `analyzeHeadless` mode supports `-preScript` and `-postScript` options that run Ghidra scripts (Python or Java) before/after auto-analysis. From the [official Ghidra docs](https://www.ghidradocs.com/11.0.1_PUBLIC/docs/GhidraClass/Intermediate/HeadlessAnalyzer.html):

```
analyzeHeadless <project_location> <project_name> \
    -import <binary> \
    -postScript MyScript.py [args...] \
    -scriptPath <path1>;<path2> \
    [-deleteProject] \
    [-noanalysis]
```

Key capabilities:
- **`-postScript`**: Runs a script after auto-analysis completes. Can be repeated for multiple scripts. Arguments can be passed to the script.
- **`-preScript`**: Runs before analysis (rarely needed for our use case).
- **`-scriptPath`**: Additional script search directories.
- **`-deleteProject`**: Deletes the temporary project after the run — ideal for one-shot analysis.
- **`-noanalysis`**: Skip auto-analysis (useful if the binary was already analyzed and cached).
- **`-analysisTimeoutPerFile`**: Set a timeout per file.

Scripts can be **Python (Jython/PyGhidra)** or **Java**. Python scripts use `#@runtime PyGhidra` directive and have full access to the Ghidra Java API.

### 2.2 Can Ghidra Scripts Locate Functions by Various Criteria?

**Yes, all of the following are supported via the Ghidra Java API:**

| Criterion | API Method | Notes |
|---|---|---|
| **Export name** | `currentProgram.getSymbolTable().getSymbols(name)` or `getExternalEntryPoint()` | Finds exported functions by name |
| **Function at address** | `currentProgram.getFunctionManager().getFunctionContaining(addr)` | Gets function containing a given address |
| **All functions** | `currentProgram.getFunctionManager().getFunctions(True)` | Iterates all defined functions |
| **String references** | `currentProgram.getListing().getStrings()` → `ref.getReferencesTo()` | Find strings, then find functions referencing them |
| **Cross-references** | `currentProgram.getReferenceManager().getReferencesTo(addr)` | Find all xrefs to an address |
| **Calling convention** | `function.getCallingConventionName()` | Filter by calling convention |
| **Function signature** | `function.getSignature()` | Get parameter types, return type |
| **Instruction bytes** | `instruction.getBytes()` | Get raw bytes of any instruction |
| **Operand types** | `instruction.getOperandType(opIndex)` | Check if operand is ADDRESS, DYNAMIC, REGISTER, etc. |
| **Operand value mask** | `instruction.getPrototype().getOperandValueMask(opIndex)` | Get bit mask for operand bytes — **the key API for automatic wildcarding** |

### 2.3 Can Scripts Extract Raw Bytes of Function Prologues?

**Yes.** The `Instruction.getBytes()` method returns the raw bytes of any instruction. To extract a function's prologue bytes:

```python
fm = currentProgram.getFunctionManager()
fn = fm.getFunctionAt(addr)
cm = currentProgram.getCodeManager()
ins = cm.getInstructionAt(fn.getEntryPoint())
bytes_list = []
while ins and len(bytes_list) < MAX_SIG_LEN:
    for b in ins.getBytes():
        bytes_list.append(b & 0xFF)
    ins = ins.getNext()
```

### 2.4 Can Scripts Output Structured Data (JSON)?

**Yes.** Ghidra scripts run in a full JVM (or Jython/PyGhidra environment) with access to standard I/O and file APIs. Multiple existing projects demonstrate JSON output from headless Ghidra:

- **JsonBlocks.java** ([ckane/CkaneGhidraScripts](https://github.com/ckane/CkaneGhidraScripts)) — "dumps one program's functions & basic blocks, with instructions and Pcode, into a JSON file named `output.json`."
- **openrelik-worker-ghidra** ([dig-sec/openrelik-worker-ghidra](https://github.com/dig-sec/openrelik-worker-ghidra)) — Produces `summary.json`, `functions.jsonl`, `strings.json`, `decompile.jsonl` from headless Ghidra analysis.
- **rbinghidra** ([kirkderp/rbinghidra](https://github.com/kirkderp/rbinghidra)) — MCP server that wraps `analyzeHeadless` with Java post-scripts, outputting JSON between `MCP_GHIDRA_JSON_START` / `MCP_GHIDRA_JSON_END` markers.
- **agent-stuff Ghidra skill** ([mitsuhiko/agent-stuff](https://github.com/mitsuhiko/agent-stuff/blob/HEAD/skills/ghidra/SKILL.md)) — `ExportFunctions.java` outputs `{name}_functions.json` with addresses, signatures, parameters, and call relationships.

A Ghidra Python script can use Python's `json` module (via PyGhidra) or Java's `JSONObject` to write structured output.

### 2.5 What Would a Headless Signature Generator Script Look Like?

Here is a concrete sketch of a Ghidra headless Python script (`GenerateSignatures.py`) that would: load orlib20.dll, locate the 12 target functions (by their known RVAs in the baseline, or by anchor strings/xrefs), extract prologue bytes with automatic operand wildcarding, verify uniqueness, and output a candidate signature JSON file.

```python
# GenerateSignatures.py
# Ghidra headless script: generates candidate byte signatures for orlib20.dll targets.
#@runtime PyGhidra
#@category TSDev

import json
import collections
from ghidra.program.model.lang import OperandType

# Target definitions: logical name → anchor strategy
# For the baseline DLL, we know the RVAs. For a new version, we'd use
# string/xref anchors to relocate the functions.
TARGETS = [
    {"name": "StartCompilingProcess", "rva": 0x165DE0, "maxLen": 64},
    {"name": "EndCompilingProcess", "rva": 0x165ED0, "maxLen": 55},
    {"name": "CELCompiler_ctor", "rva": 0x165A00, "maxLen": 64},
    {"name": "CELCompiler_dtor", "rva": 0x165AC0, "maxLen": 38},
    {"name": "InitGlobalLookup", "rva": 0x1A9460, "maxLen": 66},
    {"name": "DeleteFromTree", "rva": 0x1E4030, "maxLen": 66},
    {"name": "GetWarehouse", "rva": 0x1A9650, "maxLen": 45},
    {"name": "FUN_101a8360_delete_orchestrator", "rva": 0x1A8360, "maxLen": 64},
    {"name": "FUN_10011230_persistence_patch", "rva": 0x11230, "maxLen": 72,
     "isPatchSite": True, "patchOffset": 0,
     "originalBytes": "55 8B EC 6A FF 68 CD D3",
     "replacementBytes": "B8 01 00 00 00 C2 0C 00"},
    {"name": "FUN_101d0950_thread_check_INT3", "rva": 0x1D0950, "maxLen": 36,
     "isPatchSite": True, "patchOffset": 0x43,
     "originalBytes": "CC", "replacementBytes": "90"},
    {"name": "FUN_101cf5b0_thread_check_INT3", "rva": 0x1CF5B0, "maxLen": 37,
     "isPatchSite": True, "patchOffset": 0x24,
     "originalBytes": "CC", "replacementBytes": "90"},
    {"name": "FUN_101d1fa0_thread_check_INT3", "rva": 0x1D1FA0, "maxLen": 57,
     "isPatchSite": True, "patchOffset": 0x28,
     "originalBytes": "CC", "replacementBytes": "90"},
]

BytePattern = collections.namedtuple('BytePattern', ['is_wildcard', 'byte'])

def should_mask_operand(ins, op_index):
    """Returns True if the operand should be wildcarded (address/dynamic bytes)."""
    op_type = ins.getOperandType(op_index)
    # DYNAMIC = relative addresses (rel32 calls/jumps)
    # ADDRESS  = absolute addresses (IAT calls, data refs, SEH handlers)
    return bool(op_type & OperandType.DYNAMIC) or bool(op_type & OperandType.ADDRESS)

def get_masked_instruction(ins):
    """Returns a list of BytePattern for one instruction, with address/dynamic
    operand bytes automatically wildcarded."""
    mask = [0] * ins.length
    proto = ins.getPrototype()
    for op in range(proto.getNumOperands()):
        if should_mask_operand(ins, op):
            value_mask = proto.getOperandValueMask(op)
            mask_bytes = value_mask.getBytes()
            mask = [m | (v & 0xFF) for m, v in zip(mask, mask_bytes)]
    result = []
    for m, b in zip(mask, ins.getBytes()):
        if m == 0xFF:
            result.append(BytePattern(is_wildcard=True, byte=None))
        else:
            result.append(BytePattern(byte=b & 0xFF, is_wildcard=False))
    return result

def cleanup_trailing_wilds(pattern):
    """Remove trailing wildcard bytes (pointless at end of signature)."""
    while pattern and pattern[-1].is_wildcard:
        del pattern[-1]

def generate_signature(fn, max_len):
    """Generate a candidate signature for a function, up to max_len bytes."""
    cm = currentProgram.getCodeManager()
    ins = cm.getInstructionAt(fn.getEntryPoint())
    pattern = []
    while ins and len(pattern) < max_len:
        pattern.extend(get_masked_instruction(ins))
        ins = ins.getNext()
    cleanup_trailing_wilds(pattern)
    return pattern

def pattern_to_hex(pattern):
    """Convert BytePattern list to IDA-style hex string with ?? wildcards."""
    parts = []
    for bp in pattern:
        if bp.is_wildcard:
            parts.append("??")
        else:
            parts.append("{:02X}".format(bp.byte))
    return " ".join(parts)

def count_matches(pattern, text_bytes, text_rva):
    """Count how many times the pattern matches in the .text section."""
    matches = []
    sig_len = len(pattern)
    for i in range(len(text_bytes) - sig_len):
        ok = True
        for j, bp in enumerate(pattern):
            if not bp.is_wildcard and text_bytes[i + j] != bp.byte:
                ok = False
                break
        if ok:
            matches.append(text_rva + i)
    return matches

def get_text_section_bytes():
    """Get the .text section raw bytes and RVA."""
    memory = currentProgram.getMemory()
    for block in memory.getBlocks():
        if block.getName() == ".text":
            size = block.getSize()
            buf = bytearray(size)
            block.getBytes(block.getStart(), buf, 0, size)
            rva = block.getStart().getOffset() - currentProgram.getImageBase().getOffset()
            return list(buf), rva
    return None, None

# --- Main ---
output = {"signatures": [], "metadata": {}}
text_bytes, text_rva = get_text_section_bytes()

fm = currentProgram.getFunctionManager()
image_base = currentProgram.getImageBase().getOffset()

for target in TARGETS:
    addr = image_base + target["rva"]
    fn = fm.getFunctionAt(addr)
    if fn is None:
        fn = fm.getFunctionContaining(addr)

    if fn is None:
        output["signatures"].append({
            "name": target["name"],
            "status": "NOT_FOUND",
            "error": "No function at RVA 0x{:X}".format(target["rva"])
        })
        continue

    pattern = generate_signature(fn, target["maxLen"])
    hex_sig = pattern_to_hex(pattern)

    # Verify uniqueness
    matches = count_matches(pattern, text_bytes, text_rva)

    entry = {
        "name": target["name"],
        "status": "OK" if len(matches) == 1 else "AMBIGUOUS" if len(matches) > 1 else "NO_MATCH",
        "rva": target["rva"],
        "actualRva": fn.getEntryPoint().getOffset() - image_base,
        "hexPattern": hex_sig,
        "length": len(pattern),
        "matchCount": len(matches),
        "matchRvas": ["0x{:X}".format(m) for m in matches[:10]],
        "isPatchSite": target.get("isPatchSite", False),
    }
    if target.get("isPatchSite"):
        entry["patchOffset"] = target["patchOffset"]
        entry["originalBytes"] = target["originalBytes"]
        entry["replacementBytes"] = target["replacementBytes"]
    output["signatures"].append(entry)

# Write JSON output
output_path = getScriptArgs()[0] if len(getScriptArgs()) > 0 else "candidate_signatures.json"
with open(output_path, "w") as f:
    json.dump(output, f, indent=2)
print("Wrote {} signatures to {}".format(len(output["signatures"]), output_path))
```

**Invocation:**
```bash
analyzeHeadless /tmp/ghidra_proj TSDevSigGen \
    -import orlib20_new.dll \
    -postScript GenerateSignatures.py /tmp/candidate_signatures.json \
    -scriptPath /path/to/scripts \
    -deleteProject
```

### 2.6 Ghidra Headless Limitations for This Use Case

| Limitation | Impact | Mitigation |
|---|---|---|
| **Auto-analysis may not identify all internal functions** — Ghidra's function discovery is heuristic and may miss functions without standard prologues | Some of our 12 targets may not be defined as functions after auto-analysis | Use `-postScript` to run `FindUndefinedFunctionsScript` or manually create functions at known RVAs (for baseline) or at anchor-derived addresses (for new versions) |
| **Analysis time** — full auto-analysis of a 4.9 MB DLL takes minutes | Slows the update workflow | Cache the analyzed project (don't use `-deleteProject` for repeated runs); use `-noanalysis` for re-runs against a cached project |
| **Jython vs PyGhidra** — older Ghidra uses Jython (Python 2 syntax); newer uses PyGhidra (Python 3) | Script compatibility | Use `#@runtime PyGhidra` directive; test against the installed Ghidra version |
| **No built-in uniqueness scanner** — Ghidra doesn't have a "scan this pattern and count matches" API | Must implement pattern scanning in the script | Implement a simple byte-pattern scanner in the script (as shown above), or use the project's existing C++ scanner for post-verification |
| **RVA-based lookup only works for the baseline** — for a new DLL version, RVAs will have shifted | Can't use known RVAs to locate functions in a new version | Use anchor strategies: string references, xref patterns, export-name-relative offsets, or fuzzy matching (Rizzo) to relocate functions |

### 2.7 Assessment Summary

Ghidra headless is **fully capable** of:
1. ✅ Running analysis scripts automatically (`-postScript`)
2. ✅ Locating functions by export name, address, xrefs, string references, calling convention
3. ✅ Extracting raw bytes of function prologues (`Instruction.getBytes()`)
4. ✅ Automatically determining which bytes to wildcard (`getOperandValueMask()` + `OperandType.DYNAMIC`/`ADDRESS`)
5. ✅ Outputting structured JSON with function locations, bytes, and metadata
6. ✅ Verifying signature uniqueness by scanning the .text section

The main gap is **function relocation in new DLL versions** — RVA-based lookup only works for the baseline. For new versions, the script needs anchor strategies (string refs, xref patterns, export-relative offsets) to find the shifted functions. This is the primary human-in-the-loop step (see §3).

---

## Section 3: Proposed Update Workflow

### 3.1 Ideal Workflow: From "New DLL Received" to "Updated Signatures Verified"

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    SIGNATURE UPDATE WORKFLOW                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  1. RECEIVE new orlib20.dll                                              │
│     └─ Copy to tmp/re_artifacts/orlib20_v<version>.dll                  │
│     └─ Compute SHA-256, PE timestamp, FileVersion, file size             │
│     [AUTOMATED]                                                          │
│                                                                          │
│  2. RUN EXISTING SIGNATURES against new DLL                              │
│     └─ sigtool verify --dll orlib20_v<version>.dll                      │
│     └─ Scans all 12 existing signatures in .text                         │
│     └─ Reports per-target: match count, match RVA, pass/fail             │
│     [AUTOMATED]                                                          │
│                                                                          │
│  3. DECISION GATE: Did all 12 signatures resolve?                       │
│     ├─ YES (all 12 unique match) → Go to step 4                         │
│     └─ NO (some failed) → Go to step 5                                  │
│     [AUTOMATED]                                                          │
│                                                                          │
│  4. ALL SIGNATURES MATCHED                                               │
│     └─ Verify patch sites contain expected original bytes                │
│     └─ Run functional regression tests (insert/delete/modify)            │
│     └─ If all pass: add new version to manifest, DONE                    │
│     └─ If patch bytes differ: update patch metadata, go to step 5b      │
│     [AUTOMATED except manifest update]                                   │
│                                                                          │
│  5. SOME SIGNATURES FAILED — REGENERATION NEEDED                         │
│     └─ Run Ghidra headless analyzer on new DLL                           │
│     └─ Run GenerateSignatures.py (headless post-script)                 │
│     └─ Script auto-wildcards operand bytes, outputs candidate JSON       │
│     [AUTOMATED]                                                          │
│                                                                          │
│  5a. IF Ghidra auto-analysis found the functions (by RVA or anchor):     │
│     └─ Review candidate signatures in JSON output                        │
│     └─ Check uniqueness: all matchCount == 1?                            │
│     └─ If ambiguous: extend signature (add more bytes) — MANUAL          │
│     └─ If no-match: adjust wildcarding — MANUAL                          │
│     [HUMAN IN THE LOOP: review + adjust]                                 │
│                                                                          │
│  5b. IF Ghidra could NOT find the functions (RVA shifted, no anchor):    │
│     └─ Open Ghidra GUI on new DLL                                        │
│     └─ Use string references, xref patterns, export-relative offsets     │
│        to manually locate the shifted functions                           │
│     └─ Run makesig.py / Sigga on each located function                   │
│     └─ Verify uniqueness in Ghidra                                       │
│     [HUMAN IN THE LOOP: manual RE — the multi-hour step]                 │
│                                                                          │
│  6. VERIFY CANDIDATE SIGNATURES                                          │
│     └─ sigtool verify --dll orlib20_v<version>.dll                       │
│        --signatures candidate_signatures.json                            │
│     └─ All 12 must match uniquely                                       │
│     └─ Patch sites must contain expected original bytes                  │
│     [AUTOMATED]                                                          │
│                                                                          │
│  7. FUNCTIONAL REGRESSION TEST                                           │
│     └─ Run test_insert, test_delete, test_modify against new DLL         │
│     └─ All must pass (exit 0)                                            │
│     [AUTOMATED]                                                          │
│                                                                          │
│  8. INTEGRATE                                                             │
│     └─ Add new signature set to signatures/ directory                    │
│     └─ Add new version entry to version manifest                         │
│     └─ Update docs/SIGNATURE_CATALOG.md with new version section         │
│     └─ Commit with message: "sig: add orlib20 v<version> signature set"  │
│     [SEMI-AUTOMATED: file generation automated, commit is manual]        │
│                                                                          │
│  9. CI GATE (if CI exists)                                               │
│     └─ sigtool verify runs against all known DLL versions                │
│     └─ All signature sets must resolve uniquely                          │
│     └─ Build fails if any signature is ambiguous or missing              │
│     [AUTOMATED]                                                          │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Automation vs. Human-in-the-Loop Breakdown

| Step | Automated? | Human Time | Notes |
|---|---|---|---|
| 1. Receive DLL + compute indicators | ✅ Fully automated | 0 | Script computes SHA-256, PE timestamp, FileVersion |
| 2. Run existing signatures | ✅ Fully automated | 0 | `sigtool verify` — the key "try first" step |
| 3. Decision gate | ✅ Fully automated | 0 | Script checks if all 12 passed |
| 4. All matched → verify + test | ✅ Fully automated | 0 | Regression tests run automatically |
| 4. Manifest update | ⚠️ Semi-automated | ~2 min | Script generates manifest entry JSON; human pastes into manifest file |
| 5. Ghidra headless analysis | ✅ Fully automated | 0 | `analyzeHeadless` + `GenerateSignatures.py` |
| 5a. Review candidate sigs | ⚠️ Human review | ~15-30 min | Review JSON output, check uniqueness, adjust ambiguous sigs |
| 5b. Manual RE (worst case) | ❌ Manual | 1-4 hours | Only if Ghidra can't auto-locate functions; use GUI + string/xref anchors |
| 6. Verify candidate sigs | ✅ Fully automated | 0 | `sigtool verify` with new JSON |
| 7. Functional regression | ✅ Fully automated | 0 | Existing test suite |
| 8. Integrate | ⚠️ Semi-automated | ~5 min | Files generated; human commits |
| 9. CI gate | ✅ Fully automated | 0 | Runs on push |

### 3.3 Key Insight: The "Try Existing First" Pattern

The most important workflow optimization is **Step 2: run existing signatures against the new DLL first**. In the common case (minor rebuild, same compiler, small code changes), the existing signatures will still match. This turns a multi-hour manual RE effort into a 5-second automated check + a 2-minute manifest update.

This pattern is used by:
- **kananlib** — "Scripting around game updates: run the same query against three game versions in a shell loop."
- **Cheat Engine community** — "If you are already making use of AOB signatures, but they still fail, you'll have to fix them manually." (Implies: try first, fix only on failure.)
- **SourceMod** — gamedata files are tried as-is on new game versions; only updated when they break.

### 3.4 Verification Gates

Three verification gates should exist, in order:

1. **Uniqueness gate** (automated): Every signature must match exactly once in `.text`. Zero matches = signature broken; >1 match = signature ambiguous. This is the `find_pattern --all, fail if >1` pattern from kananlib.

2. **Patch-site validation gate** (automated): For the 4 patch sites, the bytes at `sigMatchRva + patchOffset` must match the expected original bytes (with wildcard mask). This catches the case where a signature matched a different function that happens to share the prologue. Already implemented as `validatePatchSite` in `orlib_resolver.h`.

3. **Functional regression gate** (automated): The existing test suite (`test_insert`, `test_delete`, `test_modify`) must pass against the new DLL. This is the ultimate end-to-end verification — even if signatures resolve and patch bytes match, a functional test failure indicates something deeper changed.

---

## Section 4: Proposed Tooling Architecture

### 4.1 Signature Data Format: JSON (Recommended)

**Recommendation: JSON for signature data, with C++ code generation.**

Rationale:
- **JSON is the industry standard** for structured signature data. Ghidra headless scripts naturally output JSON. The `openrelik-worker-ghidra` project outputs `functions.jsonl`, `strings.json`, etc. The `rbinghidra` MCP server outputs JSON.
- **JSON is human-readable and diff-friendly.** Unlike the current C++ header (`b1_signatures.h`), JSON signatures can be reviewed in a diff, and version-to-version changes are visible.
- **JSON separates data from code.** The current approach embeds signatures in C++ struct initializers, coupling signature data to the build. JSON decouples them — signatures can be updated without recompiling the scanner.
- **JSON supports metadata.** Each signature can carry provenance (author, date, Ghidra script version, reference), matching the YARA Style Guide's mandatory metadata pattern.
- **TOML is a viable alternative** (more human-friendly for manual editing), but JSON has better tooling support (every language has a JSON parser; Ghidra scripts output JSON natively).

**Not recommended:**
- **C++ header as primary format** (current approach) — couples data to build, not diff-friendly, no metadata.
- **YARA rules** — YARA's syntax is designed for file-level pattern matching, not function-level signature catalogs. YARA rules don't naturally express patch-site metadata, calling conventions, or per-version sets.
- **Custom DSL** — adds maintenance burden with no benefit over JSON.

**Proposed JSON schema:**

```json
{
  "schemaVersion": 1,
  "dll": {
    "fileName": "orlib20.dll",
    "fileVersion": "9.50.01.3344",
    "sha256": "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
    "peTimestamp": "0x5C01735E",
    "fileSize": 4924400,
    "textRva": "0x1000",
    "textVsize": "0x1F72FC",
    "textRawSize": "0x1F7400"
  },
  "signatures": [
    {
      "name": "StartCompilingProcess",
      "type": "call_target",
      "callingConvention": "cdecl",
      "expectedRva": "0x165DE0",
      "hexPattern": "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 8B 0D ?? ?? ?? ?? 33 C0 A3 ?? ?? ?? ?? 85 C9 74 15 51 FF 15 ?? ?? ?? ?? A3 ?? ?? ?? ?? 85 C0 75 19 A1 ?? ?? ?? ?? 69",
      "length": 64,
      "expectedMatchCount": 1,
      "provenance": {
        "author": "TVS-B.1",
        "date": "2026-08-17",
        "source": "Ghidra disassembly",
        "reference": "docs/SIGNATURE_CATALOG.md §4.1",
        "notes": "Distinctive: 51 8B 0D (push ecx; mov ecx,[imm32]) + IMUL 0x129 stride"
      },
      "disassemblyContext": [
        "PUSH EBP",
        "MOV EBP, ESP",
        "PUSH -1",
        "PUSH 0x101EC887       ; SEH handler (wildcarded)",
        "MOV EAX, FS:[0]",
        "PUSH EAX",
        "MOV FS:[0], ESP",
        "PUSH ECX",
        "MOV ECX, [0x1023cea0] ; error array ptr (wildcarded)",
        "..."
      ]
    },
    {
      "name": "FUN_10011230_persistence_patch",
      "type": "patch_site",
      "callingConvention": "fastcall",
      "expectedRva": "0x11230",
      "hexPattern": "55 8B EC 6A FF 68 ?? ?? ?? ?? ...",
      "length": 72,
      "expectedMatchCount": 1,
      "patch": {
        "offset": 0,
        "originalBytes": "55 8B EC 6A FF 68 CD D3",
        "replacementBytes": "B8 01 00 00 00 C2 0C 00",
        "originalBytesMask": "FF FF FF FF FF FF 00 00",
        "description": "Patch FUN_10011230 to MOV EAX,1; RET 0x0C — bypasses XmlDocument.Save crash"
      },
      "provenance": {
        "author": "TVS-2.2",
        "date": "2026-08-17",
        "source": "Ghidra + Frida validation",
        "reference": "docs/SIGNATURE_CATALOG.md §4.9"
      }
    }
  ],
  "dataGlobals": [
    {
      "name": "errorArrayPtr",
      "rva": "0x23cea0",
      "extractionMethod": "instruction_operand",
      "sourceSignature": "StartCompilingProcess",
      "sourceInstructionOffset": 25,
      "notes": "DAT_1023cea0 — global error-array pointer"
    }
  ]
}
```

### 4.2 Tools & Scripts

Five tools should exist:

| Tool | Purpose | Language | Automation Level |
|---|---|---|---|
| **`sigtool`** (CLI) | Verify signatures against a DLL; report match counts, RVAs, pass/fail | C++ (reuse existing scanner) | Fully automated |
| **`GenerateSignatures.py`** | Ghidra headless script: analyze DLL, extract candidate signatures with auto-wildcarding, output JSON | Python (PyGhidra) | Fully automated |
| **`sigcodegen`** | Code generator: convert signature JSON → C++ header (`signatures.gen.h`) for build consumption | Python | Fully automated |
| **`sigdiff`** | Diff two signature JSON files (old vs. new version); report which signatures changed, which are stable | Python | Fully automated |
| **`sigtest`** | Regression test runner: verify all signature sets against all known DLL versions; run functional tests | C++ (reuse existing tests) | Fully automated |

**`sigtool` (CLI verifier) — detailed spec:**

```
Usage:
  sigtool verify --dll <path> --signatures <json> [--verbose]
    Scans all signatures in <json> against <dll>.text section.
    Reports per-signature: match count, match RVA, PASS/FAIL.
    Exit 0 if all pass, exit 1 if any fail.

  sigtool verify-patch --dll <path> --signatures <json>
    Validates patch-site original bytes for all patch_site signatures.
    Exit 0 if all match, exit 1 if any mismatch.

  sigtool info --dll <path>
    Prints DLL version indicators: FileVersion, SHA-256, PE timestamp, file size.

  sigtool scan --dll <path> --pattern "55 8B EC ?? ??"
    Scans a single ad-hoc pattern. Reports all match RVAs.
```

This CLI is the workhorse of the update workflow. It wraps the existing `OrlibResolver` scanner logic (which already implements signature scanning, uniqueness enforcement, and patch-site validation) in a standalone CLI that doesn't require loading the full TsDevLib stack.

### 4.3 Build Integration: Code Generation from JSON

**Recommendation: Code generation from JSON → C++ header at build time.**

The build should consume signature data as follows:

```
signatures/v9.50.01.3344.json  (source of truth)
        │
        ▼
  sigcodegen (build-time code generator)
        │
        ▼
  src/signatures.gen.h  (generated, not committed)
        │
        ▼
  orlib_resolver.cpp  (includes signatures.gen.h)
```

**Why code generation over runtime JSON loading:**
- **Zero runtime parsing overhead.** Signatures are compiled in as `constexpr` arrays — same performance as the current hardcoded approach.
- **No JSON dependency in the C++ build.** The C++ project doesn't need a JSON parser; `sigcodegen` runs as a CMake pre-build step (Python script).
- **Type safety.** The generated header has the same `SignatureDef` struct the code already uses — no API changes needed.
- **The JSON files remain the source of truth.** They're committed to git, reviewed in diffs, and used by `sigtool` and `GenerateSignatures.py`. The generated header is a build artifact.

**CMake integration:**

```cmake
# Generate signatures.gen.h from all JSON files in signatures/
set(SIG_FILES
    ${CMAKE_SOURCE_DIR}/signatures/v9.50.01.3344.json
)
set(GEN_HEADER ${CMAKE_BINARY_DIR}/generated/signatures.gen.h)

add_custom_command(
    OUTPUT ${GEN_HEADER}
    COMMAND python ${CMAKE_SOURCE_DIR}/tools/sigcodegen.py
            --input ${SIG_FILES}
            --output ${GEN_HEADER}
    DEPENDS ${SIG_FILES} ${CMAKE_SOURCE_DIR}/tools/sigcodegen.py
    COMMENT "Generating signature header from JSON"
)
add_custom_target(siggen DEPENDS ${GEN_HEADER})
add_dependencies(tsdev_cli siggen)
target_include_directories(tsdev_cli PRIVATE ${CMAKE_BINARY_DIR}/generated)
```

**Alternative: direct JSON include at runtime.** If build-time code generation is deemed too complex, a simpler approach is to load JSON at runtime using a minimal JSON parser (e.g., nlohmann/json, single-header). This adds a ~500KB header dependency but eliminates the codegen step. The trade-off is runtime parsing overhead (negligible for 12 signatures) and a JSON dependency in the C++ build. **Code generation is still recommended** for type safety and zero-dependency.

### 4.4 Directory Structure

```
tsdev_automation/
├── signatures/                    # Signature data (source of truth)
│   ├── README.md                  # How to update signatures
│   ├── v9.50.01.3344.json         # Signature set for version 9.50.01.3344
│   ├── v9.50.01.3400.json         # (future) Signature set for version 9.50.01.3400
│   └── version_manifest.json      # Maps DLL versions → signature sets
├── tools/                         # Signature management tooling
│   ├── sigcodegen.py              # JSON → C++ header code generator
│   ├── sigdiff.py                 # Diff two signature JSON files
│   └── ghidra_scripts/
│       ├── GenerateSignatures.py  # Headless Ghidra signature extractor
│       └── FindTargetsByAnchor.py # (future) Locate functions by string/xref anchors
├── src/
│   ├── orlib_resolver.h           # (existing) Resolver interface
│   ├── orlib_resolver.cpp         # (existing) Resolver implementation
│   ├── signatures.gen.h           # (generated) C++ signature definitions
│   └── ...
├── tests/
│   ├── test_orlib_resolver.cpp    # (existing) Signature resolution tests
│   ├── test_insert.cpp            # (existing) Functional regression
│   ├── test_delete_diag.cpp       # (existing) Functional regression
│   └── test_modify.cpp            # (existing) Functional regression
├── docs/
│   ├── SIGNATURE_CATALOG.md       # (existing) Human-readable catalog
│   ├── VERSION_DETECTION.md       # (existing) Version detection design
│   └── SIGNATURE_INFRASTRUCTURE_REPORT.md  # This report
├── tmp/
│   └── re_artifacts/
│       ├── orlib20_baseline.dll   # (existing) Baseline DLL copy
│       └── orlib20_v<ver>.dll     # (future) New version DLL copies
└── CMakeLists.txt                 # (existing, modified for sigcodegen)
```

### 4.5 Version Manifest Format

The version manifest maps DLL version indicators to signature sets. It extends the existing `OrlibVersionManifestEntry` design from `docs/VERSION_DETECTION.md` §6.2, but stored as JSON instead of a compiled-in `constexpr` array.

**`signatures/version_manifest.json`:**

```json
{
  "schemaVersion": 1,
  "manifest": [
    {
      "fileVersion": "9.50.01.3344",
      "peTimestamp": "0x5C01735E",
      "fileSize": 4924400,
      "sha256": "318DB24A4F41BDB9FF69E2E647026B788706904CF887CBB0A953E003DB22EC18",
      "textVsize": "0x1F72FC",
      "textRawSize": "0x1F7400",
      "textRva": "0x1000",
      "imageBase": "0x10000000",
      "exportCount": 699,
      "exportNamedCount": 636,
      "signatureSetFile": "v9.50.01.3344.json",
      "signatureSetId": 1,
      "verified": true,
      "verifiedDate": "2026-08-17",
      "notes": "Baseline version — all 12 signatures verified, all functional tests pass"
    }
  ]
}
```

**How the build consumes it:** `sigcodegen.py` reads the manifest + all referenced signature JSON files and generates a single `signatures.gen.h` containing:
- `kOrlibVersionManifest[]` — the constexpr manifest array (same struct as existing `OrlibVersionManifestEntry`)
- `kSignatureSets[]` — an array of signature sets, indexed by `SignatureSetId`
- `kSignatures[]` — the active signature set (selected at runtime by version detection)

The runtime flow remains as designed in `orlib_resolver.h`: `detectVersion()` looks up the FileVersion in the manifest, selects the `SignatureSetId`, and uses the corresponding signature set for resolution.

---

## Section 5: Wildcard Automation Analysis

### 5.1 The Current Manual Wildcard Process

The current signatures in `b1_signatures.h` use `??` wildcards for four categories of address-dependent bytes (documented in `SIGNATURE_CATALOG.md` §2):

| Category | Bytes | Why wildcarded |
|---|---|---|
| SEH handler addresses | 4 bytes | Absolute, image-base-relative |
| Data-section references | 4 bytes in `MOV reg,[imm32]` | Absolute, image-base-relative |
| IAT indirect call targets | 4 bytes in `FF 15 imm32` | IAT slot addresses are image-base-relative |
| Relative call/jump targets | 4 bytes in `E8 imm32` / `0F 8x imm32` | Relative displacements change if code shifts |

Currently, these wildcard decisions are made manually by a human reading Ghidra disassembly. This is the most time-consuming part of signature creation.

### 5.2 Can Wildcard Decisions Be Automated?

**Yes, substantially.** Ghidra's instruction model provides the information needed to automatically determine which bytes are address/dynamic operands. The key API is `InstructionPrototype.getOperandValueMask(operandIndex)`, which returns a bit mask describing which bytes of the instruction encode the operand value.

The `makesig.py` script ([nosoop/ghidra_scripts](https://github.com/nosoop/ghidra_scripts/blob/master/makesig.py)) implements this exact logic:

```python
def shouldMaskOperand(ins, opIndex):
    optype = ins.getOperandType(opIndex)
    return optype & OperandType.DYNAMIC or optype & OperandType.ADDRESS

def getMaskedInstruction(ins):
    mask = [0] * ins.length
    proto = ins.getPrototype()
    for op in range(proto.getNumOperands()):
        if shouldMaskOperand(ins, op):
            mask = [m | (v & 0xFF) for m, v in
                    zip(mask, proto.getOperandValueMask(op).getBytes())]
    for m, b in zip(mask, ins.getBytes()):
        if m == 0xFF:
            yield BytePattern(is_wildcard=True, byte=None)
        else:
            yield BytePattern(byte=b & 0xFF, is_wildcard=False)
```

**`OperandType.DYNAMIC`** flags operands whose values are computed at load time (relative addresses — `E8 rel32`, `0F 8x rel32`). **`OperandType.ADDRESS`** flags operands that are absolute addresses (IAT call targets `FF 15 imm32`, data-section refs `MOV reg,[imm32]`, SEH handler addresses `68 imm32`).

### 5.3 Heuristics for Automatic Wildcarding

A Ghidra script can use the following heuristics, in order of reliability:

| Heuristic | API Method | What It Wildcards | Reliability |
|---|---|---|---|
| **1. Operand type = DYNAMIC** | `ins.getOperandType(op) & OperandType.DYNAMIC` | Relative call/jump displacements (`E8 rel32`, `0F 8x rel32`) | **High** — Ghidra correctly identifies relative addresses |
| **2. Operand type = ADDRESS** | `ins.getOperandType(op) & OperandType.ADDRESS` | Absolute addresses (IAT calls, data refs, SEH handlers) | **High** — Ghidra correctly identifies absolute addresses |
| **3. Operand value mask** | `proto.getOperandValueMask(op).getBytes()` | Precise byte-level mask for each operand | **High** — SLEIGH specifications define exact bit positions |
| **4. Short jump displacements** | Instruction mnemonic is `JZ`/`JNZ` with 1-byte displacement | Keep FIXED (short jumps are stable) | **High** — 1-byte displacements don't change unless code is reorganized |
| **5. Structural immediates** | Instruction has immediate operand that is NOT an address | Keep FIXED (stack sizes, record sizes, etc.) | **Medium** — requires distinguishing structural constants from addresses |
| **6. Register encodings** | Operand type = REGISTER | Keep FIXED (register assignments are stable) | **High** |

### 5.4 What CANNOT Be Automated

| Decision | Why It's Manual | Mitigation |
|---|---|---|
| **Signature length** — how many bytes to include | Too short = ambiguous; too long = brittle. Requires uniqueness check. | Automate via iterative extension: start with prologue, extend until unique. `Sigga` does this. |
| **Signature start point** — function entry vs. mid-function | For patch sites, the signature must match the containing function, not the patch byte itself. | Encode the `patchOffset` in metadata; always start at function entry. |
| **Ambiguity resolution** — when auto-generated sig matches >1 location | Requires human judgment to pick a distinguishing byte sequence. | Report all match locations; suggest extending by N bytes; human picks the extension. |
| **Cross-version stability** — which bytes will survive a version change | Cannot be determined from a single binary. | Use cross-binary verification (sigdrift pattern) if multiple versions are available. Otherwise, minimize signature length (fewer assumptions). |
| **Short jump displacement stability** — `74 15` vs. `74 16` | A 1-byte displacement could change if surrounding code changes. Currently kept fixed. | Accept as a risk; if a short-jump displacement changes, the signature breaks and must be regenerated. Wildcarding it would reduce distinctiveness. |

### 5.5 Recommended Wildcard Automation Strategy

```
┌─────────────────────────────────────────────────────────┐
│           AUTOMATIC WILDCARDING PIPELINE                 │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  For each instruction in the signature window:          │
│    1. Get instruction from Ghidra                       │
│    2. For each operand:                                 │
│       a. If OperandType.DYNAMIC → wildcard those bytes  │
│       b. If OperandType.ADDRESS → wildcard those bytes  │
│       c. Else → keep bytes fixed                        │
│    3. Remove trailing wildcard bytes                    │
│                                                         │
│  Then verify uniqueness:                                │
│    4. Scan .text for the pattern                        │
│    5. If matchCount == 1 → accept                       │
│    6. If matchCount > 1 → extend by N more instructions │
│       and re-scan (repeat until unique or maxLen)       │
│    7. If matchCount == 0 → error (shouldn't happen      │
│       for a signature extracted from the same binary)   │
│                                                         │
│  Human review:                                          │
│    8. Review the auto-generated signature               │
│    9. Compare against the old signature (sigdiff)       │
│   10. If wildcards differ, verify the change makes sense│
│                                                         │
└─────────────────────────────────────────────────────────┘
```

This pipeline automates steps 1-7 (the time-consuming part) and leaves steps 8-10 for human review (the judgment part). The `Sigga` script ([lexika979/Sigga](https://github.com/lexika979/Sigga)) implements a similar auto-cascading approach: "Automatically retries with lower strictness or different strategies if a unique signature cannot be found initially."

### 5.6 Validating Auto-Wildcarding Against the Current Signatures

As a validation step, we should run the auto-wildcarding script against the baseline DLL and compare its output to the manually-created signatures in `b1_signatures.h`. If the auto-generated wildcards match the manual wildcards for all 12 targets, we have high confidence that the automation is correct. Any discrepancies would reveal either (a) bugs in the automation or (b) cases where the human made a judgment call that the automation can't replicate — both are valuable findings.

---

## Section 6: Recommendation Summary

### 6.1 The Single Recommended Approach

**Adopt a JSON-based signature catalog with Ghidra headless auto-generation, C++ code generation at build time, and a "try existing first" update workflow.**

### 6.2 Architecture at a Glance

```
┌──────────────────────────────────────────────────────────────┐
│                    SIGNATURE MANAGEMENT                       │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  SOURCE OF TRUTH: signatures/*.json                          │
│  ├── v9.50.01.3344.json     (12 sigs + 3 data globals)      │
│  ├── v<future>.json         (future version sigs)            │
│  └── version_manifest.json  (version → sigset mapping)       │
│                                                              │
│  BUILD TIME:                                                 │
│  sigcodegen.py  →  src/signatures.gen.h  →  orlib_resolver   │
│  (JSON → C++ constexpr arrays, no runtime parsing)           │
│                                                              │
│  UPDATE TIME (new DLL version):                              │
│  1. sigtool verify --dll new.dll --signatures current.json   │
│     → (TRY EXISTING FIRST — 90% of the time, this works)     │
│  2. If fail: analyzeHeadless + GenerateSignatures.py         │
│     → (AUTO-WILDCARD via getOperandValueMask, output JSON)   │
│  3. Human reviews candidate JSON (~15-30 min)                │
│  4. sigtool verify --dll new.dll --signatures candidate.json │
│  5. Run functional regression tests                          │
│  6. Add to manifest, commit                                  │
│                                                              │
│  CI GATE:                                                    │
│  sigtool verify against all known DLL versions               │
│  → fail build if any signature ambiguous or missing          │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 6.3 Justification

| Decision | Rationale | Alternatives Considered |
|---|---|---|
| **JSON for signature data** | Industry standard (Ghidra scripts output JSON natively); human-readable; diff-friendly; supports metadata; decouples data from code | C++ header (current — couples data to build, no metadata); TOML (viable but less tooling); YARA (wrong abstraction level); custom DSL (maintenance burden) |
| **Code generation at build time** | Zero runtime overhead; no JSON dependency in C++; type-safe; same API as current code | Runtime JSON loading (adds dependency, parsing overhead); direct C++ header editing (current — not diff-friendly) |
| **Ghidra headless for auto-generation** | Already installed; full API access for operand masking; proven by makesig.py, Sigga, openrelik-worker-ghidra; can output JSON directly | IDA Pro (not installed); manual Ghidra GUI (current — multi-hour effort); pure C++ scanner (can't generate, only verify) |
| **Auto-wildcard via getOperandValueMask** | Proven by makesig.py; uses Ghidra's SLEIGH specifications (authoritative); handles all 4 wildcard categories automatically; reduces human effort from hours to minutes | Manual wildcarding (current — hours of RE); heuristic-only without Ghidra (less reliable) |
| **"Try existing first" workflow** | Most DLL updates are minor rebuilds that preserve prologue patterns; turns hours into seconds; used by kananlib, CE community, SourceMod | Always regenerate (wastes time when existing sigs work); always manual RE (current — always hours) |
| **Multi-version manifest** | Maps versions to signature sets; enables CI testing against all known versions; extends existing VERSION_DETECTION.md design | Single signature set (breaks on version change); hardcoded version checks (not scalable) |
| **sigtool CLI** | Standalone verifier wrapping existing scanner; enables rapid iteration without full TsDevLib stack; CI-friendly exit codes | Full TsDevLib test suite (too heavy for quick checks); manual Ghidra scanning (too slow) |

### 6.4 Implementation Priority (Recommended TVS Sequence)

| Priority | Task | Effort | Impact |
|---|---|---|---|
| **1 (highest)** | Create `sigtool verify` CLI — extract scanner from `OrlibResolver` into a standalone tool | Low (reuse existing code) | Enables "try existing first" — the single biggest workflow improvement |
| **2** | Convert `b1_signatures.h` → `signatures/v9.50.01.3344.json` + write `sigcodegen.py` | Medium (data migration + codegen) | Separates data from code; makes signatures diff-friendly |
| **3** | Write `GenerateSignatures.py` Ghidra headless script | Medium (Ghidra API scripting) | Automates signature regeneration; auto-wildcarding |
| **4** | Write `sigdiff.py` | Low (JSON diff tool) | Aids human review of candidate signatures |
| **5** | Add CI gate (`sigtool verify` against all known versions) | Low (CI integration) | Catches signature regressions automatically |
| **6 (future)** | Add anchor-based function relocation for new DLL versions | High (requires RE of anchor strategies) | Reduces human-in-the-loop for major version changes |
| **7 (future)** | Cross-binary signature verification (if multiple DLL versions obtained) | High (sigdrift-style pipeline) | Produces maximally stable signatures |

### 6.5 Key Insight

The single highest-ROI improvement is **`sigtool verify`** (Priority 1). It enables the "try existing first" pattern: when a new DLL version arrives, run the existing signatures against it. If they all match uniquely (the common case for minor rebuilds), the update is a 2-minute manifest entry — not a multi-hour RE session. Only when signatures fail does the heavier Ghidra headless regeneration pipeline need to run, and even then, the auto-wildcarding script reduces the human effort from hours to ~15-30 minutes of review.

This transforms the update workflow from **"always multi-hour manual RE"** to **"5-second automated check, 90% of the time; 15-30 minute review, 10% of the time; multi-hour manual RE, <1% of the time."**

---

## References

1. SourceMod SDKTools — GameConfig / Signature Scans: https://wiki.alliedmods.net/SDKTools_(SourceMod_Development)
2. SourceMod Advanced Gamedata Guide: https://wiki.alliedmods.net/User:Nosoop/Guide/Advanced
3. nosoop/ghidra_scripts — makesig.py (SourceMod signature generator for Ghidra): https://github.com/nosoop/ghidra_scripts/blob/master/makesig.py
4. lexika979/Sigga — Ghidra signature creation script with auto-cascading tiers: https://github.com/lexika979/Sigga
5. mahmoudimus/ida-sigmaker — IDA Pro signature maker with processor-aware wildcarding: https://github.com/mahmoudimus/ida-sigmaker
6. koyzdev/sigdrift — Cross-binary signature verification for Windows builds: https://github.com/koyzdev/sigdrift
7. cursey/kananlib — C++ binary analysis library for runtime modding (AVX2 scanning, string-ref function discovery): https://github.com/cursey/kananlib
8. REFramework — Multi-pattern fallback arrays for function resolution: https://github.com/praydog/REFramework/blob/af1495a5/shared/sdk/REManagedObject.cpp
9. Cheat Engine AOB Tutorial: https://wiki.cheatengine.org/index.php?title=Tutorials%3AAOBs
10. Cheat Engine — Tips for update-friendly scripts: https://www.cheatengine.org/forum/viewtopic.php?t=621624
11. YARA Style Guide: https://neo23x0.github.io/YARA-Style-Guide/
12. YARA-X Metadata Standards Enforcement: https://virustotal.github.io/yara-x/blog/enforcing-yara-metadata-standards/
13. Creating a YARA Repository: https://brkalbyrk.github.io/posts/Creating-a-YARA-Repository/
14. Ghidra Headless Analyzer documentation: https://www.ghidradocs.com/11.0.1_PUBLIC/docs/GhidraClass/Intermediate/HeadlessAnalyzer.html
15. Ghidra InstructionPrototype API (getOperandValueMask): https://www.ghidradocs.com/11.3_PUBLIC/docs/GhidraAPI_javadoc/api/ghidra/program/model/lang/InstructionPrototype.html
16. Computing Ghidra Function ID Hashes (masked instruction bytes): https://blog.nietaanraken.nl/posts/ghidra-function-id/
17. dynacylabs/ghidra-rizzo — Cross-binary function matching (fuzzy/string/immediate signatures): https://github.com/dynacylabs/ghidra-rizzo
18. ckane/CkaneGhidraScripts — JsonBlocks.java (function/block JSON export): https://github.com/ckane/CkaneGhidraScripts
19. dig-sec/openrelik-worker-ghidra — Headless Ghidra JSON output pipeline: https://github.com/dig-sec/openrelik-worker-ghidra
20. kirkderp/rbinghidra — MCP server wrapping analyzeHeadless with JSON output: https://github.com/kirkderp/rbinghidra
21. mitsuhiko/agent-stuff — Ghidra skill with ExportFunctions.java JSON output: https://github.com/mitsuhiko/agent-stuff/blob/HEAD/skills/ghidra/SKILL.md
22. K4ryuu/IDA-GameDataTracker — IDA plugin for tracking game functions across binary updates: https://github.com/K4ryuu/IDA-GameDataTracker
23. procmod/procmod-scan — IDA-style pattern scanning with SIMD acceleration: https://github.com/procmod/procmod-scan
24. H0llyW00dzZ/pe-sigscan — PE signature scanning with uniqueness verification: https://github.com/h0llyw00dzz/pe-sigscan
25. Project files: `docs/SIGNATURE_CATALOG.md`, `docs/VERSION_DETECTION.md`, `src/orlib_resolver.h`, `tmp/re_artifacts/b1_signatures.h`
