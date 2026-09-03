#include "args.h"
#include "type_table.h"

#include <cstdlib>

ParseOutcome parse_args(int argc, wchar_t** argv, Options& out) {
    if (argc < 2) return {false, false, L"missing command (expected: open|exists)"};
    if (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") return {true, true, L""};
    const std::wstring verb = argv[1];
    if (verb != L"open" && verb != L"exists")
        return {false, false, L"unknown command (expected: open|exists)"};
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
        } else {
            return {false, false, L"unknown option: " + arg};
        }
    }

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
        L"TSDev.exe is attached to if running, otherwise launched hidden.\n"
        L"\n"
        L"Options:\n"
        L"  --type <name>      Component type: Strategy, Function, Indicator,\n"
        L"                     ShowMe or TradingApp\n"
        L"  --name <name>      Item name\n"
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
        L"  0 success (exists: item exists)\n"
        L"  1 exists: item does not exist (successful answer, not an error)\n"
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
