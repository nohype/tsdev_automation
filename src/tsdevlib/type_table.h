#pragma once
#include <string>

struct TsdevType {
    const wchar_t* name;
    int number;
    const wchar_t* label;
    int new_cmd_id;  // WM_COMMAND ID of TSDev's "New <Type>" command (app-class message map).
};

// Only types with a live-verified pipe number are listed (phantom-title probe,
// 2026-09-03; see docs/KNOWLEDGE_BASE.md). ActivityBar (3), PaintBar (1026) and
// ProbabilityMap (4610) are verified too but stay out of product scope (R13).
// new_cmd_id values: TSDev.exe "New <Type>" WM_COMMAND IDs (KB, proven live 2026-09-03).
inline constexpr TsdevType kTypes[] = {
    {L"Strategy", 0, L"Strategy", 0x2950},
    {L"Function", 1, L"Function", 0x2951},
    {L"Indicator", 2, L"Indicator", 0x2947},
    {L"ShowMe", 1282, L"ShowMe", 0x2949},
    {L"TradingApp", 5122, L"Trading Application", 0x3D21},
};

bool type_from_name(const std::wstring& name, int& number, int& new_cmd_id, std::wstring& label);
