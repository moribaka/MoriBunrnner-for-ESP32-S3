#ifndef UI_TEXT_H
#define UI_TEXT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *key;
    const char *value;
} ui_text_entry_t;

extern const ui_text_entry_t g_ui_text_en[];
extern const size_t g_ui_text_en_count;
extern const ui_text_entry_t g_ui_text_zh[];
extern const size_t g_ui_text_zh_count;

const char *ui_text_lookup(uint8_t language, const char *key);

#endif /* UI_TEXT_H */
