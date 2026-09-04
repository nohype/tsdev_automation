#pragma once
#include <string>
#include <vector>

std::wstring json_escape(const std::wstring& value);
std::string to_utf8(const std::wstring& value);
std::wstring from_utf8(const std::string& value);

struct OpResult {
    bool ok = false;
    std::wstring op;
    std::wstring type;
    std::wstring name;
    bool has_pid = false;
    unsigned long pid = 0;
    unsigned long long elapsed_ms = 0;
    // Set by operations that answer an existence question (exists, getsrc).
    bool reports_exists = false;
    bool exists_value = false;
    // Set by getsrc when it has a source answer (real item or not-found).
    bool reports_source = false;
    std::wstring source;
    // Set by getsrc when --output was used and the file was written.
    std::wstring output_path;
    int error_code = 0;
    std::wstring error_name;
    std::wstring error_message;
};

std::wstring make_result_json(const OpResult& result);

// One enumerated library item (list verb). type is the resolved type label
// (TSDev's own category-table label, or the stable numeric fallback).
struct ListItem {
    std::wstring name;
    std::wstring type;
    std::wstring prog_id;
};

// List-result JSON (hand-rolled, same approach as make_result_json):
// {"ok":..,"op":"list","type":"<Type>"|null,"count":N,
//  "items":[{"name":..,"type":..,"prog_id":..},...],
//  "tsdev_pid":..,"elapsed_ms":..,"error":..}
// r.type empty (no --type filter) emits "type":null; count == items.size().
std::wstring make_list_result_json(const OpResult& result, const std::vector<ListItem>& items);
