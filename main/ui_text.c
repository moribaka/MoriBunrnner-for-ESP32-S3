#include "ui_text.h"

#include <string.h>

#include "ui.h"

static const char *ui_text_lookup_in_table(const ui_text_entry_t *table, size_t count, const char *key)
{
    size_t left = 0;
    size_t right = count;

    while (left < right) {
        size_t mid = left + ((right - left) / 2U);
        int cmp = strcmp(key, table[mid].key);
        if (cmp == 0) {
            return table[mid].value;
        }
        if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1U;
        }
    }
    return key;
}

const char *ui_text_lookup(uint8_t language, const char *key)
{
    if (key == NULL || key[0] == '\0') {
        return "";
    }
    if (language == UI_LANGUAGE_ZH) {
        return ui_text_lookup_in_table(g_ui_text_zh, g_ui_text_zh_count, key);
    }
    return ui_text_lookup_in_table(g_ui_text_en, g_ui_text_en_count, key);
}
