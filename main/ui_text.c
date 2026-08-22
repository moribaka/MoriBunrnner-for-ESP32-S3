#include "ui_text.h"

#include <string.h>

#include "ui.h"

static const ui_text_entry_t s_ui_text_zh_extra[] = {
    {"A pause/resume  L/R prev/next", "A\u64ad\u653e/\u6682\u505c  L/R\u4e0a\u4e00\u9996/\u4e0b\u4e00\u9996"},
    {"A play/pause  B back", "A\u64ad\u653e/\u6682\u505c  B\u8fd4\u56de"},
    {"Select list  A confirm/pause  B back", "Select\u5217\u8868  A\u786e\u8ba4/\u6682\u505c  B\u8fd4\u56de"},
    {"Start list  A play/pause  B back", "Select\u5217\u8868  A\u786e\u8ba4/\u6682\u505c  B\u8fd4\u56de"},
    {"Books", "\u4e66\u5e93"},
    {"Music", "\u97f3\u4e50"},
    {"Reader", "\u9605\u8bfb\u5668"},
    {"booting Retro-Go", "\u6b63\u5728\u542f\u52a8 Retro-Go"},
    {"empty file", "\u7a7a\u6587\u4ef6"},
    {"file too large", "\u6587\u4ef6\u8fc7\u5927"},
    {"fast forward", "\u5feb\u8fdb"},
    {"Next", "\u4e0b\u4e00\u9996"},
    {"Now Playing", "\u6b63\u5728\u64ad\u653e"},
    {"open file failed", "\u6253\u5f00\u6587\u4ef6\u5931\u8d25"},
    {"Play/Pause", "\u64ad\u653e/\u6682\u505c"},
    {"Prev", "\u4e0a\u4e00\u9996"},
    {"Off", "\u5173"},
    {"min", "\u5206\u949f"},
    {"power settings saved", "\u7535\u6e90\u8bbe\u7f6e\u5df2\u4fdd\u5b58"},
    {"settings saved", "\u8bbe\u7f6e\u5df2\u4fdd\u5b58"},
    {"save settings failed", "\u4fdd\u5b58\u8bbe\u7f6e\u5931\u8d25"},
    {"L/R adjust  B back", "L/R\u8c03\u8282  B\u8fd4\u56de"},
    {"hold 1s for x10", "\u6309\u4f4f1\u79d2\u540e10\u683c\u8df3\u8dc3"},
    {"Brightness", "\u4eae\u5ea6"},
    {"read TF files", "\u8bfb\u53d6TF\u6587\u4ef6"},
    {"rewind", "\u5feb\u9000"},
    {"reboot to games", "\u91cd\u542f\u5230\u6e38\u620f\u7cfb\u7edf"},
    {"save power settings failed", "\u4fdd\u5b58\u7535\u6e90\u8bbe\u7f6e\u5931\u8d25"},
    {"Screen auto off", "\u5c4f\u5e55\u81ea\u52a8\u5173\u95ed"},
    {"Track", "\u66f2\u76ee"},
    {"VOL +/- volume  B back", "\u97f3\u91cf+/-\u8c03\u8282  B\u8fd4\u56de"},
    {"Volume", "\u97f3\u91cf"},
    {"Wi-Fi auto off", "Wi-Fi\u81ea\u52a8\u5173\u95ed"},
    {"Web QR", "\u7f51\u9875\u4e8c\u7ef4\u7801"},
    {"View QR", "\u67e5\u770b\u4e8c\u7ef4\u7801"},
    {"Scan web address", "\u626b\u7801\u6253\u5f00\u7f51\u9875"},
    {"Scan to open web", "\u626b\u7801\u6253\u5f00\u5f53\u524d\u8bbe\u5907\u7f51\u9875"},
    {"Connect phone to MORI AP first", "\u5148\u8fde\u63a5\u624b\u673a\u5230 MORI \u70ed\u70b9"},
    {"Start Provision AP first", "\u5148\u5f00\u542f\u914d\u7f51\u70ed\u70b9"},
    {"QR unavailable", "\u4e8c\u7ef4\u7801\u751f\u6210\u5931\u8d25"},
    {"loading", "\u52a0\u8f7d\u4e2d"},
    {"mp3 player", "MP3\u64ad\u653e\u5668"},
    {"paused", "\u5df2\u6682\u505c"},
    {"playback start failed", "\u64ad\u653e\u542f\u52a8\u5931\u8d25"},
    {"playing", "\u64ad\u653e\u4e2d"},
    {"Update GBX cache", "\u66f4\u65b0GBX\u7f13\u5b58"},
    {"volume down", "\u97f3\u91cf\u964d\u4f4e"},
    {"volume up", "\u97f3\u91cf\u63d0\u9ad8"},
    {"Save patch SRAM/FLASH", "\u5b58\u6863\u8865\u4e01 SRAM/FLASH"},
    {"Save patch SRAM/FLASH/None", "\u5b58\u6863\u8865\u4e01 SRAM/FLASH/\u65e0"},
    {"Save patch: SRAM", "\u5b58\u6863\u8865\u4e01: SRAM"},
    {"Save patch: FLASH", "\u5b58\u6863\u8865\u4e01: FLASH"},
    {"Save patch not needed", "\u5b58\u6863\u8865\u4e01\u4e0d\u9700\u8981"},
    {"Batteryless patch disabled", "\u514d\u7535\u8865\u4e01\u5df2\u7981\u7528"},
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
