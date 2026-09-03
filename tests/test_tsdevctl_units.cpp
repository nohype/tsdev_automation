#include "tsdevlib/args.h"
#include "tsdevlib/json_writer.h"
#include "tsdevlib/type_table.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::wprintf(L"FAIL: %hs\n", what);
    }
}

void test_type_table() {
    int number = -1;
    std::wstring label;
    check(type_from_name(L"Strategy", number, label) && number == 0 && label == L"Strategy", "Strategy maps to 0");
    check(type_from_name(L"Function", number, label) && number == 1 && label == L"Function", "Function maps to 1");
    check(!type_from_name(L"Indicator", number, label), "Indicator not mapped yet");
    check(!type_from_name(L"", number, label), "empty type rejected");
    check(!type_from_name(L"strategy", number, label), "type match is case-sensitive");
}

void test_parse_args() {
    {
        Options opts;
        const ParseOutcome p = parse_args(1, nullptr, opts);
        check(!p.ok && !p.help, "no args is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"--help")};
        const ParseOutcome p = parse_args(2, argv, opts);
        check(p.ok && p.help, "--help is recognized");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyItem")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(p.ok && opts.type == L"Function" && opts.type_number == 1 && opts.name == L"MyItem",
              "open with type+name parses");
        check(!opts.json && !opts.nohide && !opts.timeouts_disabled && opts.timeout_ms == 30000, "defaults are correct");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Indicator"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "unsupported type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Bad,Name")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "name with comma is rejected");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--timeout"), const_cast<wchar_t*>(L"0")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(p.ok && opts.timeouts_disabled, "timeout <= 0 disables timeouts");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--timeout"), const_cast<wchar_t*>(L"5000")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(p.ok && !opts.timeouts_disabled && opts.timeout_ms == 5000, "timeout 5000 is accepted");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--timeout"), const_cast<wchar_t*>(L"abc")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "non-numeric timeout is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--json"), const_cast<wchar_t*>(L"--nohide"),
                           const_cast<wchar_t*>(L"--ts-path"), const_cast<wchar_t*>(L"C:\\TS")};
        const ParseOutcome p = parse_args(10, argv, opts);
        check(p.ok && opts.json && opts.nohide && opts.ts_path == L"C:\\TS", "flags parse");
    }
}

void test_json() {
    check(json_escape(L"plain") == L"plain", "json_escape passthrough");
    check(json_escape(L"a\"b\\c\nd") == L"a\\\"b\\\\c\\nd", "json escaping");
    check(json_escape(L"\t") == L"\\t", "tab escaping");

    OpResult ok_result;
    ok_result.ok = true;
    ok_result.op = L"open";
    ok_result.type = L"Function";
    ok_result.name = L"E2e Fn";
    ok_result.has_pid = true;
    ok_result.pid = 42;
    ok_result.elapsed_ms = 1234;
    const std::wstring json = make_result_json(ok_result);
    check(json == L"{\"ok\":true,\"op\":\"open\",\"type\":\"Function\",\"name\":\"E2e Fn\",\"tsdev_pid\":42,"
                  L"\"elapsed_ms\":1234,\"error\":null}",
          "success json shape");

    OpResult err_result;
    err_result.ok = false;
    err_result.op = L"open";
    err_result.type = L"Function";
    err_result.name = L"X";
    err_result.error_code = 6;
    err_result.error_name = L"timeout";
    err_result.error_message = L"took \"too\" long";
    const std::wstring err_json = make_result_json(err_result);
    check(err_json == L"{\"ok\":false,\"op\":\"open\",\"type\":\"Function\",\"name\":\"X\",\"tsdev_pid\":null,"
                      L"\"elapsed_ms\":0,\"error\":{\"code\":6,\"name\":\"timeout\",\"message\":\"took \\\"too\\\" long\"}}",
          "error json shape");
}
}

int wmain() {
    test_type_table();
    test_parse_args();
    test_json();
    if (failures == 0) {
        std::wprintf(L"all unit tests passed\n");
        return 0;
    }
    std::wprintf(L"%d unit test(s) failed\n", failures);
    return 1;
}
