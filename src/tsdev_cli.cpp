// tsdev_cli.cpp
//
// Production CLI for the TradeStation Development Environment library.
//
// Usage:
//   tsdev_cli [--json] [--ts-path <directory>] insert  <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] modify  <type> <name> <source_file>
//   tsdev_cli [--json] [--ts-path <directory>] delete  <type> <name>
//   tsdev_cli [--ts-path <directory>] batch-insert  <manifest_file>
//   tsdev_cli [--ts-path <directory>] batch-modify  <manifest_file>
//   tsdev_cli [--ts-path <directory>] batch-delete  <manifest_file>
//
//   <type>        "strategy", "function", "indicator", or "tradingapp" (case-insensitive)
//   <name>        component name
//   <source_file> path to a file containing EasyLanguage source (insert/modify)
//   <manifest_file> path to a line-based manifest (one item per line; '#'
//                 comments and blank lines are skipped). The op is fixed by the
//                 subcommand; each line carries only the per-item args:
//                   batch-insert/batch-modify: <type> <name> <source_file>
//                   batch-delete:              <type> <name>
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

void printUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  tsdev_cli [--json] [--ts-path <directory>] insert  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] modify  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--json] [--ts-path <directory>] delete  <type> <name>\n"
        "  tsdev_cli [--ts-path <directory>] batch-insert  <manifest_file>\n"
        "  tsdev_cli [--ts-path <directory>] batch-modify  <manifest_file>\n"
        "  tsdev_cli [--ts-path <directory>] batch-delete  <manifest_file>\n"
        "  <type>         strategy | function | indicator | tradingapp (case-insensitive)\n"
        "  <name>         component name\n"
        "  <source_file>  path to a file containing EasyLanguage source "
        "(insert/modify)\n"
        "  <manifest_file> path to a line-based manifest (one item per line;\n"
        "                  '#' comments and blank lines are skipped). Lines:\n"
        "                  insert/modify: <type> <name> <source_file>\n"
        "                  delete:        <type> <name>\n"
        "  --json         Emit a single JSON result object on stdout (suppress "
        "human-readable stdout)\n"
        "  --ts-path <directory>  Override the TradeStation install directory "
        "(default: auto-discovered from registry)\n"
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
};

// Map an exit code to the machine-readable error.code string (TVS-D.2 §3).
// The `op` disambiguates exit 5 (insert -> already_exists, modify/delete ->
// not_found).
std::string errorCodeString(int exitCode, const std::string& op) {
    switch (exitCode) {
        case 1:  return "usage_error";
        case 2:  return "unknown_type";
        case 3:  return "source_file_error";
        case 4:  return "init_failure";
        case 5:  return (op == "insert") ? "already_exists" : "not_found";
        case 6:  return "entry_op_failure";
        case 7:  return "save_source_failure";
        case 8:  return "compile_error";
        case 9:  return "ell_missing";
        case 10: return "thread_failure";
        default: return "unknown_error";
    }
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

// doDelete: findExistPos (distinct not-found message) -> deleteEntry. Does
// not call closeWarehouse (deleteEntry calls it internally as part of its
// flow; the batch worker's single closeWarehouse at the end is a no-op flush).
CliResult doDelete(TsDevLib& lib, ComponentType type,
                   const std::string& typeStr, const std::string& name) {
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

    bool deleted = lib.deleteEntry(type, name);
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
                         + "\" (expected strategy/function/indicator/tradingapp)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp)\n";
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
                         + "\" (expected strategy/function/indicator/tradingapp)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp)\n";
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
                         + "\" (expected strategy/function/indicator/tradingapp)";
        r.humanStderr = std::string("tsdev_cli: unknown type \"") + typeStr
                        + "\" (expected strategy/function/indicator/tradingapp)\n";
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

    std::string line;
    while (std::getline(f, line)) {
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
                + "\" (expected strategy/function/indicator/tradingapp)";
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
        } else {  // delete
            r = doDelete(*lib, item.type, item.typeStr, item.name);
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
        ordered_json j;
        if (!r.op.empty())      j["op"] = r.op;
        if (!r.typeStr.empty()) j["type"] = r.typeStr;
        if (!r.name.empty())    j["name"] = r.name;
        j["success"]    = r.success;
        j["exit_code"]  = r.exitCode;

        if (r.success) {
            if (r.progId > 0)       j["prog_id"]  = r.progId;
            if (!r.ellPath.empty()) j["ell_path"] = r.ellPath;
        } else {
            ordered_json err;
            err["code"]    = errorCodeString(r.exitCode, r.op);
            err["message"] = r.errorMessage;
            if (r.exitCode == 5 && r.op == "insert" && r.existingProgId > 0) {
                err["existing_prog_id"] = r.existingProgId;
            }
            if (r.exitCode == 8) {
                if (r.progId > 0) err["prog_id"] = r.progId;
                err["compile_errors"] = r.compileErrors;
            }
            j["error"] = err;
        }
        arr.push_back(std::move(j));
    }

    const std::string s = arr.dump();
    std::fwrite(s.c_str(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Common batch command handler: parses the manifest, launches the worker,
// emits the JSON array, returns the batch exit code.
int cmdBatch(const std::string& op, const std::string& manifestFile,
             const wchar_t* tsPathOverride) {
    const bool needsSource = (op == "insert" || op == "modify");

    // Parse the manifest on the main thread (before the worker).
    std::vector<BatchItem> items;
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

} // namespace

int main(int argc, char** argv) {
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

    // --- TVS-D.3: batch subcommands ---
    // batch-insert/batch-modify/batch-delete take a single <manifest_file>
    // argument. They ALWAYS emit a JSON array on stdout (no --json needed).
    // --json is accepted but ignored (the global pre-scan already handled it).
    if (iequals(subcommand, "batch-insert") ||
        iequals(subcommand, "batch-modify") ||
        iequals(subcommand, "batch-delete")) {
        if (reducedArgc != 3) {
            std::string opName = subcommand;
            std::fprintf(stderr,
                "tsdev_cli: %s requires exactly 1 argument: <manifest_file>\n",
                opName.c_str());
            return 1;
        }
        std::string op;
        if (iequals(subcommand, "batch-insert"))      op = "insert";
        else if (iequals(subcommand, "batch-modify")) op = "modify";
        else                                          op = "delete";
        return cmdBatch(op, reducedArgvPtr[2], tsPathWidePtr);
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
