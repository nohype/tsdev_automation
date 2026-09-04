#include "json_writer.h"

#include <windows.h>
#include <cstdio>

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::wstring json_escape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 2);
    for (wchar_t c : value) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (static_cast<unsigned>(c) < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::wstring make_result_json(const OpResult& r) {
    std::wstring json = L"{\"ok\":";
    json += r.ok ? L"true" : L"false";
    json += L",\"op\":\"" + json_escape(r.op) + L"\"";
    json += L",\"type\":\"" + json_escape(r.type) + L"\"";
    json += L",\"name\":\"" + json_escape(r.name) + L"\"";
    json += L",\"tsdev_pid\":";
    json += r.has_pid ? std::to_wstring(r.pid) : L"null";
    json += L",\"elapsed_ms\":" + std::to_wstring(r.elapsed_ms);
    if (r.reports_exists) {
        json += L",\"exists\":";
        json += r.exists_value ? L"true" : L"false";
    }
    if (r.reports_source) {
        // source is null when the item was not found (exists=false); the
        // editor text in that case is the phantom error page, not source.
        if (r.exists_value) {
            json += L",\"source\":\"" + json_escape(r.source) + L"\"";
        } else {
            json += L",\"source\":null";
        }
    }
    // output_path is emitted only when a file was actually written (real item
    // found + --output given); not emitted for not-found even if --output was
    // requested (no file is written in that case).
    if (!r.output_path.empty() && r.exists_value) {
        json += L",\"output_path\":\"" + json_escape(r.output_path) + L"\"";
    }
    json += L",\"error\":";
    if (r.error_code == 0) {
        json += L"null";
    } else {
        json += L"{\"code\":" + std::to_wstring(r.error_code);
        json += L",\"name\":\"" + json_escape(r.error_name) + L"\"";
        json += L",\"message\":\"" + json_escape(r.error_message) + L"\"}";
    }
    json += L"}";
    return json;
}

std::wstring make_list_result_json(const OpResult& r, const std::vector<ListItem>& items) {
    std::wstring json = L"{\"ok\":";
    json += r.ok ? L"true" : L"false";
    json += L",\"op\":\"" + json_escape(r.op) + L"\"";
    // type is null when no --type filter was given (r.type empty).
    if (r.type.empty()) {
        json += L",\"type\":null";
    } else {
        json += L",\"type\":\"" + json_escape(r.type) + L"\"";
    }
    json += L",\"count\":" + std::to_wstring(items.size());
    json += L",\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) json += L",";
        json += L"{\"name\":\"" + json_escape(items[i].name) + L"\"";
        json += L",\"type\":\"" + json_escape(items[i].type) + L"\"";
        json += L",\"prog_id\":\"" + json_escape(items[i].prog_id) + L"\"}";
    }
    json += L"]";
    json += L",\"tsdev_pid\":";
    json += r.has_pid ? std::to_wstring(r.pid) : L"null";
    json += L",\"elapsed_ms\":" + std::to_wstring(r.elapsed_ms);
    json += L",\"error\":";
    if (r.error_code == 0) {
        json += L"null";
    } else {
        json += L"{\"code\":" + std::to_wstring(r.error_code);
        json += L",\"name\":\"" + json_escape(r.error_name) + L"\"";
        json += L",\"message\":\"" + json_escape(r.error_message) + L"\"}";
    }
    json += L"}";
    return json;
}
