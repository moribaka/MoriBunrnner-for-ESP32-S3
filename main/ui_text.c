#include "ui_text.h"

#include <string.h>

#include "ui.h"

static const ui_text_entry_t s_ui_text_zh_extra[] = {
    {"A pause/resume  L/R prev/next", "A\u6682\u505c/\u7ee7\u7eed  L/R\u4e0a\u4e00\u9996/\u4e0b\u4e00\u9996"},
    {"A play/pause  B back", "A\u64ad\u653e/\u6682\u505c  B\u8fd4\u56de"},
    {"Start list  A play/pause  B back", "Start\u5217\u8868  A\u64ad\u653e/\u6682\u505c  B\u8fd4\u56de"},
    {"Music", "\u97f3\u4e50"},
    {"Next", "\u4e0b\u4e00\u9996"},
    {"Now Playing", "\u6b63\u5728\u64ad\u653e"},
    {"Play/Pause", "\u64ad\u653e/\u6682\u505c"},
    {"Prev", "\u4e0a\u4e00\u9996"},
    {"Track", "\u66f2\u76ee"},
    {"VOL +/- volume  B back", "\u97f3\u91cf+/-\u8c03\u8282  B\u8fd4\u56de"},
    {"Volume", "\u97f3\u91cf"},
    {"loading", "\u52a0\u8f7d\u4e2d"},
    {"mp3 player", "MP3\u64ad\u653e\u5668"},
    {"paused", "\u5df2\u6682\u505c"},
    {"playback start failed", "\u64ad\u653e\u542f\u52a8\u5931\u8d25"},
    {"playing", "\u64ad\u653e\u4e2d"},
    {"volume down", "\u97f3\u91cf\u964d\u4f4e"},
    {"volume up", "\u97f3\u91cf\u63d0\u9ad8"},
};

static const char *ui_text_lookup_in_table(const ui_text_entry_t *table, size_t count, const char *key)
{
    if (table == NULL || key == NULL) {
        return key;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(key, table[i].key) == 0) {
            return table[i].value;
        }
    }
    return key;
}

const char *ui_text_lookup(uint8_t language, const char *key)
{
    const char *translated = NULL;

    if (key == NULL || key[0] == '\0') {
        return "";
    }
    if (language == UI_LANGUAGE_ZH) {
        translated = ui_text_lookup_in_table(
            s_ui_text_zh_extra,
            sizeof(s_ui_text_zh_extra) / sizeof(s_ui_text_zh_extra[0]),
            key);
        if (translated != key) {
            return translated;
        }
        return ui_text_lookup_in_table(g_ui_text_zh, g_ui_text_zh_count, key);
    }
    return ui_text_lookup_in_table(g_ui_text_en, g_ui_text_en_count, key);
}
