#pragma once
#include <cstdio>
#include <string>


// TSDev's own type-category table (ORELUI20.dll DAT_1008fd04, 13 entries),
// used to reverse-map an enumerated item's full type — the (category,
// subtype) pair — to a label. SSOT for the table:
// docs/KNOWLEDGE_BASE.md, section "ORELUI20.dll + ORLIB20.dll ordinals"
// (dynamically confirmed, TVS #7). Full type = (subtype << 8) | category.
struct ListTypeEntry {
    unsigned short category;
    unsigned short subtype;
    const wchar_t* label;
};

inline constexpr ListTypeEntry kListTypes[13] = {
    {0x02, 20, L"TradingApp"},
    {0x02, 0,  L"Indicator"},
    {0x02, 5,  L"ShowMe"},
    {0x02, 4,  L"PaintBar"},
    {0x02, 10, L"VolatilityModel"},
    {0x02, 12, L"PricingModel"},
    {0x02, 14, L"BidAskModel"},
    {0x02, 16, L"SearchStrategy"},
    {0x02, 18, L"ProbabilityMap"},
    {0x00, 0,  L"Strategy"},
    {0x03, 0,  L"ActivityBar"},
    {0x01, 0,  L"Function"},
    {0x04, 0,  L"Function (built-in)"},
};

// Reverse-maps the full type (category, subtype) to TSDev's own type label.
// Unknown pairs get a stable numeric fallback label derived from the packed
// full type — never empty, never guessed from item content.
inline std::wstring list_type_label(unsigned short category, unsigned short subtype) {
    const unsigned packed = (static_cast<unsigned>(subtype) << 8) | category;
    for (const ListTypeEntry& e : kListTypes) {
        if (e.category == category && e.subtype == subtype) return e.label;
    }
    wchar_t buf[16];
    swprintf_s(buf, L"Type(0x%04X)", packed);
    return buf;
}

// True when the record's full type ((subtype << 8) | category) equals
// type_number (from type_table.h: Function=1, Indicator=2, ShowMe=0x0502,
// TradingApp=0x1402, Strategy=0). Note the built-in-Functions category-table
// entry (category 4, packed 0x0004) is a distinct full type and is therefore
// NOT matched by --type Function.
inline bool list_record_matches(unsigned short category, unsigned short subtype, int type_number) {
    return static_cast<int>((static_cast<unsigned>(subtype) << 8) | category) == type_number;
}
