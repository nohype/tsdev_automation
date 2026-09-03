#pragma once
#include <string>

std::wstring json_escape(const std::wstring& value);
std::string to_utf8(const std::wstring& value);

struct OpResult {
    bool ok = false;
    std::wstring op;
    std::wstring type;
    std::wstring name;
    bool has_pid = false;
    unsigned long pid = 0;
    unsigned long long elapsed_ms = 0;
    // Set by operations that answer an existence question (exists).
    bool reports_exists = false;
    bool exists_value = false;
    int error_code = 0;
    std::wstring error_name;
    std::wstring error_message;
};

std::wstring make_result_json(const OpResult& result);
