#include "args.h"
#include "type_table.h"

#include <cstdlib>

ParseOutcome parse_args(int argc, wchar_t** argv, Options& out) {
    if (argc < 2) return {false, false, L"missing command (expected: open|exists|getsrc|insert|modify)"};
    if (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") return {true, true, L""};
    const std::wstring verb = argv[1];
    if (verb != L"open" && verb != L"exists" && verb != L"getsrc" && verb != L"insert" && verb != L"modify")
        return {false, false, L"unknown command (expected: open|exists|getsrc|insert|modify)"};
    out.verb = verb;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto value = [&](std::wstring& error) -> wchar_t* {
            if (i + 1 >= argc) {
                error = L"missing value for " + std::wstring(arg);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == L"--json") {
            out.json = true;
        } else if (arg == L"--nohide") {
            out.nohide = true;
        } else if (arg == L"--timeout") {
            if (i + 1 >= argc) return {false, false, L"missing value for --timeout"};
            const std::wstring raw = argv[++i];
            wchar_t* end = nullptr;
            const long parsed = wcstol(raw.c_str(), &end, 10);
            if (end != raw.c_str() + raw.size()) return {false, false, L"--timeout expects an integer (milliseconds)"};
            if (parsed <= 0) {
                out.timeouts_disabled = true;
            } else {
                out.timeout_ms = static_cast<unsigned long>(parsed);
            }
        } else if (arg == L"--ts-path") {
            if (i + 1 >= argc) return {false, false, L"missing value for --ts-path"};
            out.ts_path = argv[++i];
        } else if (arg == L"--type") {
            if (i + 1 >= argc) return {false, false, L"missing value for --type"};
            out.type = argv[++i];
        } else if (arg == L"--name") {
            if (i + 1 >= argc) return {false, false, L"missing value for --name"};
            out.name = argv[++i];
        } else if (arg == L"--output") {
            if (i + 1 >= argc) return {false, false, L"missing value for --output"};
            out.output_path = argv[++i];
        } else if (arg == L"--source") {
            if (i + 1 >= argc) return {false, false, L"missing value for --source"};
            out.source_path = argv[++i];
        } else {
            return {false, false, L"unknown option: " + arg};
        }
    }

    // --output is getsrc-specific: reject it for non-getsrc verbs.
    if (!out.output_path.empty() && out.verb != L"getsrc")
        return {false, false, L"--output is only valid with getsrc"};

    // --source is insert/modify-specific: reject it for open/exists/getsrc.
    if (!out.source_path.empty() && out.verb != L"insert" && out.verb != L"modify")
        return {false, false, L"--source is only valid with insert or modify"};

    // --source is required for insert and modify.
    if ((out.verb == L"insert" || out.verb == L"modify") && out.source_path.empty())
        return {false, false, L"--source is required for insert and modify"};

    if (out.type.empty()) return {false, false, L"--type is required (Strategy|Function|Indicator|ShowMe|TradingApp)"};
    if (out.name.empty()) return {false, false, L"--name is required"};
    if (!type_from_name(out.type, out.type_number, out.type_new_cmd_id, out.type_label))
        return {false, false, L"unsupported type '" + out.type +
                                  L"' (supported: Strategy, Function, Indicator, ShowMe, TradingApp)"};
    if (out.name.empty()) return {false, false, L"--name must not be empty"};
    if (out.name.find(L'"') != std::wstring::npos || out.name.find(L',') != std::wstring::npos)
        return {false, false, L"name must not contain '\"' or ',' (name encoding not supported yet)"};
    return {true, false, L""};
}

std::wstring usage_text() {
    return
        L"tsdevctl - drive TSDev.exe through its own control channel\n"
        L"\n"
        L"Usage:\n"
        L"  tsdevctl open   --type <Type> --name <Name> [options]\n"
        L"  tsdevctl exists --type <Type> --name <Name> [options]\n"
        L"  tsdevctl getsrc --type <Type> --name <Name> [--output <file>] [options]\n"
        L"  tsdevctl insert --type <Type> --name <Name> --source <file> [options]\n"
        L"  tsdevctl modify --type <Type> --name <Name> --source <file> [options]\n"
        L"\n"
        L"open: Opens the named item in TSDev (existing item) or starts a new empty\n"
        L"document titled with that name (TSDev's own open-or-create behavior).\n"
        L"exists: Reports whether the named item exists in the TSDev library.\n"
        L"The check opens the item through TSDev's own control channel and reads\n"
        L"TSDev's own result: the item's source (exists) or TSDev's \"document no\n"
        L"longer exists\" error page (does not exist). The document opened for the\n"
        L"check is closed again unless it was already open. Matching follows\n"
        L"TSDev's own lookup, which is case-insensitive; a case-sensitive answer\n"
        L"requires TSDev's own item enumeration (not available externally yet).\n"
        L"The reported name is the REQUESTED name: TSDev's stored spelling and the\n"
        L"item's ProgId are not obtainable through this channel (no enumeration is\n"
        L"externally reachable) and are therefore not returned.\n"
        L"getsrc: Emits the EasyLanguage source of the named item. Reuses the same\n"
        L"open-read-close mechanism as exists: the item is opened through TSDev's\n"
        L"control channel, the editor text (what TSDev renders, which IS the item's\n"
        L"source for a real item) is read, and the document is closed again unless it\n"
        L"was already open. Encrypted/protected items are decrypted automatically by\n"
        L"TSDev when rendering them in the editor, so getsrc inherits that decryption.\n"
        L"With --output <file_path> the source is written to that file (UTF-8, no\n"
        L"BOM, line endings preserved as-is) instead of stdout; --output is only\n"
        L"valid with getsrc. By default the raw source goes to stdout; with --json a\n"
        L"JSON object (including a source field, and output_path when --output was\n"
        L"used) is emitted instead.\n"
        L"insert: Creates a new item in the TSDev library from the EasyLanguage source\n"
        L"file given by --source (UTF-8, optional BOM). Before inserting, the item is\n"
        L"checked for existence (proactive collision avoidance); if it already exists\n"
        L"the operation fails with exit code 11. The item is created through TSDev's\n"
        L"own \"New <Type>\" command (triggered via its command routing), the name is\n"
        L"filled into TSDev's own create dialog, and the source is placed into the\n"
        L"editor via WM_SETTEXT (not the clipboard). The same verify (compile + save)\n"
        L"code path the UI triggers is then invoked. Compile failure is reported with\n"
        L"exit code 12 and the error dialog text; note the item may remain in the\n"
        L"library with uncompiled source in that case. Success is confirmed by\n"
        L"re-checking that the item now exists in the library.\n"
        L"modify: Replaces an existing item's EasyLanguage source with the source\n"
        L"from the file given by --source (UTF-8, optional BOM) and recompiles/\n"
        L"reverifies through TSDev's own verify (compile + save) code path. Before\n"
        L"modifying, the item is checked for existence; if it does not exist the\n"
        L"operation fails with exit code 1. The existing item is opened through\n"
        L"TSDev's control channel, the new source is placed into the editor via\n"
        L"WM_SETTEXT (not the clipboard), and the same verify code path the UI\n"
        L"triggers is invoked. Compile failure is reported with exit code 12 and\n"
        L"the error text; note the item may remain in the library with uncompiled\n"
        L"new source in that case (the verify path saves before compiling).\n"
        L"Success is confirmed by re-reading the item's source and comparing it\n"
        L"with the source that was set.\n"
        L"TSDev.exe is attached to if running, otherwise launched hidden.\n"
        L"\n"
        L"Options:\n"
        L"  --type <name>      Component type: Strategy, Function, Indicator,\n"
        L"                     ShowMe or TradingApp\n"
        L"  --name <name>      Item name\n"
        L"  --output <file>    (getsrc only) Write source to this file (UTF-8, no\n"
        L"                     BOM) instead of stdout\n"
        L"  --source <file>    (insert/modify) Read EasyLanguage source from this\n"
        L"                     file (UTF-8, optional BOM)\n"
        L"  --json             Machine-readable JSON output on stdout\n"
        L"  --timeout <ms>     Operation timeout in milliseconds (default 30000;\n"
        L"                     value <= 0 disables timeouts entirely)\n"
        L"  --nohide           Do not hide the TSDev window on launch/attach\n"
        L"  --ts-path <dir>    TradeStation installation root override\n"
        L"                     (default: auto-locate, fallback\n"
        L"                     C:\\Program Files (x86)\\TradeStation 9.5)\n"
        L"  -h, --help         Show this help\n"
        L"\n"
        L"Exit codes:\n"
        L"  0 success (exists/getsrc: item exists, source emitted for getsrc)\n"
        L"  1 exists/getsrc: item does not exist (successful answer, not an error);\n"
        L"     modify: item does not exist in the TSDev library (error)\n"
        L"  2 usage error\n"
        L"  3 not running elevated (required)\n"
        L"  4 TSDev.exe not found or launch failed\n"
        L"  5 pipe connect failed or timed out\n"
        L"  6 operation timed out (success criterion not met)\n"
        L"  7 TSDev error dialog detected (message text in output)\n"
        L"  8 TSDev crashed\n"
        L"  9 busy (another automation invocation is running)\n"
        L"  10 unexpected TSDev state (e.g. no editor control in the opened document)\n"
        L"  11 insert: item already exists in the TSDev library\n"
        L"  12 insert/modify: compile/verify failed (error dialog or item not saved)\n";
}
