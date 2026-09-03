#include "type_table.h"

bool type_from_name(const std::wstring& name, int& number, int& new_cmd_id, std::wstring& label) {
    for (const TsdevType& t : kTypes) {
        if (name == t.name) {
            number = t.number;
            new_cmd_id = t.new_cmd_id;
            label = t.label;
            return true;
        }
    }
    return false;
}
