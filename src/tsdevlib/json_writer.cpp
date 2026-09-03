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
