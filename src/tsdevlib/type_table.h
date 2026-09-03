#pragma once
#include <string>

struct TsdevType {
    const wchar_t* name;
    int number;
    const wchar_t* label;
};

// Only types with a pipe-verified number are listed; extend after verifying
// the remaining mappings (see docs/KNOWLEDGE_BASE.md).
inline constexpr TsdevType kTypes[] = {
    {L"Strategy", 0, L"Strategy"},
    {L"Function", 1, L"Function"},
};

bool type_from_name(const std::wstring& name, int& number, std::wstring& label);
