#include "tsdevlib/args.h"
#include "tsdevlib/doc_read.h"
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
    check(type_from_name(L"Indicator", number, label) && number == 2 && label == L"Indicator", "Indicator maps to 2");
    check(type_from_name(L"ShowMe", number, label) && number == 1282 && label == L"ShowMe", "ShowMe maps to 1282");
    check(type_from_name(L"TradingApp", number, label) && number == 5122 && label == L"Trading Application",
          "TradingApp maps to 5122");
    check(!type_from_name(L"", number, label), "empty type rejected");
    check(!type_from_name(L"strategy", number, label), "type match is case-sensitive");
    check(!type_from_name(L"ActivityBar", number, label), "ActivityBar out of scope (verified 3, not offered)");
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
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"PaintBar"),
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

void test_parse_args_exists() {
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"exists"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Indicator"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyInd")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(p.ok && opts.verb == L"exists" && opts.type == L"Indicator" && opts.type_number == 2 &&
                  opts.name == L"MyInd",
              "exists with type+name parses");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"exists"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"PaintBar"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "exists with unmapped type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"exists"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy")};
        const ParseOutcome p = parse_args(4, argv, opts);
        check(!p.ok, "exists without --name is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"frobnicate")};
        const ParseOutcome p = parse_args(2, argv, opts);
        check(!p.ok, "unknown verb is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Indicator"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(p.ok && opts.verb == L"open" && opts.type_number == 2, "open accepts Indicator once mapped");
    }
}

void test_phantom_classifier() {
    // Byte-exact phantom error page captured live from TSDev 2026-09-03
    // (tmp/phantom_template.txt): '{' + 85*'!' + "\n\nError loading document\n\n"
    // + 86*'!' + "\n\nThe technique <name> no longer exists in your EasyLanguage
    // workarea. \n\n" + 85*'!' + '}' — 383 chars with a 32-char name.
    const std::wstring bang85(85, L'!');
    const std::wstring bang86(86, L'!');
    const std::wstring phantom =
        L"{" + bang85 + L"\n\nError loading document\n\n" + bang86 +
        L"\n\nThe technique ZZZExistsFixture_20260903T094049 no longer exists in your EasyLanguage workarea. \n\n" +
        bang85 + L"}";
    check(phantom.size() == 383, "phantom template length matches the live capture");
    check(is_phantom_error_text(phantom), "exact captured phantom template classifies as phantom");
    check(is_phantom_error_text(
              L"{!!!\n\nError loading document\n\nThe technique SomeOtherName no longer exists in your "
              L"EasyLanguage workarea. \n\n!!!}"),
          "phantom signature is name-parameterized");
    check(!is_phantom_error_text(L"print(\"x\");\nBuy next bar at market;"), "real source is not phantom");
    check(!is_phantom_error_text(L""), "empty text is not phantom");
    check(!is_phantom_error_text(L"Error loading document"), "half of the signature is not phantom");
    check(!is_phantom_error_text(L"no longer exists in your EasyLanguage workarea"),
          "other half of the signature is not phantom");
    check(is_phantom_error_text(L"Error loading document and no longer exists in your EasyLanguage workarea"),
          "any text containing both signature substrings classifies as phantom (substring rule; the "
          "real-item-with-matching-source false negative is a documented limitation)");
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

    OpResult exists_yes;
    exists_yes.ok = true;
    exists_yes.op = L"exists";
    exists_yes.type = L"Strategy";
    exists_yes.name = L"MyTestStrategyA";
    exists_yes.reports_exists = true;
    exists_yes.exists_value = true;
    exists_yes.has_pid = true;
    exists_yes.pid = 7;
    exists_yes.elapsed_ms = 99;
    const std::wstring yes_json = make_result_json(exists_yes);
    check(yes_json == L"{\"ok\":true,\"op\":\"exists\",\"type\":\"Strategy\",\"name\":\"MyTestStrategyA\","
                      L"\"tsdev_pid\":7,\"elapsed_ms\":99,\"exists\":true,\"error\":null}",
          "exists-true json shape");

    OpResult exists_no;
    exists_no.ok = true;
    exists_no.op = L"exists";
    exists_no.type = L"Indicator";
    exists_no.name = L"ZZZMissing";
    exists_no.reports_exists = true;
    exists_no.exists_value = false;
    exists_no.elapsed_ms = 5;
    const std::wstring no_json = make_result_json(exists_no);
    check(no_json == L"{\"ok\":true,\"op\":\"exists\",\"type\":\"Indicator\",\"name\":\"ZZZMissing\","
                     L"\"tsdev_pid\":null,\"elapsed_ms\":5,\"exists\":false,\"error\":null}",
          "exists-false json shape");
}

}  // namespace

int wmain() {
    test_type_table();
    test_parse_args();
    test_parse_args_exists();
    test_phantom_classifier();
    test_json();
    if (failures == 0) {
        std::wprintf(L"all unit tests passed\n");
        return 0;
    }
    std::wprintf(L"%d unit test(s) failed\n", failures);
    return 1;
}
