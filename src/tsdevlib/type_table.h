#pragma once
#include <string>

struct TsdevType {
    const wchar_t* name;
    int number;
    const wchar_t* label;
};

// Only types with a live-verified pipe number are listed (phantom-title probe,
// 2026-09-03; see docs/KNOWLEDGE_BASE.md). ActivityBar (3), PaintBar (1026) and
// ProbabilityMap (4610) are verified too but stay out of product scope (R13).
inline constexpr TsdevType kTypes[] = {
    {L"Strategy", 0, L"Strategy"},
    {L"Function", 1, L"Function"},
    {L"Indicator", 2, L"Indicator"},
    {L"ShowMe", 1282, L"ShowMe"},
    {L"TradingApp", 5122, L"Trading Application"},
};

bool type_from_name(const std::wstring& name, int& number, std::wstring& label);
