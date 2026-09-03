#include "type_table.h"

bool type_from_name(const std::wstring& name, int& number, std::wstring& label) {
    for (const TsdevType& t : kTypes) {
        if (name == t.name) {
            number = t.number;
            label = t.label;
            return true;
        }
    }
    return false;
}
