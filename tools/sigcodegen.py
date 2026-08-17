#!/usr/bin/env python3
# =============================================================================
# sigcodegen.py — JSON -> C++ header generator for orlib20.dll signature data.
#
# Reads the version manifest + all referenced signature set JSON files and
# generates a single C++ header (signatures.gen.h) containing:
#   - SignatureSetId enum (from manifest signatureSetId values)
#   - Per-version kSignatures_<version>[] / kDataGlobals_<version>[] /
#     kExports_<version>[] arrays
#   - kSignatureSets[] grouping per-version data
#   - Backward-compatible kSignatures / kDataGlobals / kExports aliases
#   - kOrlibVersionManifest[] (OrlibVersionManifestEntry from orlib_resolver.h)
#
# The generated header is byte-identical in DATA to the historical
# tmp/re_artifacts/b1_signatures.h, plus the new originalBytesMask field and
# the version manifest arrays.
#
# Standard library only (json, argparse, sys, os, datetime). No third-party deps.
#
# Usage:
#   python tools/sigcodegen.py --manifest <version_manifest.json> \
#                              --sigdir <signatures_dir> \
#                              --output <signatures.gen.h>
# =============================================================================

import argparse
import json
import os
import sys
from datetime import datetime, timezone


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EXPECTED_SIGNATURE_COUNT = 12
EXPECTED_DATA_GLOBAL_COUNT = 3
EXPECTED_EXPORT_COUNT = 7

# Hex pattern tokens are split across adjacent C++ string literals in chunks of
# this many bytes for readability (matches the historical b1_signatures.h style
# and the ARCHITECTURE_REDESIGN.md §4.4 example).
HEX_CHUNK_BYTES = 24


# ---------------------------------------------------------------------------
# Error helper
# ---------------------------------------------------------------------------

class CodegenError(Exception):
    """Raised on any validation or generation failure. Exits non-zero."""
    pass


def fail(message):
    raise CodegenError(message)


# ---------------------------------------------------------------------------
# Parsing / validation helpers
# ---------------------------------------------------------------------------

def parse_hex_int(value, field_name):
    """Parse a JSON value that may be an int or a hex string ('0x...') into an int."""
    if isinstance(value, bool):
        fail("{}: expected int or hex string, got bool".format(field_name))
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            if text.lower().startswith("0x"):
                return int(text, 16)
            return int(text, 0)
        except ValueError:
            fail("{}: invalid hex/integer literal {!r}".format(field_name, value))
    fail("{}: expected int or hex string, got {}".format(field_name, type(value).__name__))
    return 0  # unreachable


def count_hex_bytes(hex_pattern):
    """Count the number of byte tokens in a space-separated hex pattern string."""
    tokens = [t for t in hex_pattern.split() if t]
    return len(tokens)


def validate_hex_pattern(hex_pattern, field_name):
    """Validate that hex_pattern is space-separated hex bytes with ?? wildcards only."""
    if not isinstance(hex_pattern, str):
        fail("{}: hexPattern must be a string".format(field_name))
    tokens = [t for t in hex_pattern.split() if t]
    if not tokens:
        fail("{}: hexPattern is empty".format(field_name))
    for tok in tokens:
        if tok == "??":
            continue
        if len(tok) != 2:
            fail("{}: invalid hex token {!r} (must be 2 chars or '??')".format(
                field_name, tok))
        try:
            int(tok, 16)
        except ValueError:
            fail("{}: invalid hex token {!r}".format(field_name, tok))


def require_field(obj, field, context):
    if field not in obj:
        fail("{}: missing required field {!r}".format(context, field))
    return obj[field]


def version_suffix(file_version):
    """Convert a fileVersion string ('9.50.01.3344') to a C++ suffix ('v9_50_01_3344')."""
    return "v" + file_version.replace(".", "_")


def enum_name_for_version(file_version):
    """Convert a fileVersion string to a SignatureSetId enum name."""
    return "SIGSET_" + version_suffix(file_version).upper()


# ---------------------------------------------------------------------------
# Manifest + signature set loading
# ---------------------------------------------------------------------------

def load_manifest(manifest_path):
    """Load and parse the version manifest JSON. Returns the manifest entries list."""
    if not os.path.isfile(manifest_path):
        fail("manifest file not found: {}".format(manifest_path))
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        fail("manifest JSON parse error in {}: {}".format(manifest_path, e))
    if not isinstance(data, dict):
        fail("manifest root must be a JSON object")
    manifest = require_field(data, "manifest", "manifest")
    if not isinstance(manifest, list) or not manifest:
        fail("manifest: 'manifest' must be a non-empty array")
    return manifest


def load_signature_set(sigdir, signature_set_file):
    """Load a signature set JSON file from sigdir. Returns the parsed dict."""
    path = os.path.join(sigdir, signature_set_file)
    if not os.path.isfile(path):
        fail("signature set file not found: {} (referenced by manifest)".format(path))
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        fail("signature set JSON parse error in {}: {}".format(path, e))
    if not isinstance(data, dict):
        fail("signature set root must be a JSON object: {}".format(path))
    return data


def validate_manifest(manifest):
    """Validate manifest-level invariants: unique signatureSetId values, required fields."""
    seen_ids = set()
    for idx, entry in enumerate(manifest):
        ctx = "manifest[{}]".format(idx)
        if not isinstance(entry, dict):
            fail("{}: manifest entry must be a JSON object".format(ctx))
        for field in ("fileVersion", "peTimestamp", "fileSize", "sha256",
                      "textVsize", "textRawSize", "textRva", "imageBase",
                      "exportCount", "exportNamedCount", "exportHighestOrdinal",
                      "exportOrdinalBase", "productVersion", "productName",
                      "fileDescription", "companyName", "internalName",
                      "originalFilename", "signatureSetFile", "signatureSetId"):
            require_field(entry, field, ctx)
        sid = entry["signatureSetId"]
        if not isinstance(sid, int) or isinstance(sid, bool):
            fail("{}: signatureSetId must be an integer".format(ctx))
        if sid in seen_ids:
            fail("{}: duplicate signatureSetId {} across manifest entries".format(
                ctx, sid))
        if sid <= 0:
            fail("{}: signatureSetId must be > 0 (0 is reserved for SIGSET_NONE)".format(
                ctx))
        seen_ids.add(sid)


def validate_signature_set(sigset, file_version, signature_set_file):
    """Validate a signature set: counts, required fields, hex patterns, uniqueness."""
    ctx = "signature set {}".format(signature_set_file)

    sigs = require_field(sigset, "signatures", ctx)
    dgs = require_field(sigset, "dataGlobals", ctx)
    exps = require_field(sigset, "exports", ctx)

    if not isinstance(sigs, list) or len(sigs) != EXPECTED_SIGNATURE_COUNT:
        fail("{}: expected exactly {} signatures, got {}".format(
            ctx, EXPECTED_SIGNATURE_COUNT,
            len(sigs) if isinstance(sigs, list) else "non-array"))
    if not isinstance(dgs, list) or len(dgs) != EXPECTED_DATA_GLOBAL_COUNT:
        fail("{}: expected exactly {} dataGlobals, got {}".format(
            ctx, EXPECTED_DATA_GLOBAL_COUNT,
            len(dgs) if isinstance(dgs, list) else "non-array"))
    if not isinstance(exps, list) or len(exps) != EXPECTED_EXPORT_COUNT:
        fail("{}: expected exactly {} exports, got {}".format(
            ctx, EXPECTED_EXPORT_COUNT,
            len(exps) if isinstance(exps, list) else "non-array"))

    seen_names = set()
    for i, sig in enumerate(sigs):
        sctx = "{} signatures[{}]".format(ctx, i)
        if not isinstance(sig, dict):
            fail("{}: signature must be a JSON object".format(sctx))
        for field in ("name", "targetType", "callingConvention", "expectedRva",
                      "hexPattern", "length", "expectedMatchCount"):
            require_field(sig, field, sctx)
        name = sig["name"]
        if not isinstance(name, str) or not name:
            fail("{}: name must be a non-empty string".format(sctx))
        if name in seen_names:
            fail("{}: duplicate signature name {!r}".format(sctx, name))
        seen_names.add(name)

        target_type = sig["targetType"]
        if target_type not in ("call_target", "patch_site"):
            fail("{}: targetType must be 'call_target' or 'patch_site', got {!r}".format(
                sctx, target_type))

        hex_pattern = sig["hexPattern"]
        validate_hex_pattern(hex_pattern, sctx)
        actual_len = count_hex_bytes(hex_pattern)
        if not isinstance(sig["length"], int) or isinstance(sig["length"], bool):
            fail("{}: length must be an integer".format(sctx))
        if sig["length"] != actual_len:
            fail("{}: length {} does not match actual byte count {} in hexPattern".format(
                sctx, sig["length"], actual_len))

        if sig["expectedMatchCount"] != 1:
            fail("{}: expectedMatchCount must be 1, got {}".format(
                sctx, sig["expectedMatchCount"]))

        if target_type == "patch_site":
            patch = require_field(sig, "patch", sctx)
            if not isinstance(patch, dict):
                fail("{}: patch must be a JSON object".format(sctx))
            for field in ("offset", "originalBytes", "replacementBytes",
                          "originalBytesMask"):
                require_field(patch, field, sctx + ".patch")
            validate_hex_pattern(patch["originalBytes"], sctx + ".patch.originalBytes")
            validate_hex_pattern(patch["replacementBytes"],
                                 sctx + ".patch.replacementBytes")
            validate_hex_pattern(patch["originalBytesMask"],
                                 sctx + ".patch.originalBytesMask")
            if not isinstance(patch["offset"], int) or isinstance(patch["offset"], bool):
                fail("{}: patch.offset must be an integer".format(sctx))

    seen_dg_names = set()
    for i, dg in enumerate(dgs):
        dctx = "{} dataGlobals[{}]".format(ctx, i)
        if not isinstance(dg, dict):
            fail("{}: dataGlobal must be a JSON object".format(dctx))
        for field in ("name", "rva", "extractionMethod", "sourceSignature",
                      "sourceInstructionOffset"):
            require_field(dg, field, dctx)
        if dg["name"] in seen_dg_names:
            fail("{}: duplicate dataGlobal name {!r}".format(dctx, dg["name"]))
        seen_dg_names.add(dg["name"])

    seen_exp_names = set()
    for i, exp in enumerate(exps):
        ectx = "{} exports[{}]".format(ctx, i)
        if not isinstance(exp, dict):
            fail("{}: export must be a JSON object".format(ectx))
        for field in ("name", "rva", "callingConvention"):
            require_field(exp, field, ectx)
        if exp["name"] in seen_exp_names:
            fail("{}: duplicate export name {!r}".format(ectx, exp["name"]))
        seen_exp_names.add(exp["name"])


# ---------------------------------------------------------------------------
# C++ code generation helpers
# ---------------------------------------------------------------------------

def fmt_rva(value):
    """Format an RVA as an uppercase hex literal (no zero-padding), e.g. 0x165DE0."""
    return "0x{:X}".format(value)


def fmt_u32_hex(value):
    """Format a uint32 as an 8-digit uppercase hex literal, e.g. 0x001F72FC."""
    return "0x{:08X}".format(value)


def fmt_patch_offset(value):
    """Format a patch offset: 0 -> '0', nonzero -> uppercase hex (0x43)."""
    if value == 0:
        return "0"
    return "0x{:X}".format(value)


def split_hex_pattern(hex_pattern):
    """Split a space-separated hex pattern into chunks of HEX_CHUNK_BYTES tokens,
    returning a list of substring chunks (each ending with a trailing space except
    the last). Used to emit adjacent C++ string literals for readability."""
    tokens = hex_pattern.split()
    chunks = []
    for start in range(0, len(tokens), HEX_CHUNK_BYTES):
        chunk_tokens = tokens[start:start + HEX_CHUNK_BYTES]
        chunks.append(" ".join(chunk_tokens))
    return chunks


def emit_hex_pattern_literal(hex_pattern, indent):
    """Emit a hex pattern as one or more adjacent C++ string literals."""
    chunks = split_hex_pattern(hex_pattern)
    if len(chunks) == 1:
        return '"{}"'.format(chunks[0])
    pad = " " * indent
    lines = []
    for i, chunk in enumerate(chunks):
        suffix = " " if i < len(chunks) - 1 else ""
        lines.append('{}"{}{}"'.format(pad, chunk, suffix))
    return "\n".join(lines)


def c_string_literal(s):
    """Emit a C string literal for a normal string value."""
    return '"{}"'.format(s)


# ---------------------------------------------------------------------------
# C++ code generation
# ---------------------------------------------------------------------------

def generate_header(manifest, sigsets, manifest_paths, timestamp_str):
    """Generate the full C++ header text.

    manifest: list of manifest entry dicts (validated).
    sigsets: list of (manifest_entry, signature_set_dict) tuples (validated).
    manifest_paths: list of signatureSetFile names (for the header comment).
    """
    out = []

    # 1. Header comment block
    out.append("// =============================================================================")
    out.append("// signatures.gen.h - AUTO-GENERATED by tools/sigcodegen.py")
    out.append("// DO NOT EDIT. Regenerate by rebuilding the project.")
    out.append("//")
    for p in manifest_paths:
        out.append("// Source: signatures/{}".format(p))
    out.append("// Manifest: signatures/version_manifest.json")
    out.append("// Generated: {}".format(timestamp_str))
    out.append("// =============================================================================")
    out.append("")

    # 2. Include guard
    out.append("#ifndef SIGNATURES_GEN_H")
    out.append("#define SIGNATURES_GEN_H")
    out.append("")

    # 3. Includes
    out.append("#include <cstdint>")
    out.append("#include <cstddef>")
    out.append("")

    # 4. namespace tsdev open (for SignatureSetId enum)
    out.append("namespace tsdev {")
    out.append("")

    # 5. SignatureSetId enum
    out.append("// ---------------------------------------------------------------------------")
    out.append("// SignatureSetId enum (generated from manifest signatureSetId values)")
    out.append("// OWNED by this generated header - orlib_resolver.h must NOT redefine it.")
    out.append("// Defined before #include \"orlib_resolver.h\" so that")
    out.append("// OrlibVersionManifestEntry (which contains a SignatureSetId member by value)")
    out.append("// can use it.")
    out.append("// ---------------------------------------------------------------------------")
    out.append("enum SignatureSetId : uint16_t {")
    out.append("    SIGSET_NONE = 0,")
    # Sort manifest entries by signatureSetId for stable enum ordering.
    sorted_entries = sorted(manifest, key=lambda e: e["signatureSetId"])
    for entry in sorted_entries:
        name = enum_name_for_version(entry["fileVersion"])
        sid = entry["signatureSetId"]
        out.append("    {} = {},".format(name, sid))
    out.append("};")
    out.append("")

    # 6. Close namespace
    out.append("} // namespace tsdev")
    out.append("")

    # 7. Include orlib_resolver.h
    out.append('#include "orlib_resolver.h"  // Provides OrlibVersionManifestEntry (uses SignatureSetId)')
    out.append("")

    # 8. Reopen namespace tsdev
    out.append("namespace tsdev {")
    out.append("")

    # 9. SignatureDef struct
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Signature definition (compatible with existing SignatureDef from b1_signatures.h)")
    out.append("// ---------------------------------------------------------------------------")
    out.append("struct SignatureDef {")
    out.append("    const char* name;")
    out.append("    int          rva;")
    out.append("    const char* hexPattern;")
    out.append("    int          expectedMatchCount;")
    out.append("    bool         isPatchSite;")
    out.append("    const char* originalBytes;")
    out.append("    const char* replacementBytes;")
    out.append("    const char* originalBytesMask;  // Per-byte mask: \"FF\" = check, \"00\" = wildcard")
    out.append("    int          patchOffset;")
    out.append("    const char* containingFunction;")
    out.append("};")
    out.append("")

    # 10. DataGlobalDef struct
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Data global definition")
    out.append("// ---------------------------------------------------------------------------")
    out.append("struct DataGlobalDef {")
    out.append("    const char* name;")
    out.append("    int          rva;")
    out.append("    const char* extractionMethod;")
    out.append("    const char* sourceSignature;")
    out.append("    int          sourceInstructionOffset;")
    out.append("};")
    out.append("")

    # 11. ExportDef struct
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Export definition")
    out.append("// ---------------------------------------------------------------------------")
    out.append("struct ExportDef {")
    out.append("    const char* name;")
    out.append("    int          rva;")
    out.append("    const char* callingConvention;")
    out.append("    bool         decorated;")
    out.append("};")
    out.append("")

    # 12. SignatureSet struct
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Signature set: groups signatures + data globals + exports for one DLL version")
    out.append("// ---------------------------------------------------------------------------")
    out.append("struct SignatureSet {")
    out.append("    SignatureSetId            id;")
    out.append("    const SignatureDef*       signatures;")
    out.append("    int                       signatureCount;")
    out.append("    const DataGlobalDef*      dataGlobals;")
    out.append("    int                       dataGlobalCount;")
    out.append("    const ExportDef*          exports;")
    out.append("    int                       exportCount;")
    out.append("};")
    out.append("")

    # 13-15. Per-version arrays
    for (entry, sigset) in sigsets:
        suffix = version_suffix(entry["fileVersion"])
        out.append("// ---------------------------------------------------------------------------")
        out.append("// Generated data (from {})".format(entry["signatureSetFile"]))
        out.append("// ---------------------------------------------------------------------------")
        out.append("")

        # 13. kSignatures_<version>[]
        out.append("constexpr SignatureDef kSignatures_{}[] = {{".format(suffix))
        for sig in sigset["signatures"]:
            name = sig["name"]
            rva = parse_hex_int(sig["expectedRva"], "{}.expectedRva".format(name))
            hex_pattern = sig["hexPattern"]
            is_patch = sig["targetType"] == "patch_site"
            out.append("    {")
            out.append('        {},'.format(c_string_literal(name)))
            out.append('        {},'.format(fmt_rva(rva)))
            out.append(emit_hex_pattern_literal(hex_pattern, 8) + ",")
            if is_patch:
                patch = sig["patch"]
                orig = patch["originalBytes"]
                repl = patch["replacementBytes"]
                mask = patch["originalBytesMask"]
                offset = patch["offset"]
                containing = sig.get("containingFunction")
                out.append('        1, true,')
                out.append('        {},'.format(c_string_literal(orig)))
                out.append('        {},'.format(c_string_literal(repl)))
                out.append('        {},'.format(c_string_literal(mask)))
                if containing is not None:
                    out.append('        {},'.format(fmt_patch_offset(offset)))
                    out.append('        {}'.format(c_string_literal(containing)))
                else:
                    out.append('        {}'.format(fmt_patch_offset(offset)))
            else:
                out.append('        1, false, nullptr, nullptr, nullptr, 0, nullptr')
            out.append("    },")
        out.append("};")
        out.append("")

        # 14. kDataGlobals_<version>[]
        out.append("constexpr DataGlobalDef kDataGlobals_{}[] = {{".format(suffix))
        for dg in sigset["dataGlobals"]:
            name = dg["name"]
            rva = parse_hex_int(dg["rva"], "{}.rva".format(name))
            method = dg["extractionMethod"]
            src_sig = dg["sourceSignature"]
            offset = dg["sourceInstructionOffset"]
            out.append('    {{{}, {}, {}, {}, {}}},'.format(
                c_string_literal(name),
                fmt_rva(rva),
                c_string_literal(method),
                c_string_literal(src_sig),
                offset))
        out.append("};")
        out.append("")

        # 15. kExports_<version>[]
        out.append("constexpr ExportDef kExports_{}[] = {{".format(suffix))
        for exp in sigset["exports"]:
            name = exp["name"]
            rva = parse_hex_int(exp["rva"], "{}.rva".format(name))
            conv = exp["callingConvention"]
            decorated = exp.get("decorated", True)
            out.append('    {{{}, {}, {}, {}}},'.format(
                c_string_literal(name),
                fmt_rva(rva),
                c_string_literal(conv),
                "true" if decorated else "false"))
        out.append("};")
        out.append("")

    # 16. kSignatureSets[]
    out.append("constexpr SignatureSet kSignatureSets[] = {")
    for (entry, sigset) in sigsets:
        suffix = version_suffix(entry["fileVersion"])
        enum = enum_name_for_version(entry["fileVersion"])
        out.append("    {")
        out.append("        {},".format(enum))
        out.append("        kSignatures_{}, {},".format(suffix, len(sigset["signatures"])))
        out.append("        kDataGlobals_{}, {},".format(suffix, len(sigset["dataGlobals"])))
        out.append("        kExports_{}, {},".format(suffix, len(sigset["exports"])))
        out.append("    },")
    out.append("};")
    out.append("")

    # 17. kSignatureSetCount
    out.append("constexpr int kSignatureSetCount =")
    out.append("    static_cast<int>(sizeof(kSignatureSets) / sizeof(kSignatureSets[0]));")
    out.append("")

    # 18. Backward-compatible aliases (point to first set)
    first_entry, first_sigset = sigsets[0]
    first_suffix = version_suffix(first_entry["fileVersion"])
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Backward-compatible aliases (for orlib_resolver.cpp's existing")
    out.append("// buildSignatureCatalog). The active signature set (first set for now;")
    out.append("// runtime selection added later).")
    out.append("// ---------------------------------------------------------------------------")
    out.append("constexpr const SignatureDef* kSignatures = kSignatures_{};".format(first_suffix))
    out.append("constexpr int kSignatureCount = {};".format(len(first_sigset["signatures"])))
    out.append("constexpr const DataGlobalDef* kDataGlobals = kDataGlobals_{};".format(first_suffix))
    out.append("constexpr int kDataGlobalCount = {};".format(len(first_sigset["dataGlobals"])))
    out.append("constexpr const ExportDef* kExports = kExports_{};".format(first_suffix))
    out.append("constexpr int kExportCount = {};".format(len(first_sigset["exports"])))
    # Per-version count aliases
    for (entry, sigset) in sigsets:
        suffix = version_suffix(entry["fileVersion"])
        out.append("constexpr int kDataGlobalCount_{} = {};".format(
            suffix, len(sigset["dataGlobals"])))
        out.append("constexpr int kExportCount_{} = {};".format(
            suffix, len(sigset["exports"])))
    out.append("")

    # 19. kOrlibVersionManifest[]
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Version manifest (from version_manifest.json)")
    out.append("// Uses OrlibVersionManifestEntry from orlib_resolver.h (included above).")
    out.append("// Field order matches OrlibVersionManifestEntry struct EXACTLY:")
    out.append("//   fileVersion, peTimestamp, fileSize, sha256,")
    out.append("//   textVsize, textRawSize, textRva, imageBase,")
    out.append("//   exportCount, exportNamedCount, exportHighestOrdinal, exportOrdinalBase,")
    out.append("//   productVersion, productName, fileDescription, companyName,")
    out.append("//   internalName, originalFilename, signatureSet")
    out.append("// ---------------------------------------------------------------------------")
    out.append("constexpr OrlibVersionManifestEntry kOrlibVersionManifest[] = {")
    for entry in manifest:
        enum = enum_name_for_version(entry["fileVersion"])
        pe_ts = parse_hex_int(entry["peTimestamp"], "manifest peTimestamp")
        file_size = entry["fileSize"]
        sha = entry["sha256"]
        text_vsize = parse_hex_int(entry["textVsize"], "manifest textVsize")
        text_raw = parse_hex_int(entry["textRawSize"], "manifest textRawSize")
        text_rva = parse_hex_int(entry["textRva"], "manifest textRva")
        image_base = parse_hex_int(entry["imageBase"], "manifest imageBase")
        out.append("    {")
        out.append('        {},'.format(c_string_literal(entry["fileVersion"])))
        out.append('        {}, {}, // peTimestamp, fileSize'.format(
            fmt_u32_hex(pe_ts), file_size))
        out.append('        {},'.format(c_string_literal(sha)))
        out.append('        {}, {}, {}, {}, // textVsize, textRawSize, textRva, imageBase'.format(
            fmt_u32_hex(text_vsize), fmt_u32_hex(text_raw),
            fmt_u32_hex(text_rva), fmt_u32_hex(image_base)))
        out.append('        {}, {}, {}, {}, // exportCount, exportNamedCount, exportHighestOrdinal, exportOrdinalBase'.format(
            entry["exportCount"], entry["exportNamedCount"],
            entry["exportHighestOrdinal"], entry["exportOrdinalBase"]))
        out.append('        {},'.format(c_string_literal(entry["productVersion"])))
        out.append('        {},'.format(c_string_literal(entry["productName"])))
        out.append('        {},'.format(c_string_literal(entry["fileDescription"])))
        out.append('        {},'.format(c_string_literal(entry["companyName"])))
        out.append('        {},'.format(c_string_literal(entry["internalName"])))
        out.append('        {},'.format(c_string_literal(entry["originalFilename"])))
        out.append('        {}  // signatureSet'.format(enum))
        out.append("    },")
    out.append("};")
    out.append("")

    # 20. kOrlibVersionManifestCount
    out.append("constexpr size_t kOrlibVersionManifestCount =")
    out.append("    sizeof(kOrlibVersionManifest) / sizeof(kOrlibVersionManifest[0]);")
    out.append("")

    # 21. Close namespace + endif
    out.append("} // namespace tsdev")
    out.append("")
    out.append("#endif // SIGNATURES_GEN_H")
    out.append("")

    return "\n".join(out)


# ---------------------------------------------------------------------------
# Timestamp
# ---------------------------------------------------------------------------

def get_timestamp():
    """Return a timestamp string. Uses SOURCE_DATE_EPOCH if set (deterministic),
    else current UTC time."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            dt = datetime.fromtimestamp(int(epoch), tz=timezone.utc)
        except (ValueError, OSError):
            dt = datetime.now(timezone.utc)
    else:
        dt = datetime.now(timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="sigcodegen.py",
        description="Generate signatures.gen.h from signature JSON files.")
    parser.add_argument("--manifest", required=True,
                        help="Path to version_manifest.json.")
    parser.add_argument("--sigdir", required=True,
                        help="Directory containing signature set JSON files.")
    parser.add_argument("--output", required=True,
                        help="Path to write the generated header.")
    args = parser.parse_args(argv)

    try:
        # 1. Parse manifest
        manifest = load_manifest(args.manifest)

        # Validate manifest-level invariants
        validate_manifest(manifest)

        # 2. Load each referenced signature set
        sigsets = []
        manifest_paths = []
        for entry in manifest:
            sigset_file = entry["signatureSetFile"]
            manifest_paths.append(sigset_file)
            sigset = load_signature_set(args.sigdir, sigset_file)

            # 3. Validate the signature set
            validate_signature_set(sigset, entry["fileVersion"], sigset_file)

            # Cross-check: the manifest fileVersion should match the set's dll.fileVersion
            dll = sigset.get("dll", {})
            set_fv = dll.get("fileVersion")
            if set_fv is not None and set_fv != entry["fileVersion"]:
                fail("manifest fileVersion {!r} does not match signature set "
                     "dll.fileVersion {!r} in {}".format(
                         entry["fileVersion"], set_fv, sigset_file))

            sigsets.append((entry, sigset))

        # 4. Generate C++ code
        timestamp_str = get_timestamp()
        header_text = generate_header(manifest, sigsets, manifest_paths, timestamp_str)

        # 5. Write output
        out_dir = os.path.dirname(os.path.abspath(args.output))
        if out_dir and not os.path.isdir(out_dir):
            os.makedirs(out_dir, exist_ok=True)
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(header_text)

        print("sigcodegen.py: generated {} ({} manifest entries, {} signature sets)".format(
            args.output, len(manifest), len(sigsets)))
        return 0

    except CodegenError as e:
        sys.stderr.write("sigcodegen.py: ERROR: {}\n".format(e))
        return 1
    except Exception as e:  # pragma: no cover - defensive
        sys.stderr.write("sigcodegen.py: UNEXPECTED ERROR: {}: {}\n".format(
            type(e).__name__, e))
        return 2


if __name__ == "__main__":
    sys.exit(main())
