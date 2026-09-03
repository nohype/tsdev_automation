#include "args.h"
#include "type_table.h"

#include <cstdlib>

ParseOutcome parse_args(int argc, wchar_t** argv, Options& out) {
    if (argc < 2) return {false, false, L"missing command (expected: open|exists|getsrc)"};
    if (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") return {true, true, L""};
    const std::wstring verb = argv[1];
    if (verb != L"open" && verb != L"exists" && verb != L"getsrc")
        return {false, false, L"unknown command (expected: open|exists|getsrc)"};
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
        } else {
            return {false, false, L"unknown option: " + arg};
        }
    }

    // --output is getsrc-specific: reject it for open/exists.
    if (!out.output_path.empty() && out.verb != L"getsrc")
        return {false, false, L"--output is only valid with getsrc"};

    if (out.type.empty()) return {false, false, L"--type is required (Strategy|Function|Indicator|ShowMe|TradingApp)"};
    if (out.name.empty()) return {false, false, L"--name is required"};
    if (!type_from_name(out.type, out.type_number, out.type_label))
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
        L"TSDev.exe is attached to if running, otherwise launched hidden.\n"
        L"\n"
        L"Options:\n"
        L"  --type <name>      Component type: Strategy, Function, Indicator,\n"
        L"                     ShowMe or TradingApp\n"
        L"  --name <name>      Item name\n"
        L"  --output <file>    (getsrc only) Write source to this file (UTF-8, no\n"
        L"                     BOM) instead of stdout\n"
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
        L"  1 exists/getsrc: item does not exist (successful answer, not an error)\n"
        L"  2 usage error\n"
        L"  3 not running elevated (required)\n"
        L"  4 TSDev.exe not found or launch failed\n"
        L"  5 pipe connect failed or timed out\n"
        L"  6 operation timed out (success criterion not met)\n"
        L"  7 TSDev error dialog detected (message text in output)\n"
        L"  8 TSDev crashed\n"
        L"  9 busy (another automation invocation is running)\n"
        L"  10 unexpected TSDev state (e.g. no editor control in the opened document)\n";
}
