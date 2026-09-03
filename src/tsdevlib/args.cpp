#include "args.h"
#include "type_table.h"

#include <cstdlib>

ParseOutcome parse_args(int argc, wchar_t** argv, Options& out) {
    if (argc < 2) return {false, false, L"missing command (expected: open)"};
    if (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") return {true, true, L""};
    if (std::wstring(argv[1]) != L"open") return {false, false, L"unknown command (expected: open)"};

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

    if (out.type.empty()) return {false, false, L"--type is required (Strategy|Function)"};
    if (out.name.empty()) return {false, false, L"--name is required"};
    if (!type_from_name(out.type, out.type_number, out.type_label))
        return {false, false, L"unsupported type '" + out.type + L"' (supported: Strategy, Function)"};
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
        L"  tsdevctl open --type <Strategy|Function> --name <Name> [options]\n"
        L"\n"
        L"Opens the named item in TSDev (existing item) or starts a new empty\n"
        L"document titled with that name (TSDev's own open-or-create behavior).\n"
        L"TSDev.exe is attached to if running, otherwise launched hidden.\n"
        L"\n"
        L"Options:\n"
        L"  --type <name>      Component type: Strategy or Function\n"
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
        L"  0 success\n"
        L"  2 usage error\n"
        L"  3 not running elevated (required)\n"
        L"  4 TSDev.exe not found or launch failed\n"
        L"  5 pipe connect failed or timed out\n"
        L"  6 operation timed out (success criterion not met)\n"
        L"  7 TSDev error dialog detected (message text in output)\n"
        L"  8 TSDev crashed\n"
        L"  9 busy (another automation invocation is running)\n";
}
