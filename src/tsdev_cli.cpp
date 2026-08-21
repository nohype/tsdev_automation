// tsdev_cli.cpp
//
// Production CLI for the TradeStation Development Environment library.
//
// Usage:
//   tsdev_cli [--json] [--ts-path <directory>] insert  <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] modify  <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] delete  <type> <name>
//   tsdev_cli [--json] [--ts-path <directory>] exists  <type> <name> [--case-insensitive]
//   tsdev_cli [--json] [--ts-path <directory>] get-source <type> <name> [--output <file>]
//   tsdev_cli [--json] [--ts-path <directory>] upsert <type> <name> <source_file>
//   tsdev_cli [--ts-path <directory>] batch-insert  <manifest_file>
//   tsdev_cli [--ts-path <directory>] batch-modify  <manifest_file>
//   tsdev_cli [--ts-path <directory>] batch-delete  <manifest_file>
//   tsdev_cli [--ts-path <directory>] batch-upsert  <manifest_file>
//   tsdev_cli [--ts-path <directory>] batch-insert-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--ts-path <directory>] batch-modify-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--ts-path <directory>] batch-delete-dir  <type> <dir_path> [-t <ext>...]
//   tsdev_cli [--ts-path <directory>] batch-upsert-dir  <type> <dir_path> [-t <ext>...]
//
//   <type>        "strategy", "function", "indicator", "tradingapp", or "showme" (case-insensitive)
//   <name>        component name
//   <source_file> path to a file containing EasyLanguage source (insert/modify)
//   <manifest_file> path to a line-based manifest (one item per line; '#'
//                 comments and blank lines are skipped). The op is fixed by the
//                 subcommand; each line carries only the per-item args:
//                   batch-insert/batch-modify/batch-upsert: <type> <name> <source_file>
//                   batch-delete:                              <type> <name>
//   --json        Global flag. When present, emit a single JSON object on stdout
//                 (followed by a newline) describing the operation result, and
//                 suppress the human-readable stdout messages. Exit codes are
//                 unchanged in both modes. When absent, output is byte-identical
//                 to the pre-JSON behavior (backward compatible).
//                 (batch-* subcommands ALWAYS emit a JSON array on stdout;
//                 --json is accepted but ignored for batch.)
//   --ts-path <directory>  Override the TradeStation install directory
//                          (default: auto-discovered from registry)
//
// The CLI wraps the shared tsdev_lib module (which wraps orlib20.dll). It:
//   1. Reads the source file
//   2. Constructs TsDevLib (loads orlib20, patches, inits)
//   3. Checks if the component already exists (fail loudly — no overwrite)
//   4. Creates the entry, saves source, compiles/verifies
//   5. Fails loudly on compilation errors (prints to stderr, exit non-zero)
//   6. Closes the warehouse and verifies the .ell file exists
//   7. Prints success with ProgId
//
// Exit codes: 0=success, 1=usage/arg error, 2=unknown type, 3=source file
// error, 4=init failure, 5=not-found (modify/delete) OR already-exists
// (insert), 6=entry op failure, 7=saveSource failure, 8=compile error,
// 9=.ell missing, 10=CreateThread failure.
//
// Batch subcommands (batch-insert/batch-modify/batch-delete) use a separate
// exit-code scheme (they are distinct subcommands, so no conflict):
//   0 = all items succeeded
//   1 = usage error / manifest file not found / manifest empty (batch did not run)
//   2 = batch ran but one or more items failed (per-item details in the JSON array)
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (archive/elevated_test20.cpp) runs its
// ENTIRE flow in a worker thread with a 64 MB stack. We do the same: all
// TsDevLib operations run on a 64 MB-stack worker thread.

#include "tsdev_lib.h"

#include <nlohmann/json.hpp>  // TVS-D.2: --json structured output

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <vector>
#include <filesystem>
#include <io.h>
#include <fcntl.h>
#include <atomic>      // TVS-K.4: std::atomic<bool> for serve quit flag
#include <sddl.h>      // TVS-K.4: ConvertStringSecurityDescriptorToSecurityDescriptorW

using namespace tsdev;
using ordered_json = nlohmann::ordered_json;  // preserves insertion order

namespace {

// --- Helpers ---

// Case-insensitive ASCII string comparison.
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
        [](char ca, char cb) {
            return std::tolower(static_cast<unsigned char>(ca)) ==
                   std::tolower(static_cast<unsigned char>(cb));
        });
}

// Parse the <type> argument into a ComponentType. Returns false on unknown.
bool parseType(const std::string& s, ComponentType& out) {
    if (iequals(s, "strategy")) { out = ComponentType::Strategy; return true; }
    if (iequals(s, "function")) { out = ComponentType::Function;  return true; }
    if (iequals(s, "indicator")) { out = ComponentType::Indicator; return true; }
    if (iequals(s, "tradingapp")) { out = ComponentType::TradingApp; return true; }
    if (iequals(s, "showme")) { out = ComponentType::ShowMe; return true; }
    return false;
}

// Read an entire file into a string. Returns false on failure.
bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Write a string to a file (binary mode). Returns false on failure.
bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// Elevation guard (TVS-K.1): query whether the current process is running
// with an elevated (administrator) token. Uses the Windows token-API
// sequence: OpenProcessToken -> GetTokenInformation(TokenElevation). On any
// API failure, returns false (fail safe: treat as not elevated).
bool isElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;  // fail safe: treat as not elevated
    TOKEN_ELEVATION elev = {};
    DWORD ret = 0;
    bool ok = GetTokenInformation(hToken, TokenElevation, &elev,
                                  sizeof(elev), &ret);
    CloseHandle(hToken);
    return ok && elev.TokenIsElevated != 0;
}

void printUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  tsdev_cli [--json] [--ts-path <directory>] insert  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] modify  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] delete  <type> <name>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] exists  <type> <name> "
        "[--case-insensitive]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] get-source <type> <name> "
        "[--output <file>]\n"
        "  tsdev_cli [--json] [--ts-path <directory>] upsert <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--ts-path <directory>] batch-insert  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--ts-path <directory>] batch-modify  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--ts-path <directory>] batch-delete  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--ts-path <directory>] batch-upsert  <manifest_file> | --batch-stdin\n"
        "  tsdev_cli [--ts-path <directory>] batch-insert-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--ts-path <directory>] batch-modify-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--ts-path <directory>] batch-delete-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--ts-path <directory>] batch-upsert-dir  <type> <dir_path> [-t <ext>...]\n"
        "  tsdev_cli [--ts-path <directory>] serve [--serve-pipe-file <path>]\n"
        "  <type>         strategy | function | indicator | tradingapp | showme (case-insensitive)\n"
        "  <name>         component name\n"
        "  <source_file>  path to a file containing EasyLanguage source "
        "(insert/modify)\n"
        "  <manifest_file> path to a line-based manifest (one item per line;\n"
        "                  '#' comments and blank lines are skipped). Lines:\n"
        "                  insert/modify/upsert: <type> <name> <source_file>\n"
        "                  delete:               <type> <name>\n"
        "  --batch-stdin    read the manifest from stdin instead of <manifest_file>\n"
        "                   (same line format; stdin is read in binary mode).\n"
        "  --json         Emit a single JSON result object on stdout (suppress "
        "human-readable stdout)\n"
        "  --ts-path <directory>  Override the TradeStation install directory "
        "(default: auto-discovered from registry)\n"
        "  --output <file>  (get-source only) Write the retrieved EasyLanguage "
        "source to <file>\n"
        "                   instead of stdout. Standard (non-password-protected) "
        "items only.\n"
        "  --case-insensitive  (exists only) Match the name case-insensitively.\n"
        "                      Without this flag, the name must match exactly.\n"
        "  -t <ext>       (batch-*-dir only) Filter files by extension. Repeatable.\n"
        "                 First -t overrides defaults (.el, .txt); subsequent -t flags add.\n"
        "                 Extensions are case-insensitive; dot prefix optional (el == .el).\n"
        "                 File stem (sans last extension) = item name.\n"
        "  --serve-pipe-file <path>  (serve only) Write the named pipe path and PID to\n"
        "                 <path> once the pipe is created (atomic: .tmp + rename).\n"
        "                 If absent, the pipe name is printed to stdout instead.\n"
        "  batch-* always emit a JSON array on stdout (one element per item).\n");
}

// --- TVS-D.2: structured result + output -------------------------------------

// CliResult captures everything needed to render the result in EITHER the
// human-readable form (existing, byte-identical) or the JSON form (new).
// Each run*/cmd* path populates exactly one CliResult; a single emitOutput()
// call then renders it. This avoids duplicating the operation logic across
// the two output modes and keeps the diff focused.
//
// Fields:
//   op            "insert"/"modify"/"delete" (empty when unknown, e.g. usage
//                 errors before the subcommand is fully resolved).
//   typeStr       the <type> argument verbatim (empty when unknown).
//   name          the component name (empty when unknown).
//   success       true iff the operation succeeded (exit 0).
//   exitCode      the exit code (unchanged in both modes).
//   progId        the ProgId, when known (success, and some error cases).
//   ellPath       the .ell file path (success insert/modify only; absent for
//                 delete success).
//   existingProgId  the ProgId of an already-existing entry (insert exit 5).
//   compileErrors the raw compiler error text (exit 8 only).
//   humanStdout   the EXACT human-readable text to write to stdout (success
//                 cases). Empty for error cases.
//   humanStderr   the EXACT human-readable text to write to stderr (error
//                 cases). Empty for success cases.
//
// The JSON error.message field is derived from a concise, machine-friendly
// message (errorMessage), distinct from the prefixed humanStderr line.
struct CliResult {
    std::string op;
    std::string typeStr;
    std::string name;
    bool        success = false;
    int         exitCode = 0;
    int         progId = 0;
    std::string ellPath;
    int         existingProgId = 0;
    std::string compileErrors;
    std::string humanStdout;
    std::string humanStderr;
    std::string errorMessage;  // concise message for JSON error.message
    bool        exists = false;  // "exists" op: whether the item was found
    std::string storedName;  // "exists" op: verbatim stored name (empty if not found)
    std::string sourceText;  // "get-source" op: retrieved EL source (JSON mode)
};

// Map an exit code to the machine-readable error.code string (TVS-D.2 §3).
// The `op` disambiguates exit 5 (insert -> already_exists, modify/delete ->
// not_found) and exit 3 (get-source -> output_write_error, else
// source_file_error).
std::string errorCodeString(int exitCode, const std::string& op) {
    switch (exitCode) {
        case 1:  return "usage_error";
        case 2:  return "unknown_type";
        case 3:  return (op == "get-source") ? "output_write_error" : "source_file_error";
        case 4:  return "init_failure";
        case 5:  return (op == "insert") ? "already_exists" : "not_found";
        case 6:  return "entry_op_failure";
        case 7:  return "save_source_failure";
        case 8:  return "compile_error";
        case 9:  return "ell_missing";
        case 10: return "thread_failure";
        case 11: return "not_elevated";
        default: return "unknown_error";
    }
}

// cliResultToJson serializes a CliResult to an ordered_json object using the
// EXACT schema shared by emitOutput()'s JSON mode and emitBatchOutput()'s
// per-result array entries (TVS-K.2). Extracted from emitOutput() so the
// serve layer (and both emit paths) share a single serialization function.
//
// op/type/name are included only when known (omitted for usage errors that
// occur before the subcommand/args are resolved). On success, prog_id is
// emitted only when > 0, ell_path only when non-empty. The "exists" op always
// emits exists + stored_name; the "get-source" op always emits source. On
// failure, an error object is emitted with code/message and op-specific
// fields (existing_prog_id for insert exit 5; prog_id + compile_errors for
// exit 8).
nlohmann::ordered_json cliResultToJson(const CliResult& r) {
    ordered_json j;
    // op/type/name are included only when known (omitted for usage errors
    // that occur before the subcommand/args are resolved).
    if (!r.op.empty())      j["op"] = r.op;
    if (!r.typeStr.empty()) j["type"] = r.typeStr;
    if (!r.name.empty())    j["name"] = r.name;
    j["success"]    = r.success;
    j["exit_code"]  = r.exitCode;

    if (r.success) {
        if (r.progId > 0)     j["prog_id"]  = r.progId;
        if (!r.ellPath.empty()) j["ell_path"] = r.ellPath;
        // "exists" op: emit the exists boolean (found vs not-found, both exit 0).
        if (r.op == "exists") {
            j["exists"] = r.exists;
            j["stored_name"] = r.storedName;
        }
        // "get-source" op: emit the retrieved EL source text.
        if (r.op == "get-source") j["source"] = r.sourceText;
    } else {
        ordered_json err;
        err["code"]    = errorCodeString(r.exitCode, r.op);
        err["message"] = r.errorMessage;
        // exit 5 (insert): include the existing ProgId.
        if (r.exitCode == 5 && r.op == "insert" && r.existingProgId > 0) {
            err["existing_prog_id"] = r.existingProgId;
        }
        // exit 8 (compile error): include the ProgId and raw compiler errors.
        if (r.exitCode == 8) {
            if (r.progId > 0) err["prog_id"] = r.progId;
            err["compile_errors"] = r.compileErrors;
        }
        j["error"] = err;
    }

    return j;
}

// emitOutput renders a CliResult in either human-readable or JSON form.
//
// Human mode (jsonMode == false): writes humanStdout to stdout and
// humanStderr to stderr exactly as captured (byte-identical to the
// pre-JSON behavior). At most one of the two is non-empty per result.
//
// JSON mode (jsonMode == true): emits exactly ONE JSON object on stdout
// followed by a single newline, and suppresses the human-readable stdout
// and stderr messages. The library's own stderr diagnostic
// ("TS install path resolved...") is emitted by tsdev_lib.cpp and is left
// untouched (it is a diagnostic, not a result). nlohmann/json performs all
// escaping (backslashes in Windows paths, quotes, newlines) automatically.
void emitOutput(const CliResult& r, bool jsonMode) {
    if (!jsonMode) {
        if (!r.humanStdout.empty()) {
            std::fwrite(r.humanStdout.c_str(), 1, r.humanStdout.size(), stdout);
            std::fflush(stdout);
        }
        if (!r.humanStderr.empty()) {
            std::fwrite(r.humanStderr.c_str(), 1, r.humanStderr.size(), stderr);
            std::fflush(stderr);
        }
        return;
    }

    ordered_json j = cliResultToJson(r);
    const std::string s = j.dump();
    std::fwrite(s.c_str(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// --- TVS-D.3: reusable per-op logic (shared by single-item + batch) --------
//
// doInsert/doModify/doDelete contain the core operation logic operating on an
// ALREADY-CONSTRUCTED TsDevLib instance. They do NOT construct or destruct the
// library (that is the caller's responsibility — single-item constructs per
// call, batch constructs once and reuses). This avoids duplicating the
// createEntry/saveSource/verifyModule/deleteEntry sequence.
//
// doInsert: when `flushPerItem` is true (single-item path), closeWarehouse() is
// called after verifyModule and before the .ell check — matching the original
// single-item sequence byte-for-byte. When false (batch path), closeWarehouse
// is skipped here; the batch worker calls it ONCE at the end of the batch.
CliResult doInsert(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   const std::string& source, bool flushPerItem) {
    CliResult r;
    r.op = "insert";
    r.typeStr = typeStr;
    r.name = name;

    // 1. Check if the component already exists — fail loudly, no overwrite.
    int existing = lib.findExistPos(name, type);
    if (existing > 0) {
        r.exitCode = 5;
        r.existingProgId = existing;
        r.errorMessage = typeStr + " \"" + name + "\" already exists (ProgId="
                         + std::to_string(existing) + "). Use modify instead.";
        r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                        + name + "\" already exists (ProgId=" + std::to_string(existing)
                        + "). Use modify instead.\n";
        return r;
    }

    // 2. Create the entry.
    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    if (progId <= 0) {
        r.exitCode = 6;
        r.progId = progId;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "0x%X", errCode);
        r.errorMessage = std::string("createEntry failed (ProgId=")
                         + std::to_string(progId) + ", errCode=" + buf + ")";
        r.humanStderr = std::string("tsdev_cli: createEntry failed (ProgId=")
                        + std::to_string(progId) + ", errCode=" + buf + ")\n";
        return r;
    }

    // 3. Save the source.
    if (!lib.saveSource(type, progId, source)) {
        r.exitCode = 7;
        r.progId = progId;
        r.errorMessage = std::string("saveSource failed for ProgId=")
                         + std::to_string(progId);
        r.humanStderr = std::string("tsdev_cli: saveSource failed for ProgId=")
                        + std::to_string(progId) + "\n";
        return r;
    }

    // 4. Compile / verify — fail loudly on errors.
    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        r.exitCode = 8;
        r.progId = progId;
        r.errorMessage = "compilation FAILED";
        r.compileErrors = errors;
        r.humanStderr = std::string("tsdev_cli: compilation FAILED for \"")
                        + name + "\" (ProgId=" + std::to_string(progId)
                        + "):\n" + errors + "\n";
        return r;
    }

    // 5. Close the warehouse (flush) — single-item only; batch flushes once
    //    at the end.
    if (flushPerItem) {
        lib.closeWarehouse();
    }

    // 6. Verify the .ell file exists.
    std::string ellPath = lib.getEllPath(type, progId);
    if (!lib.ellFileExists(type, progId)) {
        r.exitCode = 9;
        r.progId = progId;
        r.errorMessage = std::string("compilation succeeded but .ell file not found: ")
                         + ellPath;
        r.humanStderr = std::string("tsdev_cli: compilation succeeded but .ell file not found: ")
                        + ellPath + "\n";
        return r;
    }

    // 7. Success.
    r.success = true;
    r.exitCode = 0;
    r.progId = progId;
    r.ellPath = ellPath;
    r.humanStdout = std::string("SUCCESS: ") + typeStr + " \"" + name
                    + "\" inserted, ProgId=" + std::to_string(progId)
                    + ", .ell=" + ellPath + "\n";
    return r;
}

// doModify: reuses the library's modifyEntry (findExistPos -> saveSource ->
// verifyModule on the existing ProgId). The ProgId is preserved (no
// createEntry). Does not call closeWarehouse (the original single-item path
// did not either — persistence happens via OrlibExit in the destructor, or
// via the batch worker's single closeWarehouse at the end).
CliResult doModify(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   const std::string& source) {
    CliResult r;
    r.op = "modify";
    r.typeStr = typeStr;
    r.name = name;

    Result res = lib.modifyEntry(type, name, source);

    if (!res.success) {
        if (res.message == "component not found") {
            r.exitCode = 5;
            r.errorMessage = typeStr + " \"" + name
                             + "\" not found in the warehouse.";
            r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                            + name + "\" not found in the warehouse. "
                            "Use insert to create it.\n";
        } else {
            r.exitCode = 8;
            r.progId = res.progId;
            r.errorMessage = "compilation FAILED";
            r.compileErrors = res.message;
            r.humanStderr = std::string("tsdev_cli: modification FAILED for \"")
                            + name + "\" (ProgId=" + std::to_string(res.progId)
                            + "):\n" + res.message + "\n";
        }
        return r;
    }

    std::string ellPath = lib.getEllPath(type, res.progId);
    r.success = true;
    r.exitCode = 0;
    r.progId = res.progId;
    r.ellPath = ellPath;
    r.humanStdout = std::string("SUCCESS: ") + typeStr + " \"" + name
                    + "\" modified, ProgId=" + std::to_string(res.progId)
                    + ", .ell=" + ellPath + "\n";
    return r;
}

// doDelete: findExistPos (distinct not-found message) -> deleteEntry. When
// flushPerItem is true (single-item path), deleteEntry calls closeWarehouse()
// internally as part of its flow. When false (serve/batch path), the
// closeWarehouse is skipped — the warehouse stays open for the session
// lifetime, and runServe/runBatch calls closeWarehouse() once at the end.
// This is critical: closing the warehouse after delete frees the IWHouse
// interface, causing subsequent enumerateRecords/refreshCache calls to crash.
CliResult doDelete(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   bool flushPerItem = true) {
    CliResult r;
    r.op = "delete";
    r.typeStr = typeStr;
    r.name = name;

    int progId = lib.findExistPos(name, type);
    if (progId <= 0) {
        r.exitCode = 5;
        r.errorMessage = typeStr + " \"" + name
                         + "\" not found in the warehouse.";
        r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                        + name + "\" not found in the warehouse.\n";
        return r;
    }

    bool deleted = lib.deleteEntry(type, name, flushPerItem);
    if (!deleted) {
        r.exitCode = 6;
        r.progId = progId;
        r.errorMessage = std::string("deleteEntry failed for ") + typeStr
                         + " \"" + name + "\" (ProgId="
                         + std::to_string(progId) + ").";
        r.humanStderr = std::string("tsdev_cli: deleteEntry failed for ")
                        + typeStr + " \"" + name + "\" (ProgId="
                        + std::to_string(progId) + ").\n";
        return r;
    }

    r.success = true;
    r.exitCode = 0;
    r.progId = progId;
    r.humanStdout = std::string("SUCCESS: ") + typeStr + " \"" + name
                    + "\" deleted (ProgId=" + std::to_string(progId)
                    + " was removed from warehouse, "
                    ".ell/.xml files removed from disk).\n";
    return r;
}

// doExists: query whether a component exists by name + type. This is a
// QUERY, not a mutation: not-found is a valid result (NOT an error). Both
// found and not-found return exit 0, distinguished by the `exists` field.
//
// When caseInsensitive is true, uses enumerateRecords (case-insensitive
// search) and reports a match regardless of name casing. The verbatim
// stored name is returned via stored_name.
//
// When caseInsensitive is false (default), uses enumerateRecords to find
// the item case-insensitively (since findExistPos is inherently
// case-insensitive), then post-compares the stored name with the query
// name using exact (case-sensitive) comparison. Only reports exists=1
// when the names match exactly.
CliResult doExists(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   bool caseInsensitive) {
    CliResult r;
    r.op = "exists";
    r.typeStr = typeStr;
    r.name = name;

    EnumerateResult er = lib.enumerateRecords(type, name);
    if (er.exists) {
        if (caseInsensitive || er.storedName == name) {
            r.success = true;
            r.exitCode = 0;
            r.exists = true;
            r.progId = er.progId;
            r.storedName = er.storedName;
            r.humanStdout = "exists=1\nstored_name=" + er.storedName
                            + "\nprogId=" + std::to_string(er.progId) + "\n";
        } else {
            r.success = true;
            r.exitCode = 0;
            r.exists = false;
            r.progId = 0;
            r.storedName = "";
            r.humanStdout = "exists=0\nstored_name=\nprogId=0\n";
        }
    } else {
        r.success = true;
        r.exitCode = 0;
        r.exists = false;
        r.progId = 0;
        r.storedName = "";
        r.humanStdout = "exists=0\nstored_name=\nprogId=0\n";
    }
    return r;
}

// doUpsert: insert-or-modify by name + type + source. Calls findExistPos
// FIRST, then branches — avoids the overhead of a failed insert attempt.
// When the item exists (ProgId > 0), delegates to doModify (preserves the
// ProgId); otherwise delegates to doInsert with the caller-supplied
// flushPerItem (true for single-item, false for batch) (assigns a
// new ProgId). The op field is overridden to "upsert" after delegation
// (doInsert/doModify set their own op internally). Upsert never emits exit 5
// (not_found / already_exists) — it branches on existence, so neither
// condition is possible by construction.
CliResult doUpsert(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name,
                   const std::string& source, bool flushPerItem = true) {
    int progId = lib.findExistPos(name, type);
    CliResult r;
    if (progId > 0) {
        r = doModify(lib, type, typeStr, name, source);
    } else {
        r = doInsert(lib, type, typeStr, name, source, flushPerItem);
    }
    r.op = "upsert";
    return r;
}

// Parameters passed to the worker thread.
struct InsertParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runInsert
};

// The INSERT flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runInsert(LPVOID param) {
    InsertParams* p = static_cast<InsertParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "insert";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }

    p->result = doInsert(*lib, p->type, p->typeStr, p->name, p->source,
                         /*flushPerItem=*/true);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

// --- The INSERT subcommand (parses args, validates, launches worker) ---
int cmdInsert(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride,
              bool jsonMode) {
    CliResult r;
    r.op = "insert";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        r.exitCode = 3;
        r.errorMessage = std::string("failed to read source file \"") + sourceFile + "\"";
        r.humanStderr = std::string("tsdev_cli: failed to read source file \"")
                        + sourceFile + "\"\n";
        emitOutput(r, jsonMode);
        return 3;
    }
    if (source.empty()) {
        r.exitCode = 3;
        r.errorMessage = "source file is empty";
        r.humanStderr = "tsdev_cli: source file is empty\n";
        emitOutput(r, jsonMode);
        return 3;
    }

    // Run the orlib flow on a 64 MB-stack worker thread (the compiler needs
    // a large stack and the EDIT control is thread-affine).
    InsertParams params;
    params.type    = type;
    params.typeStr = typeStr;
    params.name    = name;
    params.source  = source;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runInsert, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The MODIFY subcommand ---

// Parameters passed to the worker thread.
struct ModifyParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runModify
};

// The MODIFY flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runModify(LPVOID param) {
    ModifyParams* p = static_cast<ModifyParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "modify";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }

    p->result = doModify(*lib, p->type, p->typeStr, p->name, p->source);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdModify(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride,
              bool jsonMode) {
    CliResult r;
    r.op = "modify";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        r.exitCode = 3;
        r.errorMessage = std::string("failed to read source file \"") + sourceFile + "\"";
        r.humanStderr = std::string("tsdev_cli: failed to read source file \"")
                        + sourceFile + "\"\n";
        emitOutput(r, jsonMode);
        return 3;
    }
    if (source.empty()) {
        r.exitCode = 3;
        r.errorMessage = "source file is empty";
        r.humanStderr = "tsdev_cli: source file is empty\n";
        emitOutput(r, jsonMode);
        return 3;
    }

    // Run the orlib flow on a 64 MB-stack worker thread (the compiler needs
    // a large stack and the EDIT control is thread-affine).
    ModifyParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.source   = source;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runModify, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The DELETE subcommand ---

// Parameters passed to the worker thread.
struct DeleteParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runDelete
};

// The DELETE flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runDelete(LPVOID param) {
    DeleteParams* p = static_cast<DeleteParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "delete";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }

    p->result = doDelete(*lib, p->type, p->typeStr, p->name);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdDelete(const std::string& typeStr, const std::string& name,
              const wchar_t* tsPathOverride, bool jsonMode) {
    CliResult r;
    r.op = "delete";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    DeleteParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDelete, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The EXISTS subcommand ---

// Parameters passed to the worker thread.
struct ExistsParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    bool           caseInsensitive;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runExists
};

// The EXISTS flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runExists(LPVOID param) {
    ExistsParams* p = static_cast<ExistsParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "exists";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }

    p->result = doExists(*lib, p->type, p->typeStr, p->name, p->caseInsensitive);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdExists(const std::string& typeStr, const std::string& name,
              bool caseInsensitive,
              const wchar_t* tsPathOverride, bool jsonMode) {
    CliResult r;
    r.op = "exists";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    ExistsParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.caseInsensitive = caseInsensitive;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runExists, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The UPSERT subcommand ---

// Parameters passed to the worker thread.
struct UpsertParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runUpsert
};

// The UPSERT flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runUpsert(LPVOID param) {
    UpsertParams* p = static_cast<UpsertParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "upsert";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }

    p->result = doUpsert(*lib, p->type, p->typeStr, p->name, p->source);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdUpsert(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride,
              bool jsonMode) {
    CliResult r;
    r.op = "upsert";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        r.exitCode = 3;
        r.errorMessage = std::string("failed to read source file \"") + sourceFile + "\"";
        r.humanStderr = std::string("tsdev_cli: failed to read source file \"")
                        + sourceFile + "\"\n";
        emitOutput(r, jsonMode);
        return 3;
    }
    if (source.empty()) {
        r.exitCode = 3;
        r.errorMessage = "source file is empty";
        r.humanStderr = "tsdev_cli: source file is empty\n";
        emitOutput(r, jsonMode);
        return 3;
    }

    // Run the orlib flow on a 64 MB-stack worker thread (the compiler needs
    // a large stack and the EDIT control is thread-affine).
    UpsertParams params;
    params.type    = type;
    params.typeStr = typeStr;
    params.name    = name;
    params.source  = source;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runUpsert, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- The GET-SOURCE subcommand ---

// doGetSource: retrieve the EasyLanguage source text for an existing item by
// name + type. getSource returns the source string; an EMPTY string means the
// item was not found OR the source is unavailable (e.g. password-protected).
// We cannot distinguish these cases from the empty string, so all failures
// map to exit 5 / not_found. When an --output file is requested, the source
// is written to that file (on the worker thread); a write failure maps to
// exit 3 / output_write_error.
CliResult doGetSource(TsDevLib& lib, ComponentType type,
                      const std::string& typeStr, const std::string& name,
                      const std::string& outputPath) {
    CliResult r;
    r.op = "get-source";
    r.typeStr = typeStr;
    r.name = name;

    std::string source = lib.getSource(type, name);
    if (source.empty()) {
        r.exitCode = 5;
        r.errorMessage = typeStr + " \"" + name
                         + "\" not found or source unavailable "
                           "(may be password-protected).";
        r.humanStderr = std::string("tsdev_cli: ERROR — ") + typeStr + " \""
                        + name + "\" not found or source unavailable "
                          "(may be password-protected).\n";
        return r;
    }

    // Source retrieved successfully.
    r.success = true;
    r.exitCode = 0;
    r.sourceText = source;

    if (!outputPath.empty()) {
        // Write the source to the requested file.
        if (!writeFile(outputPath, source)) {
            r.success = false;
            r.exitCode = 3;
            r.sourceText.clear();
            r.errorMessage = std::string("failed to write source to output file \"")
                             + outputPath + "\"";
            r.humanStderr = std::string("tsdev_cli: failed to write source to "
                            "output file \"") + outputPath + "\"\n";
            return r;
        }
        // File written — human-readable stdout reports the destination, NOT
        // the source text. JSON mode still carries the source in `source`.
        r.humanStdout = std::string("Source written to ") + outputPath + "\n";
    } else {
        // stdout mode: emit the raw source text directly (no wrapper).
        r.humanStdout = source;
    }
    return r;
}

// Parameters passed to the worker thread.
struct GetSourceParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    outputPath;  // empty = stdout mode
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    CliResult      result;          // populated by runGetSource
};

// The GET-SOURCE flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runGetSource(LPVOID param) {
    GetSourceParams* p = static_cast<GetSourceParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        CliResult& r = p->result;
        r.op = "get-source";
        r.typeStr = p->typeStr;
        r.name = p->name;
        r.exitCode = 4;
        r.errorMessage = std::string("init failed: ") + e.what();
        r.humanStderr = std::string("tsdev_cli: init failed: ") + e.what() + "\n";
        return 0;
    }

    p->result = doGetSource(*lib, p->type, p->typeStr, p->name, p->outputPath);

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdGetSource(const std::string& typeStr, const std::string& name,
                 const std::string& outputPath,
                 const wchar_t* tsPathOverride, bool jsonMode) {
    CliResult r;
    r.op = "get-source";
    r.typeStr = typeStr;
    r.name = name;

    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.exitCode = 2;
        r.errorMessage = std::string("unknown type \"") + typeStr
                         + "\" (expected strategy/function/indicator/tradingapp/showme)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp/showme)\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    if (name.empty()) {
        r.exitCode = 2;
        r.errorMessage = "component name must not be empty";
        r.humanStderr = "tsdev_cli: component name must not be empty\n";
        emitOutput(r, jsonMode);
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    GetSourceParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.outputPath = outputPath;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runGetSource, &params, 0, nullptr);
    if (!hThread) {
        r.exitCode = 10;
        r.errorMessage = std::string("CreateThread failed: ")
                         + std::to_string(GetLastError());
        r.humanStderr = std::string("tsdev_cli: CreateThread failed: ")
                        + std::to_string(GetLastError()) + "\n";
        emitOutput(r, jsonMode);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    emitOutput(params.result, jsonMode);
    return params.result.exitCode;
}

// --- TVS-D.3: batch mode ----------------------------------------------------
//
// Three batch subcommands mirror the single-item subcommands but process a
// manifest file containing one item per line:
//   batch-insert <manifest_file>   lines: <type> <name> <source_file>
//   batch-modify <manifest_file>   lines: <type> <name> <source_file>
//   batch-delete <manifest_file>   lines: <type> <name>
//
// ONE TsDevLib instance is constructed on a single 64 MB-stack worker thread
// and reused for ALL items (amortizing the ~411ms init). Output is ALWAYS a
// JSON array on stdout (one element per real manifest item, in order). Exit
// codes: 0 = all succeeded, 1 = usage/manifest error (batch did not run),
// 2 = batch ran but one or more items failed.

// A single parsed manifest item. The op (insert/modify/delete) is fixed by
// the subcommand; each item carries only the per-item args.
struct BatchItem {
    ComponentType type;
    std::string   typeStr;     // verbatim <type> token (for JSON output)
    std::string   name;
    std::string   sourceFile;  // insert/modify only (empty for delete)
    std::string   source;      // pre-read source text (insert/modify only)
    bool          preValid = true;   // false = malformed line / unreadable file
    CliResult     result;      // pre-populated for preValid=false items
};

// trim from both ends (whitespace only).
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Parse manifest content (a single string with newline-separated lines) into
// BatchItems. Used by both the file-path mode (parseManifest) and the
// --batch-stdin mode. Always succeeds (parsing a string cannot fail to open).
void parseManifestLines(const std::string& content, bool needsSource,
                        std::vector<BatchItem>& outItems) {
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // Strip a trailing '\r' (manifest may use CRLF).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trim(line);
        // Skip blank lines and comments.
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;

        BatchItem item;

        // Token 1: <type> (first whitespace-delimited token).
        size_t sp1 = trimmed.find_first_of(" \t");
        std::string typeTok;
        std::string rest1;
        if (sp1 == std::string::npos) {
            typeTok = trimmed;
            rest1 = "";
        } else {
            typeTok = trimmed.substr(0, sp1);
            rest1 = trim(trimmed.substr(sp1 + 1));
        }

        // Token 2: <name> (first whitespace-delimited token of the remainder).
        size_t sp2 = rest1.find_first_of(" \t");
        std::string nameTok;
        std::string rest2;
        if (sp2 == std::string::npos) {
            nameTok = rest1;
            rest2 = "";
        } else {
            nameTok = rest1.substr(0, sp2);
            rest2 = trim(rest1.substr(sp2 + 1));
        }

        item.typeStr = typeTok;
        item.name = nameTok;

        // Validate <type>.
        ComponentType ctype;
        if (!parseType(typeTok, ctype)) {
            item.preValid = false;
            item.result.op = "";  // op set by caller (cmdBatch*) after parse
            item.result.typeStr = typeTok;
            item.result.name = nameTok;
            item.result.exitCode = 2;
            item.result.errorMessage = std::string("unknown type \"") + typeTok
                + "\" (expected strategy/function/indicator/tradingapp/showme)";
            outItems.push_back(std::move(item));
            continue;
        }
        item.type = ctype;

        // Validate <name>.
        if (nameTok.empty()) {
            item.preValid = false;
            item.result.typeStr = typeTok;
            item.result.exitCode = 2;
            item.result.errorMessage = "component name must not be empty";
            outItems.push_back(std::move(item));
            continue;
        }

        if (needsSource) {
            // <source_file> = the REST of the line after the second token.
            if (rest2.empty()) {
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.name = nameTok;
                item.result.exitCode = 1;
                item.result.errorMessage =
                    "manifest line missing <source_file>: \"" + trimmed + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            item.sourceFile = rest2;
            // Read the source file now (binary mode, matching single-item).
            if (!readFile(rest2, item.source)) {
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.name = nameTok;
                item.result.exitCode = 3;
                item.result.errorMessage =
                    std::string("failed to read source file \"") + rest2 + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
            if (item.source.empty()) {
                item.preValid = false;
                item.result.typeStr = typeTok;
                item.result.name = nameTok;
                item.result.exitCode = 3;
                item.result.errorMessage = "source file is empty";
                outItems.push_back(std::move(item));
                continue;
            }
        }

        item.preValid = true;
        outItems.push_back(std::move(item));
    }
}

// Parse a manifest file into a list of BatchItems. Lines starting with '#'
// (comments) and blank/whitespace-only lines are skipped. Malformed lines
// (wrong token count, unknown type, empty name) and unreadable source files
// (insert/modify) are recorded as pre-validated FAILURE items so the batch
// continues and reports them. Returns false only if the manifest file itself
// cannot be opened (caller reports exit 1).
//
// needsSource: true for batch-insert/batch-modify (3 tokens: type name file),
//              false for batch-delete (2 tokens: type name).
bool parseManifest(const std::string& path, bool needsSource,
                   std::vector<BatchItem>& outItems) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    parseManifestLines(content, needsSource, outItems);
    return true;
}

// Parameters passed to the batch worker thread.
struct BatchParams {
    std::string             op;  // "insert"/"modify"/"delete"
    std::vector<BatchItem>  items;
    const wchar_t*          tsPathOverride;  // nullptr = auto-discover
    std::vector<CliResult>  results;  // populated by runBatch
    int                     exitCode = 0;   // 0/2 (set by worker)
};

// The batch worker: constructs ONE TsDevLib, loops over items IN ORDER using
// the shared doInsert/doModify/doDelete logic, collects results, calls
// closeWarehouse() once at the end, then destructs TsDevLib. Continue on
// failure (never abort) — collect all results.
DWORD WINAPI runBatch(LPVOID param) {
    BatchParams* p = static_cast<BatchParams*>(param);
    const std::string& op = p->op;
    p->results.clear();
    p->results.reserve(p->items.size());

    // Construct ONE TsDevLib instance (all one-time init happens once).
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        // Init failure: every item gets an init_failure result.
        for (auto& item : p->items) {
            CliResult r;
            r.op = op;
            r.typeStr = item.typeStr;
            r.name = item.name;
            r.exitCode = 4;
            r.errorMessage = std::string("init failed: ") + e.what();
            p->results.push_back(std::move(r));
        }
        p->exitCode = 2;  // batch ran (attempted) but all failed
        return 0;
    }

    bool anyFailed = false;

    for (auto& item : p->items) {
        CliResult r;
        if (!item.preValid) {
            // Pre-validated failure (malformed line / unreadable file) —
            // carry over the result populated during manifest parsing, just
            // stamp the op.
            r = item.result;
            r.op = op;
            anyFailed = true;
            p->results.push_back(std::move(r));
            continue;
        }

        if (op == "insert") {
            r = doInsert(*lib, item.type, item.typeStr, item.name,
                         item.source, /*flushPerItem=*/false);
        } else if (op == "modify") {
            r = doModify(*lib, item.type, item.typeStr, item.name, item.source);
        } else if (op == "upsert") {
            r = doUpsert(*lib, item.type, item.typeStr, item.name,
                         item.source, /*flushPerItem=*/false);
        } else {  // delete
            r = doDelete(*lib, item.type, item.typeStr, item.name,
                         /*flushPerItem=*/false);
        }

        if (!r.success) anyFailed = true;
        p->results.push_back(std::move(r));
    }

    // Flush the warehouse ONCE at the end of the batch (per the TsDevLib
    // header note: "safe to call once at the end of a batch").
    lib->closeWarehouse();

    // Destruct TsDevLib (OrlibExit persists; EDIT control destroyed on this
    // thread).
    delete lib;

    p->exitCode = anyFailed ? 2 : 0;
    return 0;
}

// Serialize a vector of CliResult to a single JSON array on stdout (followed
// by one newline). Reuses the same per-result schema as emitOutput's JSON
// mode (TVS-D.2). stdout contains ONLY the JSON array; the library's stderr
// diagnostic stays on stderr.
void emitBatchOutput(const std::vector<CliResult>& results) {
    ordered_json arr = ordered_json::array();
    for (const CliResult& r : results) {
        arr.push_back(cliResultToJson(r));
    }

    const std::string s = arr.dump();
    std::fwrite(s.c_str(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Shared batch execution + emit helper: sets up BatchParams, launches the
// worker thread (64 MB stack), runs runBatch, emits the JSON array, and
// returns the batch exit code. Used by both cmdBatch() (manifest mode) and
// cmdBatchDir() (directory mode) so the batch execution logic is not
// duplicated.
int runBatchAndEmit(const std::string& op, std::vector<BatchItem>& items,
                    const wchar_t* tsPathOverride) {
    // Launch the batch on a 64 MB-stack worker thread (ONE TsDevLib instance).
    BatchParams params;
    params.op = op;
    params.items = std::move(items);
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runBatch, &params, 0, nullptr);
    if (!hThread) {
        // Thread creation failure -> exit 1 (batch did not run). stderr only.
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        return 1;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    // Emit the JSON array (always, for batch mode that ran).
    emitBatchOutput(params.results);
    return params.exitCode;
}

// Common batch command handler: parses the manifest, launches the worker,
// emits the JSON array, returns the batch exit code.
int cmdBatch(const std::string& op, const std::string& manifestFile,
             bool useStdin, const wchar_t* tsPathOverride) {
    const bool needsSource = (op == "insert" || op == "modify" || op == "upsert");

    // Parse the manifest on the main thread (before the worker).
    std::vector<BatchItem> items;
    if (useStdin) {
        // Read the manifest from stdin in BINARY mode (avoids CRLF/newline
        // translation on Windows). EOF / empty stdin -> empty manifest.
        _setmode(_fileno(stdin), _O_BINARY);
        std::string content;
        char buf[8192];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
            content.append(buf, n);
        }
        parseManifestLines(content, needsSource, items);
        if (items.empty()) {
            // No JSON array on stdout (the batch did not run); stderr only.
            std::fprintf(stderr, "tsdev_cli: manifest from stdin is empty\n");
            return 1;
        }
    } else {
        if (!parseManifest(manifestFile, needsSource, items)) {
            // Manifest file not found / unreadable -> exit 1 (batch did not run).
            // No JSON array on stdout (the batch did not run); stderr only.
            std::fprintf(stderr, "tsdev_cli: failed to open manifest file \"%s\"\n",
                         manifestFile.c_str());
            return 1;
        }
        if (items.empty()) {
            // Manifest contained no real items (only comments/blanks) -> exit 1.
            // No JSON array on stdout (the batch did not run); stderr only.
            std::fprintf(stderr, "tsdev_cli: manifest \"%s\" is empty\n",
                         manifestFile.c_str());
            return 1;
        }
    }

    return runBatchAndEmit(op, items, tsPathOverride);
}

// --- TVS-I.2: directory batch mode -------------------------------------------
//
// parseDirectory: enumerate regular files in dirPath using
// std::filesystem::directory_iterator (NOT recursive — subdirectories are
// skipped via is_regular_file()). Filter by accepted extensions: if
// `extensions` is empty, use defaults {".el", ".txt"}; if non-empty, use only
// the specified extensions. Extension matching is case-insensitive (normalize
// by lowercasing both the file extension and the accepted extension). Each
// accepted extension is normalized: prepend '.' if missing and non-empty (so
// "el" -> ".el"; "" stays "" to match extensionless files). For each matching
// file: BatchItem with type = the type int, typeStr = the type string,
// name = file stem (path::stem() strips only the last extension,
// so my.strategy.el -> my.strategy), sourceFile = the file path string,
// source = readFile(path) if needsSource is true, else empty string. The
// directory_iterator construction is wrapped in try/catch for
// filesystem_error — on error, emit stderr and return false (caller emits
// exit 1 without a second message). Returns true on success (outItems may
// still be empty if no files matched). Items are sorted by name for
// deterministic ordering.
bool parseDirectory(int type, const std::string& typeStr, const std::string& dirPath,
                    const std::vector<std::string>& extensions, bool needsSource,
                    std::vector<BatchItem>& outItems) {
    // Build the normalized accepted-extensions set. If empty, use defaults.
    std::vector<std::string> acceptedExts;
    if (extensions.empty()) {
        acceptedExts = {".el", ".txt"};
    } else {
        for (const std::string& ext : extensions) {
            std::string norm = ext;
            // Prepend '.' if missing and non-empty (so "el" -> ".el"; an
            // empty string stays empty to match extensionless files).
            if (!norm.empty() && norm[0] != '.') norm = "." + norm;
            // Lowercase for case-insensitive comparison.
            std::transform(norm.begin(), norm.end(), norm.begin(),
                [](char c) { return static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))); });
            acceptedExts.push_back(norm);
        }
    }

    // Enumerate regular files in dirPath (non-recursive).
    std::filesystem::directory_iterator it;
    try {
        it = std::filesystem::directory_iterator(dirPath);
    } catch (const std::filesystem::filesystem_error&) {
        std::fprintf(stderr, "tsdev_cli: failed to open directory \"%s\"\n",
                     dirPath.c_str());
        return false;
    }

    for (const auto& entry : it) {
        // Skip subdirectories — only regular files.
        if (!std::filesystem::is_regular_file(entry.status())) continue;

        const std::filesystem::path& p = entry.path();
        // Extract the file extension (includes the leading dot; empty if none).
        std::string fileExt = p.extension().string();
        // Lowercase for case-insensitive comparison.
        std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(),
            [](char c) { return static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))); });

        bool matches = false;
        for (const std::string& acc : acceptedExts) {
            if (fileExt == acc) { matches = true; break; }
        }
        if (!matches) continue;

        BatchItem item;
        item.type = static_cast<ComponentType>(type);
        item.typeStr = typeStr;
        item.name = p.stem().string();
        item.sourceFile = p.string();
        if (needsSource) {
            if (!readFile(item.sourceFile, item.source)) {
                // Mirror parseManifestLines: read failure -> preValid false,
                // exit 3, source_file_error. The batch continues and reports
                // this item as a failure.
                item.preValid = false;
                item.result.typeStr = typeStr;
                item.result.name = item.name;
                item.result.exitCode = 3;
                item.result.errorMessage =
                    std::string("failed to read source file \"") + item.sourceFile + "\"";
                outItems.push_back(std::move(item));
                continue;
            }
        }
        item.preValid = true;
        outItems.push_back(std::move(item));
    }

    // Sort by name for deterministic ordering.
    std::sort(outItems.begin(), outItems.end(),
        [](const BatchItem& a, const BatchItem& b) { return a.name < b.name; });
    return true;
}

// cmdBatchDir: directory batch command handler. Validates the type, parses
// the directory into BatchItems via parseDirectory(), then calls the shared
// runBatchAndEmit() helper. If parseDirectory returns false (directory could
// not be opened), the "failed to open directory" message was already emitted
// by parseDirectory — return exit 1 without a second message. If it returns
// true but yielded zero items (empty dir, no matching files), emit the
// "no files matching" message and return exit 1 (no JSON array emitted).
int cmdBatchDir(const std::string& op, int type, const std::string& typeStr,
                const std::string& dirPath, const std::vector<std::string>& extensions,
                const wchar_t* tsPath) {
    // Validate type (unknown -> exit 2, matching existing pattern).
    ComponentType ctype;
    if (!parseType(typeStr, ctype)) {
        std::fprintf(stderr, "tsdev_cli: unknown type \"%s\" "
                     "(expected strategy/function/indicator/tradingapp/showme)\n",
                     typeStr.c_str());
        return 2;
    }

    const bool needsSource = (op == "insert" || op == "modify" || op == "upsert");

    std::vector<BatchItem> items;
    if (!parseDirectory(type, typeStr, dirPath, extensions, needsSource, items)) {
        // Directory open failed — parseDirectory already emitted the
        // "failed to open directory" stderr message. Exit 1, no JSON array.
        return 1;
    }

    if (items.empty()) {
        // No matching files (empty dir or no matches). Exit 1, stderr only,
        // no JSON array emitted. Build the extensions description for the
        // message.
        std::string extDesc;
        if (extensions.empty()) {
            extDesc = ".el,.txt";
        } else {
            for (size_t i = 0; i < extensions.size(); ++i) {
                if (i > 0) extDesc += ",";
                std::string norm = extensions[i];
                if (!norm.empty() && norm[0] != '.') norm = "." + norm;
                extDesc += norm;
            }
        }
        std::fprintf(stderr, "tsdev_cli: no files matching %s found in %s\n",
                     extDesc.c_str(), dirPath.c_str());
        return 1;
    }

    return runBatchAndEmit(op, items, tsPath);
}

// --- TVS-K.3: serve request dispatch -----------------------------------------
//
// The serve layer takes a parsed JSON request (from the named-pipe broker in
// TVS-K.4), validates it, and dispatches to the corresponding do* function on
// a shared TsDevLib. This is the pure logic layer with NO pipe I/O — fully
// unit-testable (parseServeRequest is pure; dispatchServeRequest needs a real
// TsDevLib and is validated via E2E in K.6a).

// ServeRequest is the parsed, validated form of a single broker request.
//   op            insert/modify/delete/exists/upsert/get-source/quit
//   type          ComponentType (resolved from typeStr)
//   typeStr       the original type string verbatim (for CliResult)
//   name          component name
//   source        raw EasyLanguage source TEXT (not a file path — the Python
//                 wrapper reads files client-side and sends the text)
//   caseInsensitive  exists only (optional, default false)
//   outputPath    get-source only (optional; broker-local file path)
struct ServeRequest {
    std::string op;
    ComponentType type = ComponentType::Strategy;
    std::string typeStr;
    std::string name;
    std::string source;
    bool caseInsensitive = false;
    std::string outputPath;
};

// parseServeRequest validates a JSON request and populates `out`. On success
// returns a CliResult with success=true, exitCode=0. On a malformed request
// returns a CliResult with success=false and the appropriate exit code:
//   exit 1 (usage_error): missing 'op', unknown op, missing type/name/source.
//   exit 2 (unknown_type): unknown type string.
// The error messages contain the relevant keyword (op/type/name/source) so
// callers can surface them directly. See the error-handling table in
// docs/workstream_serve_plan.md (lines 530-544).
CliResult parseServeRequest(const ordered_json& req, ServeRequest& out) {
    CliResult r;

    // 1. 'op' is required.
    if (!req.contains("op") || !req["op"].is_string()) {
        r.success = false;
        r.exitCode = 1;
        r.op = "";
        r.errorMessage = "missing 'op' field";
        return r;
    }
    std::string op = req["op"].get<std::string>();

    // 2. Validate the op against the known set.
    const std::string knownOps[] = {
        "insert", "modify", "delete", "exists", "upsert", "get-source", "quit"
    };
    bool opKnown = false;
    for (const std::string& k : knownOps) {
        if (op == k) { opKnown = true; break; }
    }
    if (!opKnown) {
        r.success = false;
        r.exitCode = 1;
        r.op = "";
        r.errorMessage = "unknown op '" + op + "'";
        return r;
    }

    // 3. 'quit' needs no other fields.
    if (op == "quit") {
        out.op = "quit";
        r.success = true;
        r.exitCode = 0;
        r.op = "quit";
        return r;
    }

    // 4. All other ops require 'type' and 'name'.
    if (!req.contains("type") || !req["type"].is_string() ||
        !req.contains("name") || !req["name"].is_string()) {
        r.success = false;
        r.exitCode = 1;
        r.op = op;
        r.errorMessage = "op '" + op + "' requires 'type' and 'name'";
        return r;
    }
    std::string typeStr = req["type"].get<std::string>();
    std::string name = req["name"].get<std::string>();

    // 5. Resolve the type string (case-insensitive, same as CLI).
    ComponentType type;
    if (!parseType(typeStr, type)) {
        r.success = false;
        r.exitCode = 2;
        r.op = op;
        r.typeStr = typeStr;
        r.errorMessage = "unknown type '" + typeStr + "'";
        return r;
    }

    // 6. insert/modify/upsert require 'source' (raw EL text, not a path).
    if (op == "insert" || op == "modify" || op == "upsert") {
        if (!req.contains("source") || !req["source"].is_string()) {
            r.success = false;
            r.exitCode = 1;
            r.op = op;
            r.typeStr = typeStr;
            r.name = name;
            r.errorMessage = "op '" + op + "' requires 'source'";
            return r;
        }
    }

    // 7. Populate the output struct.
    out.op = op;
    out.type = type;
    out.typeStr = typeStr;
    out.name = name;
    if (op == "insert" || op == "modify" || op == "upsert") {
        out.source = req["source"].get<std::string>();
    }
    if (op == "exists") {
        if (req.contains("case_insensitive") && req["case_insensitive"].is_boolean()) {
            out.caseInsensitive = req["case_insensitive"].get<bool>();
        } else {
            out.caseInsensitive = false;
        }
    }
    if (op == "get-source") {
        if (req.contains("output_path") && req["output_path"].is_string()) {
            out.outputPath = req["output_path"].get<std::string>();
        } else {
            out.outputPath.clear();
        }
    }

    r.success = true;
    r.exitCode = 0;
    r.op = op;
    r.typeStr = typeStr;
    r.name = name;
    return r;
}

// dispatchServeRequest switches on req.op and calls the corresponding do*
// function on the shared TsDevLib. Mutating ops use flushPerItem=false (the
// batch pattern): no per-op closeWarehouse(). The warehouse stays open for
// the entire serve session, and runServe calls closeWarehouse() once at the
// end (before destructing TsDevLib). This is critical because closing the
// warehouse after each op would cause subsequent ops (especially
// exists/get-source which need the warehouse open) to crash with
// ACCESS_VIOLATION in orlib20. Persistence is guaranteed by OrlibExit (in
// the TsDevLib destructor) and the explicit closeWarehouse() in runServe.
// Read-only ops (exists/get-source) need no flush. "quit" returns a success
// CliResult with op="quit" (the broker layer in K.4 handles the actual
// shutdown).
//
// NOTE: not unit-tested (needs a real, concrete TsDevLib). Validated via E2E
// in K.6a.
CliResult dispatchServeRequest(TsDevLib& lib, const ServeRequest& req) {
    if (req.op == "insert") {
        return doInsert(lib, req.type, req.typeStr, req.name,
                        req.source, /*flushPerItem=*/false);
    }
    if (req.op == "modify") {
        return doModify(lib, req.type, req.typeStr, req.name, req.source);
    }
    if (req.op == "delete") {
        return doDelete(lib, req.type, req.typeStr, req.name,
                        /*flushPerItem=*/false);
    }
    if (req.op == "upsert") {
        return doUpsert(lib, req.type, req.typeStr, req.name, req.source,
                        /*flushPerItem=*/false);
    }
    if (req.op == "exists") {
        return doExists(lib, req.type, req.typeStr, req.name, req.caseInsensitive);
    }
    if (req.op == "get-source") {
        return doGetSource(lib, req.type, req.typeStr, req.name, req.outputPath);
    }
    if (req.op == "quit") {
        CliResult r;
        r.op = "quit";
        r.success = true;
        r.exitCode = 0;
        return r;
    }

    // Unknown op — should not reach here (parseServeRequest rejects unknown
    // ops), but defend in depth with a usage error.
    CliResult r;
    r.success = false;
    r.exitCode = 1;
    r.errorMessage = "unknown op '" + req.op + "'";
    return r;
}

// --- TVS-K.4: named pipe server ----------------------------------------------
//
// Framing helpers, pipe creation, and the serve loop. The framing layer is
// unit-tested in tests/test_serve_framing.cpp (this .cpp is #included with
// TSDEV_CLI_TEST defined, so the helpers in this anonymous namespace are
// visible to the test). The serve loop + cmdServe are validated via E2E in
// K.6a (they need a real TsDevLib and an elevated broker).

// 64 MiB cap for framed messages (per protocol spec §2). Messages whose
// declared length exceeds this are rejected by readFramed without allocating.
static constexpr size_t kServeFrameCap = 67108864;

// writeAll loops WriteFile until all `len` bytes are written. Returns false on
// error or a zero-byte write (broken pipe). Handles partial writes, which
// named pipes can perform even in PIPE_TYPE_BYTE mode.
bool writeAll(HANDLE h, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        DWORD wrote = 0;
        if (!WriteFile(h, static_cast<const char*>(buf) + total,
                       static_cast<DWORD>(len - total), &wrote, nullptr))
            return false;
        if (wrote == 0) return false;  // broken pipe / no progress
        total += wrote;
    }
    return true;
}

// readAll loops ReadFile until all `len` bytes are read. Returns false on
// error or EOF (a zero-byte read before completion indicates a closed pipe).
bool readAll(HANDLE h, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        DWORD got = 0;
        if (!ReadFile(h, static_cast<char*>(buf) + total,
                      static_cast<DWORD>(len - total), &got, nullptr))
            return false;
        if (got == 0) return false;  // EOF / broken pipe
        total += got;
    }
    return true;
}

// writeFramed writes a 4-byte little-endian length prefix followed by the
// UTF-8 payload. Returns true on success, false on a broken pipe / write
// error (the caller closes the connection).
bool writeFramed(HANDLE pipe, const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    if (!writeAll(pipe, &len, 4)) return false;
    if (len > 0 && !writeAll(pipe, payload.data(), len)) return false;
    return true;
}

// readFramed reads a 4-byte little-endian length prefix, rejects lengths
// above the 64 MiB cap, then reads the payload into `out`. Returns true on
// success, false on a broken pipe / EOF / oversized length / short read.
bool readFramed(HANDLE pipe, std::string& out) {
    uint32_t len = 0;
    if (!readAll(pipe, &len, 4)) return false;
    if (len > kServeFrameCap) return false;
    out.resize(len);
    if (len > 0 && !readAll(pipe, out.data(), len)) return false;
    return true;
}

// createServePipe creates the named pipe with the security descriptor from
// protocol spec §3 (Medium-IL + owner/admins/system DACL). Returns the pipe
// handle, or INVALID_HANDLE_VALUE on failure. The pipe is a single-instance,
// duplex, byte-mode, blocking pipe.
HANDLE createServePipe(const std::string& pipeName) {
    // SDDL: O:<userSid> sets the owner to the actual user (not Administrators,
    //   which is the default owner when the broker runs elevated).
    // D:P = protected DACL (no inherited ACEs).
    //   (A;;GA;;;BA)           Administrators: full control.
    //   (A;;GWGR;;;SY)         LOCAL SYSTEM: read/write.
    //   (A;;GWGR;;;<userSid>)  The current user: read/write.
    //   S:(ML;;NW;;;ME)        SACL mandatory label: Medium, No-Write-Up.
    //
    // Why the user's SID instead of the SDDL "OW" (Owner) well-known SID:
    // When the broker runs elevated, the default owner of objects it creates is
    // BUILTIN\Administrators, so "OW" resolves to Administrators. An unelevated
    // client running as the same user has the Administrators group as "deny
    // only" in its token and therefore CANNOT match an OW/Administrators ACE,
    // resulting in ERROR_ACCESS_DENIED (5). Using the user's real SID (and
    // setting it as the owner via O:) makes the ACE match the unelevated client
    // regardless of elevation level.
    std::wstring userSidStr;
    {
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            DWORD bufLen = 0;
            GetTokenInformation(hToken, TokenUser, nullptr, 0, &bufLen);
            std::vector<BYTE> buf(bufLen);
            if (bufLen > 0 && GetTokenInformation(hToken, TokenUser, buf.data(),
                                                  bufLen, &bufLen)) {
                const TOKEN_USER* tu =
                    reinterpret_cast<const TOKEN_USER*>(buf.data());
                wchar_t* sidStr = nullptr;
                if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                    userSidStr = sidStr;
                    LocalFree(sidStr);
                }
            }
            CloseHandle(hToken);
        }
    }

    std::wstring sddl;
    if (userSidStr.empty()) {
        // Fallback: SID retrieval failed — use the old OW-based SDDL.
        // The unelevated client may fail to connect (OW resolves to
        // Administrators when elevated, and unelevated tokens have
        // Administrators as deny-only). Emit a diagnostic so this is
        // diagnosable if it ever occurs.
        std::fprintf(stderr, "tsdev_cli: WARNING — failed to retrieve user SID "
                             "for pipe security descriptor; falling back to OW-based "
                             "SDDL (unelevated clients may be unable to connect)\n");
        sddl = L"D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;OW)S:(ML;;NW;;;ME)";
    } else {
        sddl = L"O:" + userSidStr +
               L"D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;" + userSidStr +
               L")S:(ML;;NW;;;ME)";
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &pSD, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = pSD;

    // Convert the narrow pipe name to wide (UTF-8; pipe names are ASCII here
    // but UTF-8 is safe and forward-compatible).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(),
                                   static_cast<int>(pipeName.size()),
                                   nullptr, 0);
    std::wstring wname(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(),
                        static_cast<int>(pipeName.size()),
                        &wname[0], wlen);

    HANDLE hPipe = CreateNamedPipeW(
        wname.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,   // nMaxInstances (single instance; MVP is single-broker)
        0,   // nOutBufferSize (default)
        0,   // nInBufferSize (default)
        0,   // nDefaultTimeOut (default)
        &sa);

    LocalFree(pSD);  // the SD was copied into the pipe; free our copy
    return hPipe;
}

// Parameters passed to the serve worker thread.
struct ServeParams {
    HANDLE             pipe = INVALID_HANDLE_VALUE;
    const wchar_t*     tsPathOverride = nullptr;  // nullptr = auto-discover
    std::atomic<bool>  quit{false};               // set by worker on 'quit' op
    int                exitCode = 0;              // set by worker
};

// runServe is the 64MB-stack worker thread entry point. It constructs ONE
// TsDevLib (lives for the whole serve session, like runBatch), then loops:
// ConnectNamedPipe -> inner loop (readFramed -> parseServeRequest ->
// dispatchServeRequest -> cliResultToJson -> writeFramed) ->
// FlushFileBuffers + DisconnectNamedPipe. The 'quit' op sets the quit flag
// and breaks both loops. TsDevLib is constructed AND used on this same
// thread (thread-affine constraint: the hidden EDIT control is thread-local).
DWORD WINAPI runServe(LPVOID param) {
    ServeParams* p = static_cast<ServeParams*>(param);
    HANDLE pipe = p->pipe;

    // Construct ONE TsDevLib instance (all one-time init happens once).
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tsdev_cli: serve init failed: %s\n", e.what());
        p->exitCode = 4;
        return 0;
    }

    // Outer loop: accept connections until the 'quit' op (or a fatal
    // ConnectNamedPipe failure).
    while (!p->quit.load()) {
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            // ConnectNamedPipe failed unexpectedly — stop the broker.
            break;
        }

        // Inner loop: read framed requests, dispatch, write framed responses.
        while (!p->quit.load()) {
            std::string reqStr;
            if (!readFramed(pipe, reqStr)) {
                break;  // broken pipe / EOF / oversized -> close connection
            }

            // Parse the JSON request. Malformed JSON -> usage error response.
            CliResult r;
            ordered_json reqJson;
            try {
                reqJson = ordered_json::parse(reqStr);
            } catch (const std::exception&) {
                r.success = false;
                r.exitCode = 1;
                r.errorMessage = "malformed JSON request";
                if (!writeFramed(pipe, cliResultToJson(r).dump())) break;
                continue;
            }

            // Validate + parse the request into a ServeRequest.
            ServeRequest req;
            r = parseServeRequest(reqJson, req);
            if (!r.success) {
                if (!writeFramed(pipe, cliResultToJson(r).dump())) break;
                continue;
            }

            // 'quit' op: respond with the quit success result, then signal
            // shutdown and break the inner loop.
            if (req.op == "quit") {
                CliResult qr = dispatchServeRequest(*lib, req);
                writeFramed(pipe, cliResultToJson(qr).dump());
                p->quit.store(true);
                break;
            }

            // Dispatch on THIS thread (TsDevLib is thread-affine to this
            // 64MB-stack worker). Catch unexpected exceptions and respond
            // with an entry_op_failure result instead of crashing.
            try {
                r = dispatchServeRequest(*lib, req);
            } catch (const std::exception& e) {
                r = CliResult();
                r.op = req.op;
                r.typeStr = req.typeStr;
                r.name = req.name;
                r.success = false;
                r.exitCode = 6;
                r.errorMessage = e.what();
            }
            if (!writeFramed(pipe, cliResultToJson(r).dump())) break;
        }

        // Flush any buffered output, then disconnect so the next client can
        // connect (the pipe handle is reused across connections).
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
    }

    // Flush the warehouse once at the end of the serve session, then destruct
    // TsDevLib (OrlibExit persists; EDIT control destroyed on this thread).
    lib->closeWarehouse();
    delete lib;
    return 0;
}

// cmdServe is the 'serve' subcommand handler. It parses --serve-pipe-file
// (optional), creates the named pipe with the security descriptor, writes
// the pipe name + PID to the pipe file (atomic: .tmp + MoveFileEx) or prints
// the pipe name to stdout, launches runServe on a 64MB-stack thread, and
// waits for it to exit.
int cmdServe(int argc, char** argv, bool jsonMode,
             const wchar_t* tsPathOverride) {
    // Parse --serve-pipe-file <path> (optional; survives the global pre-scan
    // because it is serve-specific, like --case-insensitive/--output).
    std::string pipeFile;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--serve-pipe-file") {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "tsdev_cli: --serve-pipe-file requires a file path\n");
                return 1;
            }
            pipeFile = argv[i + 1];
            i++;  // skip the value
        } else if (a.rfind("--serve-pipe-file=", 0) == 0) {
            pipeFile = a.substr(std::strlen("--serve-pipe-file="));
            if (pipeFile.empty()) {
                std::fprintf(stderr,
                    "tsdev_cli: --serve-pipe-file requires a non-empty path\n");
                return 1;
            }
        } else {
            std::fprintf(stderr,
                "tsdev_cli: unknown serve argument \"%s\"\n", a.c_str());
            return 1;
        }
    }

    // Build the pipe name: \\.\pipe\tsdev_cli_<PID> (PID for uniqueness).
    DWORD pid = GetCurrentProcessId();
    std::string pipeName = "\\\\.\\pipe\\tsdev_cli_" + std::to_string(pid);

    HANDLE hPipe = createServePipe(pipeName);
    if (hPipe == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
            "tsdev_cli: failed to create named pipe \"%s\" (err=%lu)\n",
            pipeName.c_str(), GetLastError());
        return 1;
    }

    // Publish the pipe name. With --serve-pipe-file: write the full pipe name
    // (line 1) and the broker PID (line 2) atomically (write to <path>.tmp,
    // then MoveFileEx with REPLACE_EXISTING). Without it: print the pipe name
    // to stdout for manual use.
    if (!pipeFile.empty()) {
        std::string content = pipeName + "\n" + std::to_string(pid) + "\n";
        std::string tmp = pipeFile + ".tmp";
        if (!writeFile(tmp, content) ||
            !MoveFileExA(tmp.c_str(), pipeFile.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            std::fprintf(stderr,
                "tsdev_cli: failed to write pipe file \"%s\" (err=%lu)\n",
                pipeFile.c_str(), GetLastError());
            CloseHandle(hPipe);
            return 1;
        }
    } else {
        std::fprintf(stdout, "%s\n", pipeName.c_str());
        std::fflush(stdout);
    }

    // Launch runServe on a 64MB-stack worker thread (ONE TsDevLib instance,
    // constructed and used on that thread).
    ServeParams params;
    params.pipe = hPipe;
    params.tsPathOverride = tsPathOverride;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runServe, &params, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        CloseHandle(hPipe);
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(hPipe);
    return params.exitCode;
}

// Vectored Exception Handler — catches fatal exceptions (access violations,
// stack overflows, heap corruption) BEFORE orlib20's unhandled exception
// filter can spawn TSCrashReport.exe. Calls TerminateProcess with the
// exception code so the process exits cleanly with a known exit code
// and NO crash dialog. This is critical for serve mode: when the elevated
// broker crashes, TSCrashReport.exe would run elevated and a non-elevated
// Python client cannot kill it.
LONG WINAPI vehCrashHandler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Only handle truly fatal exceptions — don't intercept normal C++ exceptions
    // (which use a different mechanism) or debug breakpoints.
    if (code == EXCEPTION_ACCESS_VIOLATION ||       // 0xC0000005
        code == EXCEPTION_STACK_OVERFLOW ||          // 0xC00000FD
        code == STATUS_STACK_BUFFER_OVERRUN ||       // 0xC0000409
        code == 0xC0000374) {                        // STATUS_HEAP_CORRUPTION (ntstatus.h not included due to winnt.h conflicts)
        TerminateProcess(GetCurrentProcess(), code);
        // TerminateProcess doesn't return, but just in case:
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

#ifndef TSDEV_CLI_TEST
int main(int argc, char** argv) {
    // Install VEH crash handler — must be before any orlib20 calls.
    // Catches fatal exceptions and terminates cleanly instead of spawning
    // TSCrashReport.exe (which runs elevated and can't be killed by non-elevated clients).
    AddVectoredExceptionHandler(1, vehCrashHandler);
    // Also suppress WerFault.exe as belt-and-suspenders (won't suppress TSCrashReport
    // but prevents the standard Windows crash dialog too).
    SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);

    // --- Global flag pre-scan: --ts-path and --json ---
    // Scan argv[1..argc-1] for the optional global flags and remove them
    // (and, for --ts-path, their value) from the argv array. The resulting
    // reduced argv contains only the subcommand and its positional args, so
    // the existing exact-argc subcommand dispatch below works unchanged.
    //
    // --ts-path: space-separated ("--ts-path <path>") or equals
    //            ("--ts-path=<path>") form. If given multiple times, the
    //            LAST occurrence wins.
    // --json:    boolean switch. Presence (bare "--json" or "--json=...")
    //            enables JSON output. Takes no value.
    const char* tsPathOverride = nullptr;
    std::string tsPathStr;  // holds the value for the equals form
    bool jsonMode = false;

    std::vector<char*> reducedArgv;
    reducedArgv.push_back(argv[0]);  // program name

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--ts-path") {
            // Space-separated form: value is the next arg.
            if (i + 1 >= argc) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "--ts-path requires a directory path";
                r.humanStderr = "Error: --ts-path requires a directory path\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            if (argv[i + 1][0] == '\0') {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "--ts-path requires a non-empty directory path";
                r.humanStderr = "Error: --ts-path requires a non-empty directory path\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            tsPathOverride = argv[i + 1];
            i++;  // skip the value
        } else if (arg.rfind("--ts-path=", 0) == 0) {
            // Equals form: value is everything after the '='.
            tsPathStr = arg.substr(std::strlen("--ts-path="));
            if (tsPathStr.empty()) {
                CliResult r;
                r.exitCode = 1;
                r.errorMessage = "--ts-path requires a non-empty directory path";
                r.humanStderr = "Error: --ts-path requires a non-empty directory path\n";
                emitOutput(r, jsonMode);
                return 1;
            }
            tsPathOverride = tsPathStr.c_str();
        } else if (arg == "--json" || arg.rfind("--json=", 0) == 0) {
            // Boolean switch: presence enables JSON output. Any value
            // after '=' is ignored (presence == true).
            jsonMode = true;
        } else {
            reducedArgv.push_back(argv[i]);
        }
    }

    int reducedArgc = static_cast<int>(reducedArgv.size());
    char** reducedArgvPtr = reducedArgv.data();

    if (reducedArgc < 2) {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = "no subcommand given";
        r.humanStderr = "";  // printUsage writes directly below
        if (jsonMode) {
            emitOutput(r, jsonMode);
        } else {
            printUsage();
        }
        return 1;
    }

    std::string subcommand = reducedArgvPtr[1];

    // Convert the narrow tsPathOverride to wide for the TsDevLib constructor.
    // The existing toWide() helper lives in an anonymous namespace in
    // tsdev_lib.cpp (file-local, not accessible here), so the conversion is
    // performed inline using MultiByteToWideChar (the same Windows API that
    // toWide() wraps) — no duplicate named helper is introduced.
    std::wstring tsPathWide;
    const wchar_t* tsPathWidePtr = nullptr;
    if (tsPathOverride) {
        int wlen = MultiByteToWideChar(CP_ACP, 0, tsPathOverride,
            static_cast<int>(std::strlen(tsPathOverride)),
            nullptr, 0);
        tsPathWide.resize(wlen);
        MultiByteToWideChar(CP_ACP, 0, tsPathOverride,
            static_cast<int>(std::strlen(tsPathOverride)),
            &tsPathWide[0], wlen);
        tsPathWidePtr = tsPathWide.c_str();
    }

    // Elevation guard (TVS-K.1): all subcommands require administrator
    // privileges. Placed AFTER the global flag pre-scan (so --json/--ts-path
    // are honored and the error can be emitted in JSON form) but BEFORE
    // subcommand dispatch. Excluded under TSDEV_CLI_TEST (unit tests include
    // this .cpp and define their own main()).
    if (!isElevated()) {
        if (jsonMode) {
            nlohmann::ordered_json j;
            j["success"] = false;
            j["exit_code"] = 11;
            j["error"]["code"] = "not_elevated";
            j["error"]["message"] = "this tool requires administrator privileges. "
                                    "Run from an elevated terminal (or let the Python wrapper auto-elevate).";
            std::string s = j.dump();
            std::fwrite(s.c_str(), 1, s.size(), stdout);
            std::fputc('\n', stdout);
        } else {
            std::fprintf(stderr, "tsdev_cli: ERROR — this tool requires administrator privileges. "
                                 "Run from an elevated terminal (or let the Python wrapper auto-elevate).\n");
        }
        return 11;
    }

    // --- TVS-K.4: serve subcommand ---
    // serve runs a long-lived named-pipe broker: it creates the pipe, launches
    // runServe on a 64MB-stack worker thread, and waits. It takes an optional
    // --serve-pipe-file <path> (parsed inside cmdServe). Placed before the
    // other subcommands so it is dispatched first.
    if (iequals(subcommand, "serve")) {
        return cmdServe(reducedArgc, reducedArgvPtr, jsonMode, tsPathWidePtr);
    }

    if (iequals(subcommand, "insert")) {
        if (reducedArgc != 5) {
            CliResult r;
            r.op = "insert";
            r.exitCode = 1;
            r.errorMessage = "insert requires exactly 3 arguments: <type> <name> <source_file>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdInsert(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr, jsonMode);
    }

    if (iequals(subcommand, "modify")) {
        if (reducedArgc != 5) {
            CliResult r;
            r.op = "modify";
            r.exitCode = 1;
            r.errorMessage = "modify requires exactly 3 arguments: <type> <name> <source_file>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdModify(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr, jsonMode);
    }

    if (iequals(subcommand, "delete")) {
        if (reducedArgc != 4) {
            CliResult r;
            r.op = "delete";
            r.exitCode = 1;
            r.errorMessage = "delete requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdDelete(reducedArgvPtr[2], reducedArgvPtr[3],
                         tsPathWidePtr, jsonMode);
    }

    if (iequals(subcommand, "exists")) {
        // exists takes <type> <name> plus an optional --case-insensitive flag.
        // --case-insensitive is exists-specific (not a global flag), so it
        // remains in reducedArgv and is parsed here. Valid forms:
        //   exists <type> <name>                        (reducedArgc == 4)
        //   exists <type> <name> --case-insensitive     (reducedArgc == 5)
        bool caseInsensitive = false;
        std::vector<char*> exArgs;
        for (int i = 2; i < reducedArgc; i++) {
            std::string a = reducedArgvPtr[i];
            if (a == "--case-insensitive") {
                caseInsensitive = true;
            } else {
                exArgs.push_back(reducedArgvPtr[i]);
            }
        }
        int exArgc = static_cast<int>(exArgs.size());
        if (exArgc != 2) {
            CliResult r;
            r.op = "exists";
            r.exitCode = 1;
            r.errorMessage = "exists requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdExists(exArgs[0], exArgs[1], caseInsensitive,
                         tsPathWidePtr, jsonMode);
    }

    if (iequals(subcommand, "upsert")) {
        if (reducedArgc != 5) {
            CliResult r;
            r.op = "upsert";
            r.exitCode = 1;
            r.errorMessage = "upsert requires exactly 3 arguments: <type> <name> <source_file>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdUpsert(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr, jsonMode);
    }

    if (iequals(subcommand, "get-source")) {
        // get-source takes <type> <name> plus an optional --output <file>.
        // --output is get-source-specific (not a global flag), so it remains
        // in reducedArgv and is parsed here. Valid forms:
        //   get-source <type> <name>                      (reducedArgc == 4)
        //   get-source <type> <name> --output <file>      (reducedArgc == 6)
        std::string outputPath;

        // Scan for --output and extract it (and its value), counting the
        // remaining positional args.
        std::vector<char*> gsArgs;
        for (int i = 2; i < reducedArgc; i++) {
            std::string a = reducedArgvPtr[i];
            if (a == "--output") {
                if (i + 1 >= reducedArgc) {
                    CliResult r;
                    r.op = "get-source";
                    r.exitCode = 1;
                    r.errorMessage = "--output requires a file path";
                    r.humanStderr = "tsdev_cli: --output requires a file path\n";
                    if (jsonMode) {
                        emitOutput(r, jsonMode);
                    } else {
                        printUsage();
                    }
                    return 1;
                }
                outputPath = reducedArgvPtr[i + 1];
                i++;  // skip the value
            } else if (a.rfind("--output=", 0) == 0) {
                outputPath = a.substr(std::strlen("--output="));
                if (outputPath.empty()) {
                    CliResult r;
                    r.op = "get-source";
                    r.exitCode = 1;
                    r.errorMessage = "--output requires a non-empty file path";
                    r.humanStderr = "tsdev_cli: --output requires a non-empty file path\n";
                    if (jsonMode) {
                        emitOutput(r, jsonMode);
                    } else {
                        printUsage();
                    }
                    return 1;
                }
            } else {
                gsArgs.push_back(reducedArgvPtr[i]);
            }
        }

        if (gsArgs.size() != 2) {
            CliResult r;
            r.op = "get-source";
            r.exitCode = 1;
            r.errorMessage = "get-source requires exactly 2 arguments: <type> <name>";
            if (jsonMode) {
                emitOutput(r, jsonMode);
            } else {
                printUsage();
            }
            return 1;
        }
        return cmdGetSource(gsArgs[0], gsArgs[1], outputPath,
                            tsPathWidePtr, jsonMode);
    }

    // --- TVS-D.3: batch subcommands ---
    // batch-insert/batch-modify/batch-delete take a single <manifest_file>
    // argument OR --batch-stdin (read the manifest from stdin instead). They
    // ALWAYS emit a JSON array on stdout (no --json needed). --json is accepted
    // but ignored (the global pre-scan already handled it).
    if (iequals(subcommand, "batch-insert") ||
        iequals(subcommand, "batch-modify") ||
        iequals(subcommand, "batch-delete") ||
        iequals(subcommand, "batch-upsert")) {
        if (reducedArgc != 3) {
            std::string opName = subcommand;
            std::fprintf(stderr,
                "tsdev_cli: %s requires exactly 1 argument: <manifest_file> or --batch-stdin\n",
                opName.c_str());
            return 1;
        }
        std::string op;
        if (iequals(subcommand, "batch-insert"))      op = "insert";
        else if (iequals(subcommand, "batch-modify")) op = "modify";
        else if (iequals(subcommand, "batch-upsert")) op = "upsert";
        else                                          op = "delete";
        std::string arg2 = reducedArgvPtr[2];
        if (arg2 == "--batch-stdin") {
            return cmdBatch(op, std::string(), true, tsPathWidePtr);
        }
        return cmdBatch(op, arg2, false, tsPathWidePtr);
    }

    // --- TVS-I.2: directory batch subcommands ---
    // batch-insert-dir/batch-modify-dir/batch-delete-dir/batch-upsert-dir take
    // <type> <dir_path> plus an optional repeatable -t <ext> flag. They ALWAYS
    // emit a JSON array on stdout (no --json needed). --json is accepted but
    // ignored (the global pre-scan already handled it). The -t flag is
    // directory-batch-specific: it survives the global pre-scan and is parsed
    // here (similar to --case-insensitive in the exists dispatch).
    if (iequals(subcommand, "batch-insert-dir") ||
        iequals(subcommand, "batch-modify-dir") ||
        iequals(subcommand, "batch-delete-dir") ||
        iequals(subcommand, "batch-upsert-dir")) {
        std::string op;
        if (iequals(subcommand, "batch-insert-dir"))      op = "insert";
        else if (iequals(subcommand, "batch-modify-dir")) op = "modify";
        else if (iequals(subcommand, "batch-upsert-dir")) op = "upsert";
        else                                               op = "delete";

        // Scan reducedArgv for -t <ext> pairs. Extract the extension argument,
        // add to the extensions vector, and remove both tokens from the arg
        // list. The remaining args should be: program + subcommand + type + dir
        // (argc == 4).
        std::vector<std::string> extensions;
        std::vector<char*> dirArgs;
        dirArgs.push_back(reducedArgvPtr[0]);  // program name
        dirArgs.push_back(reducedArgvPtr[1]);  // subcommand
        for (int i = 2; i < reducedArgc; i++) {
            std::string a = reducedArgvPtr[i];
            if (a == "-t") {
                // Space-separated form: the next token is the extension.
                if (i + 1 >= reducedArgc) {
                    std::fprintf(stderr,
                        "tsdev_cli: -t requires an extension argument\n");
                    return 1;
                }
                extensions.push_back(reducedArgvPtr[i + 1]);
                i++;  // skip the extension argument
            } else {
                dirArgs.push_back(reducedArgvPtr[i]);
            }
        }
        int dirArgc = static_cast<int>(dirArgs.size());
        if (dirArgc != 4) {
            std::string opName = subcommand;
            std::fprintf(stderr,
                "tsdev_cli: %s requires exactly 2 arguments: <type> <dir_path> "
                "[-t <ext>...]\n",
                opName.c_str());
            return 1;
        }
        std::string typeStr = dirArgs[2];
        std::string dirPath = dirArgs[3];

        // Resolve the type int for parseDirectory.
        ComponentType ctype;
        if (!parseType(typeStr, ctype)) {
            std::fprintf(stderr, "tsdev_cli: unknown type \"%s\" "
                         "(expected strategy/function/indicator/tradingapp/showme)\n",
                         typeStr.c_str());
            return 2;
        }
        int typeInt = static_cast<int>(ctype);

        return cmdBatchDir(op, typeInt, typeStr, dirPath, extensions,
                           tsPathWidePtr);
    }

    {
        CliResult r;
        r.exitCode = 1;
        r.errorMessage = std::string("unknown subcommand \"") + subcommand + "\"";
        r.humanStderr = std::string("tsdev_cli: unknown subcommand \"")
                        + subcommand + "\"\n";
        if (jsonMode) {
            emitOutput(r, jsonMode);
        } else {
            std::fprintf(stderr, "tsdev_cli: unknown subcommand \"%s\"\n",
                         subcommand.c_str());
            printUsage();
        }
        return 1;
    }
}
#endif  // TSDEV_CLI_TEST
