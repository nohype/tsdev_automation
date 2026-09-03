#pragma once
#include <string>

struct Options {
    std::wstring verb;
    bool json = false;
    bool nohide = false;
    bool timeouts_disabled = false;
    unsigned long timeout_ms = 30000;
    std::wstring ts_path;
    std::wstring type;
    std::wstring name;
    int type_number = -1;
    int type_new_cmd_id = 0;
    std::wstring type_label;
    std::wstring output_path;  // getsrc-only: write source to this file (--output).
    std::wstring source_path;  // insert-only: read source from this file (--source).
};

struct ParseOutcome {
    bool ok = false;
    bool help = false;
    std::wstring error;
};

ParseOutcome parse_args(int argc, wchar_t** argv, Options& out);
std::wstring usage_text();
