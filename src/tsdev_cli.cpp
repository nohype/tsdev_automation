// tsdev_cli.cpp
//
// Production CLI for the TradeStation Development Environment library.
//
// Usage:
//   tsdev_cli insert  <type> <name> <source_file>
//   tsdev_cli modify  <type> <name> <source_file>
//   tsdev_cli delete  <type> <name>
//
//   <type>        "strategy", "function", "indicator", or "tradingapp" (case-insensitive)
//   <name>        component name
//   <source_file> path to a file containing EasyLanguage source (insert/modify)
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
// Exit codes: 0 on success, non-zero on any failure.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (elevated_test20.cpp) runs its
// ENTIRE flow in a worker thread with a 64 MB stack. We do the same: all
// TsDevLib operations run on a 64 MB-stack worker thread.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <vector>

using namespace tsdev;

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
        "  tsdev_cli [--ts-path <directory>] insert  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--ts-path <directory>] modify  <type> <name> "
        "<source_file>\n"
        "  tsdev_cli [--ts-path <directory>] delete  <type> <name>\n"
        "  <type>        strategy | function | indicator | tradingapp (case-insensitive)\n"
        "  <name>        component name\n"
        "  <source_file> path to a file containing EasyLanguage source "
        "(insert/modify)\n"
        "  --ts-path <directory>  Override the TradeStation install directory "
        "(default: auto-discovered from registry)\n");
}

// Parameters passed to the worker thread.
struct InsertParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    int            exitCode;
};

// The INSERT flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runInsert(LPVOID param) {
    InsertParams* p = static_cast<InsertParams*>(param);

    // 2. Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tsdev_cli: init failed: %s\n", e.what());
        p->exitCode = 4;
        return 0;
    }

    // 3. Check if the component already exists — fail loudly, no overwrite.
    int existing = lib->findExistPos(p->name, p->type);
    if (existing > 0) {
        std::fprintf(stderr,
            "tsdev_cli: ERROR — %s \"%s\" already exists (ProgId=%d). "
            "Use modify instead.\n",
            p->typeStr.c_str(), p->name.c_str(), existing);
        p->exitCode = 5;
        delete lib;
        return 0;
    }

    // 4. Create the entry.
    int errCode = 0;
    int progId = lib->createEntry(p->type, p->name, errCode);
    if (progId <= 0) {
        std::fprintf(stderr,
            "tsdev_cli: createEntry failed (ProgId=%d, errCode=0x%X)\n",
            progId, errCode);
        p->exitCode = 6;
        delete lib;
        return 0;
    }

    // 5. Save the source.
    if (!lib->saveSource(p->type, progId, p->source)) {
        std::fprintf(stderr,
            "tsdev_cli: saveSource failed for ProgId=%d\n", progId);
        p->exitCode = 7;
        delete lib;
        return 0;
    }

    // 6. Compile / verify — fail loudly on errors.
    std::string errors = lib->verifyModule(p->type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
            "tsdev_cli: compilation FAILED for \"%s\" (ProgId=%d):\n%s\n",
            p->name.c_str(), progId, errors.c_str());
        p->exitCode = 8;
        delete lib;
        return 0;
    }

    // 7. Close the warehouse (flush).
    lib->closeWarehouse();

    // 8. Verify the .ell file exists.
    std::string ellPath = lib->getEllPath(p->type, progId);
    if (!lib->ellFileExists(p->type, progId)) {
        std::fprintf(stderr,
            "tsdev_cli: compilation succeeded but .ell file not found: %s\n",
            ellPath.c_str());
        p->exitCode = 9;
        delete lib;
        return 0;
    }

    // 9. Success.
    std::fprintf(stdout,
        "SUCCESS: %s \"%s\" inserted, ProgId=%d, .ell=%s\n",
        p->typeStr.c_str(), p->name.c_str(), progId, ellPath.c_str());
    p->exitCode = 0;

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

// --- The INSERT subcommand (parses args, validates, launches worker) ---
int cmdInsert(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride) {
    ComponentType type;
    if (!parseType(typeStr, type)) {
        std::fprintf(stderr,
            "tsdev_cli: unknown type \"%s\" "
            "(expected strategy/function/indicator/tradingapp)\n",
            typeStr.c_str());
        return 2;
    }

    if (name.empty()) {
        std::fprintf(stderr, "tsdev_cli: component name must not be empty\n");
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        std::fprintf(stderr,
            "tsdev_cli: failed to read source file \"%s\"\n",
            sourceFile.c_str());
        return 3;
    }
    if (source.empty()) {
        std::fprintf(stderr, "tsdev_cli: source file is empty\n");
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
    params.exitCode = 0;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runInsert, &params, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    return params.exitCode;
}

// --- The MODIFY subcommand ---

// Parameters passed to the worker thread.
struct ModifyParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    std::string    source;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    int            exitCode;
};

// The MODIFY flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it. modifyEntry composes the
// existing INSERT primitives (findExistPos -> saveSource -> verifyModule)
// on the EXISTING ProgId — no createEntry, so the ProgId is preserved.
DWORD WINAPI runModify(LPVOID param) {
    ModifyParams* p = static_cast<ModifyParams*>(param);

    // Construct TsDevLib.
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib(p->tsPathOverride);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tsdev_cli: init failed: %s\n", e.what());
        p->exitCode = 4;
        return 0;
    }

    // modifyEntry: find existing -> saveSource -> verifyModule (recompile).
    Result res = lib->modifyEntry(p->type, p->name, p->source);

    if (!res.success) {
        // Distinguish "component not found" from compile errors so the
        // user gets a clear, actionable message.
        if (res.message == "component not found") {
            std::fprintf(stderr,
                "tsdev_cli: ERROR — %s \"%s\" not found in the warehouse. "
                "Use insert to create it.\n",
                p->typeStr.c_str(), p->name.c_str());
            p->exitCode = 5;
        } else {
            // Compile error (or saveSource failure) — fail loudly with the
            // full error message returned by the library.
            std::fprintf(stderr,
                "tsdev_cli: modification FAILED for \"%s\" (ProgId=%d):\n%s\n",
                p->name.c_str(), res.progId, res.message.c_str());
            p->exitCode = 8;
        }
        delete lib;
        return 0;
    }

    // Success — component modified, recompiled, and persisted.
    std::string ellPath = lib->getEllPath(p->type, res.progId);
    std::fprintf(stdout,
        "SUCCESS: %s \"%s\" modified, ProgId=%d, .ell=%s\n",
        p->typeStr.c_str(), p->name.c_str(), res.progId, ellPath.c_str());
    p->exitCode = 0;

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdModify(const std::string& typeStr,
              const std::string& name,
              const std::string& sourceFile,
              const wchar_t* tsPathOverride) {
    ComponentType type;
    if (!parseType(typeStr, type)) {
        std::fprintf(stderr,
            "tsdev_cli: unknown type \"%s\" "
            "(expected strategy/function/indicator/tradingapp)\n",
            typeStr.c_str());
        return 2;
    }

    if (name.empty()) {
        std::fprintf(stderr, "tsdev_cli: component name must not be empty\n");
        return 2;
    }

    // 1. Read the source file (on the main thread — no orlib dependency).
    std::string source;
    if (!readFile(sourceFile, source)) {
        std::fprintf(stderr,
            "tsdev_cli: failed to read source file \"%s\"\n",
            sourceFile.c_str());
        return 3;
    }
    if (source.empty()) {
        std::fprintf(stderr, "tsdev_cli: source file is empty\n");
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
    params.exitCode = 0;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runModify, &params, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    return params.exitCode;
}

// --- The DELETE subcommand ---

// Parameters passed to the worker thread.
struct DeleteParams {
    ComponentType  type;
    std::string    typeStr;
    std::string    name;
    const wchar_t* tsPathOverride;  // nullptr = auto-discover
    int            exitCode;
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
        std::fprintf(stderr, "tsdev_cli: init failed: %s\n", e.what());
        p->exitCode = 4;
        return 0;
    }

    // Check if the component exists first (for a distinct not-found message).
    int progId = lib->findExistPos(p->name, p->type);
    if (progId <= 0) {
        std::fprintf(stderr,
            "tsdev_cli: ERROR — %s \"%s\" not found in the warehouse.\n",
            p->typeStr.c_str(), p->name.c_str());
        p->exitCode = 5;
        delete lib;
        return 0;
    }

    // Delete the entry. deleteEntry calls FindExistPos again internally,
    // then InitGlobalLookup + DeleteFromTree, and deletes the .ell/.xml
    // files. The destructor calls OrlibExit which persists the deletion.
    bool deleted = lib->deleteEntry(p->type, p->name);
    if (!deleted) {
        std::fprintf(stderr,
            "tsdev_cli: deleteEntry failed for %s \"%s\" (ProgId=%d).\n",
            p->typeStr.c_str(), p->name.c_str(), progId);
        p->exitCode = 6;
        delete lib;
        return 0;
    }

    std::fprintf(stdout,
        "SUCCESS: %s \"%s\" deleted (ProgId=%d was removed from warehouse, "
        ".ell/.xml files removed from disk).\n",
        p->typeStr.c_str(), p->name.c_str(), progId);
    p->exitCode = 0;

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

int cmdDelete(const std::string& typeStr, const std::string& name,
              const wchar_t* tsPathOverride) {
    ComponentType type;
    if (!parseType(typeStr, type)) {
        std::fprintf(stderr,
            "tsdev_cli: unknown type \"%s\" "
            "(expected strategy/function/indicator/tradingapp)\n",
            typeStr.c_str());
        return 2;
    }

    if (name.empty()) {
        std::fprintf(stderr, "tsdev_cli: component name must not be empty\n");
        return 2;
    }

    // Run the orlib flow on a 64 MB-stack worker thread.
    DeleteParams params;
    params.type     = type;
    params.typeStr  = typeStr;
    params.name     = name;
    params.tsPathOverride = tsPathOverride;
    params.exitCode = 0;

    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDelete, &params, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "tsdev_cli: CreateThread failed: %lu\n",
                     GetLastError());
        return 10;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    return params.exitCode;
}

} // namespace

int main(int argc, char** argv) {
    // --- Global flag pre-scan: --ts-path ---
    // Scan argv[1..argc-1] for the optional --ts-path flag (which may appear
    // in either space-separated ("--ts-path <path>") or equals ("--ts-path=
    // <path>") form) and remove it (and its value) from the argv array. The
    // resulting reduced argv contains only the subcommand and its positional
    // args, so the existing exact-argc subcommand dispatch below works
    // unchanged. If --ts-path is not given, the reduced argv equals the
    // original argv and the path override is nullptr (auto-discover).
    //
    // If --ts-path appears multiple times, the LAST occurrence wins.
    const char* tsPathOverride = nullptr;
    std::string tsPathStr;  // holds the value for the equals form

    std::vector<char*> reducedArgv;
    reducedArgv.push_back(argv[0]);  // program name

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--ts-path") {
            // Space-separated form: value is the next arg.
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "Error: --ts-path requires a directory path\n");
                return 1;
            }
            if (argv[i + 1][0] == '\0') {
                std::fprintf(stderr,
                    "Error: --ts-path requires a non-empty directory path\n");
                return 1;
            }
            tsPathOverride = argv[i + 1];
            i++;  // skip the value
        } else if (arg.rfind("--ts-path=", 0) == 0) {
            // Equals form: value is everything after the '='.
            tsPathStr = arg.substr(std::strlen("--ts-path="));
            if (tsPathStr.empty()) {
                std::fprintf(stderr,
                    "Error: --ts-path requires a non-empty directory path\n");
                return 1;
            }
            tsPathOverride = tsPathStr.c_str();
        } else {
            reducedArgv.push_back(argv[i]);
        }
    }

    int reducedArgc = static_cast<int>(reducedArgv.size());
    char** reducedArgvPtr = reducedArgv.data();

    if (reducedArgc < 2) {
        printUsage();
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
            printUsage();
            return 1;
        }
        return cmdInsert(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr);
    }

    if (iequals(subcommand, "modify")) {
        if (reducedArgc != 5) {
            printUsage();
            return 1;
        }
        return cmdModify(reducedArgvPtr[2], reducedArgvPtr[3],
                         reducedArgvPtr[4], tsPathWidePtr);
    }

    if (iequals(subcommand, "delete")) {
        if (reducedArgc != 4) {
            printUsage();
            return 1;
        }
        return cmdDelete(reducedArgvPtr[2], reducedArgvPtr[3],
                         tsPathWidePtr);
    }

    std::fprintf(stderr,
        "tsdev_cli: unknown subcommand \"%s\"\n", subcommand.c_str());
    printUsage();
    return 1;
}
