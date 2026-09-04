#include "tsdevlib/args.h"
#include "tsdevlib/doc_read.h"
#include "tsdevlib/inject.h"
#include "tsdevlib/json_writer.h"
#include "tsdevlib/type_table.h"

#include <windows.h>

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
    int cmd = 0;
    std::wstring label;
    check(type_from_name(L"Strategy", number, cmd, label) && number == 0 && label == L"Strategy", "Strategy maps to 0");
    check(type_from_name(L"Function", number, cmd, label) && number == 1 && label == L"Function", "Function maps to 1");
    check(type_from_name(L"Indicator", number, cmd, label) && number == 2 && label == L"Indicator", "Indicator maps to 2");
    check(type_from_name(L"ShowMe", number, cmd, label) && number == 1282 && label == L"ShowMe", "ShowMe maps to 1282");
    check(type_from_name(L"TradingApp", number, cmd, label) && number == 5122 && label == L"Trading Application",
          "TradingApp maps to 5122");
    check(!type_from_name(L"", number, cmd, label), "empty type rejected");
    check(!type_from_name(L"strategy", number, cmd, label), "type match is case-sensitive");
    check(!type_from_name(L"ActivityBar", number, cmd, label), "ActivityBar out of scope (verified 3, not offered)");
}

void test_type_table_new_cmd() {
    int num = 0, cmd = 0;
    std::wstring label;
    check(type_from_name(L"Strategy", num, cmd, label) && cmd == 0x2950, "Strategy new_cmd_id");
    check(type_from_name(L"Function", num, cmd, label) && cmd == 0x2951, "Function new_cmd_id");
    check(type_from_name(L"Indicator", num, cmd, label) && cmd == 0x2947, "Indicator new_cmd_id");
    check(type_from_name(L"ShowMe", num, cmd, label) && cmd == 0x2949, "ShowMe new_cmd_id");
    check(type_from_name(L"TradingApp", num, cmd, label) && cmd == 0x3D21, "TradingApp new_cmd_id");
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

void test_parse_args_getsrc() {
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyTestStrategyA")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(p.ok && opts.verb == L"getsrc" && opts.type == L"Strategy" && opts.type_number == 0 &&
                  opts.name == L"MyTestStrategyA" && opts.output_path.empty(),
              "getsrc with type+name parses, no output_path");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Fn1"),
                           const_cast<wchar_t*>(L"--output"), const_cast<wchar_t*>(L"C:\\out\\src.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(p.ok && opts.verb == L"getsrc" && opts.output_path == L"C:\\out\\src.eld",
              "getsrc --output parses output_path");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--output"), const_cast<wchar_t*>(L"C:\\f.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--output with open is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"exists"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--output"), const_cast<wchar_t*>(L"C:\\f.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--output with exists is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy")};
        const ParseOutcome p = parse_args(4, argv, opts);
        check(!p.ok, "getsrc without --name is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"PaintBar"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "getsrc with unmapped type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--output")};
        const ParseOutcome p = parse_args(7, argv, opts);
        check(!p.ok, "getsrc --output without a value is a usage error");
    }
}

void test_json_getsrc() {
    // getsrc success (no --output): source present, exists true.
    OpResult src_ok;
    src_ok.ok = true;
    src_ok.op = L"getsrc";
    src_ok.type = L"Strategy";
    src_ok.name = L"MyTestStrategyA";
    src_ok.has_pid = true;
    src_ok.pid = 1234;
    src_ok.elapsed_ms = 567;
    src_ok.reports_exists = true;
    src_ok.exists_value = true;
    src_ok.reports_source = true;
    src_ok.source = L"Buy next bar at market;\n";
    const std::wstring ok_json = make_result_json(src_ok);
    check(ok_json == L"{\"ok\":true,\"op\":\"getsrc\",\"type\":\"Strategy\",\"name\":\"MyTestStrategyA\","
                     L"\"tsdev_pid\":1234,\"elapsed_ms\":567,\"exists\":true,\"source\":\"Buy next bar at market;\\n\","
                     L"\"error\":null}",
          "getsrc success json shape (source + exists)");

    // getsrc NotFound: source null, exists false.
    OpResult src_no;
    src_no.ok = true;
    src_no.op = L"getsrc";
    src_no.type = L"Strategy";
    src_no.name = L"NoSuchItem";
    src_no.has_pid = true;
    src_no.pid = 1234;
    src_no.elapsed_ms = 567;
    src_no.reports_exists = true;
    src_no.exists_value = false;
    src_no.reports_source = true;
    src_no.source.clear();
    const std::wstring no_json = make_result_json(src_no);
    check(no_json == L"{\"ok\":true,\"op\":\"getsrc\",\"type\":\"Strategy\",\"name\":\"NoSuchItem\","
                     L"\"tsdev_pid\":1234,\"elapsed_ms\":567,\"exists\":false,\"source\":null,\"error\":null}",
          "getsrc NotFound json shape (source null + exists false)");

    // getsrc success with --output: source + output_path.
    OpResult src_out;
    src_out.ok = true;
    src_out.op = L"getsrc";
    src_out.type = L"Strategy";
    src_out.name = L"MyTestStrategyA";
    src_out.has_pid = true;
    src_out.pid = 1234;
    src_out.elapsed_ms = 567;
    src_out.reports_exists = true;
    src_out.exists_value = true;
    src_out.reports_source = true;
    src_out.source = L"Buy next bar at market;\n";
    src_out.output_path = L"C:\\path\\to\\file.eld";
    const std::wstring out_json = make_result_json(src_out);
    check(out_json == L"{\"ok\":true,\"op\":\"getsrc\",\"type\":\"Strategy\",\"name\":\"MyTestStrategyA\","
                      L"\"tsdev_pid\":1234,\"elapsed_ms\":567,\"exists\":true,\"source\":\"Buy next bar at market;\\n\","
                      L"\"output_path\":\"C:\\\\path\\\\to\\\\file.eld\",\"error\":null}",
          "getsrc success json shape with output_path");

    // getsrc NotFound with --output: no output_path emitted (no file written).
    OpResult src_no_out;
    src_no_out.ok = true;
    src_no_out.op = L"getsrc";
    src_no_out.type = L"Strategy";
    src_no_out.name = L"NoSuchItem";
    src_no_out.reports_exists = true;
    src_no_out.exists_value = false;
    src_no_out.reports_source = true;
    src_no_out.source.clear();
    src_no_out.output_path = L"C:\\should\\not\\appear.eld";
    const std::wstring no_out_json = make_result_json(src_no_out);
    check(no_out_json.find(L"output_path") == std::wstring::npos,
          "getsrc NotFound json omits output_path (no file written)");
}

void test_parse_args_insert() {
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyNewFn"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\src\\fn.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(p.ok && opts.verb == L"insert" && opts.type == L"Function" && opts.type_number == 1 &&
                  opts.name == L"MyNewFn" && opts.source_path == L"C:\\src\\fn.eld",
              "insert with type+name+source parses");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "insert without --source is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with open is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"exists"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with exists is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with getsrc is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"PaintBar"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "insert with unsupported type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Bad,Name"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "insert with name containing comma is rejected");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Bad\"Name"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "insert with name containing quote is rejected");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source")};
        const ParseOutcome p = parse_args(7, argv, opts);
        check(!p.ok, "insert --source without a value is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"insert"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"TradingApp"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"App1"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld"),
                           const_cast<wchar_t*>(L"--json")};
        const ParseOutcome p = parse_args(9, argv, opts);
        check(p.ok && opts.verb == L"insert" && opts.type_number == 5122 && opts.json,
              "insert with TradingApp and --json parses");
    }
}

void test_parse_args_modify() {
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyFn"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\src\\fn.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(p.ok && opts.verb == L"modify" && opts.type == L"Function" && opts.type_number == 1 &&
                  opts.name == L"MyFn" && opts.source_path == L"C:\\src\\fn.eld",
              "modify with type+name+source parses");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "modify without --source is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"open"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with open is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"exists"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with exists is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"getsrc"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with getsrc is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"PaintBar"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "modify with unsupported type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Bad,Name"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "modify with name containing comma is rejected");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Bad\"Name"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "modify with name containing quote is rejected");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source")};
        const ParseOutcome p = parse_args(7, argv, opts);
        check(!p.ok, "modify --source without a value is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld"),
                           const_cast<wchar_t*>(L"--output"), const_cast<wchar_t*>(L"C:\\o.eld")};
        const ParseOutcome p = parse_args(10, argv, opts);
        check(!p.ok, "--output with modify is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"modify"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"TradingApp"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"App1"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld"),
                           const_cast<wchar_t*>(L"--json")};
        const ParseOutcome p = parse_args(9, argv, opts);
        check(p.ok && opts.verb == L"modify" && opts.type_number == 5122 && opts.json,
              "modify with TradingApp and --json parses");
    }
}

void test_parse_args_delete() {
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyFn")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(p.ok && opts.verb == L"delete" && opts.type == L"Function" && opts.type_number == 1 &&
                  opts.name == L"MyFn" && opts.source_path.empty(),
              "delete with type+name parses, no source required");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Strategy")};
        const ParseOutcome p = parse_args(4, argv, opts);
        check(!p.ok, "delete without --name is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(4, argv, opts);
        check(!p.ok, "delete without --type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--source"), const_cast<wchar_t*>(L"C:\\s.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--source with delete is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"ShowMe"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"MyShow"),
                           const_cast<wchar_t*>(L"--json")};
        const ParseOutcome p = parse_args(7, argv, opts);
        check(p.ok && opts.verb == L"delete" && opts.type_number == 1282 && opts.json,
              "delete with ShowMe and --json parses");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"PaintBar"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "delete with unsupported type is a usage error");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"Bad,Name")};
        const ParseOutcome p = parse_args(6, argv, opts);
        check(!p.ok, "delete with name containing comma is rejected");
    }
    {
        Options opts;
        wchar_t* argv[] = {const_cast<wchar_t*>(L"tsdevctl"), const_cast<wchar_t*>(L"delete"),
                           const_cast<wchar_t*>(L"--type"), const_cast<wchar_t*>(L"Function"),
                           const_cast<wchar_t*>(L"--name"), const_cast<wchar_t*>(L"X"),
                           const_cast<wchar_t*>(L"--output"), const_cast<wchar_t*>(L"C:\\o.eld")};
        const ParseOutcome p = parse_args(8, argv, opts);
        check(!p.ok, "--output with delete is a usage error");
    }
}

void test_usage_contains_delete() {
    const std::wstring usage = usage_text();
    check(usage.find(L"delete") != std::wstring::npos, "usage text contains delete");
    check(usage.find(L"tsdevctl delete") != std::wstring::npos, "usage text has delete usage line");
}

void test_json_delete() {
    // delete success
    OpResult del_ok;
    del_ok.ok = true;
    del_ok.op = L"delete";
    del_ok.type = L"Function";
    del_ok.name = L"MyFn";
    del_ok.has_pid = true;
    del_ok.pid = 42;
    del_ok.elapsed_ms = 500;
    const std::wstring ok_json = make_result_json(del_ok);
    check(ok_json == L"{\"ok\":true,\"op\":\"delete\",\"type\":\"Function\",\"name\":\"MyFn\","
                     L"\"tsdev_pid\":42,\"elapsed_ms\":500,\"error\":null}",
          "delete success json shape");

    // delete not found
    OpResult del_no;
    del_no.ok = false;
    del_no.op = L"delete";
    del_no.type = L"Function";
    del_no.name = L"Missing";
    del_no.error_code = 1;
    del_no.error_name = L"not_found";
    del_no.error_message = L"item does not exist in the TSDev library";
    del_no.elapsed_ms = 100;
    const std::wstring no_json = make_result_json(del_no);
    check(no_json == L"{\"ok\":false,\"op\":\"delete\",\"type\":\"Function\",\"name\":\"Missing\","
                      L"\"tsdev_pid\":null,\"elapsed_ms\":100,"
                      L"\"error\":{\"code\":1,\"name\":\"not_found\",\"message\":\"item does not exist in the TSDev library\"}}",
          "delete not-found json shape");

    // delete error (injection failed)
    OpResult del_err;
    del_err.ok = false;
    del_err.op = L"delete";
    del_err.type = L"Strategy";
    del_err.name = L"X";
    del_err.error_code = 10;
    del_err.error_name = L"injection_failed";
    del_err.error_message = L"OpenProcess failed";
    del_err.elapsed_ms = 50;
    const std::wstring err_json = make_result_json(del_err);
    check(err_json == L"{\"ok\":false,\"op\":\"delete\",\"type\":\"Strategy\",\"name\":\"X\","
                       L"\"tsdev_pid\":null,\"elapsed_ms\":50,"
                       L"\"error\":{\"code\":10,\"name\":\"injection_failed\",\"message\":\"OpenProcess failed\"}}",
          "delete error json shape");
}

void test_from_utf8() {
    check(from_utf8("") == L"", "from_utf8 empty passthrough");
    check(from_utf8("hello") == L"hello", "from_utf8 ASCII");
    check(from_utf8("\xC3\xA9") == L"\u00E9", "from_utf8 two-byte (e-acute)");
    // Round-trip: to_utf8 then from_utf8 should be identity.
    const std::wstring original = L"Buy next bar at market;\n// \u00E9\u00FC\u4e2d";
    const std::string utf8 = to_utf8(original);
    check(from_utf8(utf8) == original, "from_utf8 round-trips through to_utf8");
}

}  // namespace

int wmain() {
    test_type_table();
    test_type_table_new_cmd();
    test_parse_args();
    test_parse_args_exists();
    test_parse_args_getsrc();
    test_parse_args_insert();
    test_parse_args_modify();
    test_parse_args_delete();
    test_usage_contains_delete();
    test_json_delete();
    {
        // Verify get_export_rva finds ReArmHook in delete_hook.dll (next to the test exe).
        wchar_t exe_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        std::wstring dll_path(exe_dir);
        const size_t slash = dll_path.find_last_of(L'\\');
        if (slash != std::wstring::npos) dll_path = dll_path.substr(0, slash + 1) + L"delete_hook.dll";
        else dll_path = L"delete_hook.dll";
        const unsigned long rva = get_export_rva(dll_path, "ReArmHook");
        check(rva != 0, "get_export_rva finds ReArmHook");
        if (rva != 0) {
            wchar_t buf[64];
            swprintf_s(buf, L"ReArmHook RVA = 0x%lX", rva);
            std::wprintf(L"%ls\n", buf);
        }
    }
    test_phantom_classifier();
    test_json();
    test_json_getsrc();
    test_from_utf8();
    if (failures == 0) {
        std::wprintf(L"all unit tests passed\n");
        return 0;
    }
    std::wprintf(L"%d unit test(s) failed\n", failures);
    return 1;
}
