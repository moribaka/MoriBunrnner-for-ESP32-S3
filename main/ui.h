#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    UI_WIFI_STATE_UNKNOWN = 0,
    UI_WIFI_STATE_DISCONNECTED,
    UI_WIFI_STATE_PROVISIONING,
    UI_WIFI_STATE_CONNECTED,
} ui_wifi_state_t;

typedef enum {
    UI_BUTTON_LEFT = 0,
    UI_BUTTON_RIGHT,
    UI_BUTTON_UP,
    UI_BUTTON_DOWN,
    UI_BUTTON_SELECT,
    UI_BUTTON_PANEL_TOGGLE,
    UI_BUTTON_BACK,
    UI_BUTTON_MENU,
    UI_BUTTON_VOL_UP,
    UI_BUTTON_VOL_DOWN,
} ui_button_t;

typedef enum {
    UI_INPUT_ACTION_NONE = 0,
    UI_INPUT_ACTION_LEFT,
    UI_INPUT_ACTION_RIGHT,
    UI_INPUT_ACTION_UP,
    UI_INPUT_ACTION_DOWN,
    UI_INPUT_ACTION_SELECT,
    UI_INPUT_ACTION_PANEL_TOGGLE,
    UI_INPUT_ACTION_BACK,
    UI_INPUT_ACTION_MENU,
    UI_INPUT_ACTION_VOLUME_UP,
    UI_INPUT_ACTION_VOLUME_DOWN,
} ui_input_action_t;

#define UI_LANGUAGE_EN 0U
#define UI_LANGUAGE_ZH 1U
#define UI_LANGUAGE_DEFAULT UI_LANGUAGE_ZH

esp_err_t ui_init(void);
void ui_process(void);
void ui_mark_activity(void);
void ui_mark_network_activity(void);
uint32_t ui_get_last_activity_ms(void);
uint32_t ui_get_last_network_activity_ms(void);
void ui_set_activity_callback(void (*cb)(void));
void ui_handle_button(ui_button_t button, bool pressed);
void ui_post_button(ui_button_t button, bool pressed);
uint8_t ui_get_language(void);
void ui_set_language(uint8_t language);
void ui_set_wifi_state(ui_wifi_state_t state);
void ui_set_ip_text(const char *ip);
void ui_set_burn_progress(int progress, uint32_t processed, uint32_t total);
void ui_show_burn_task_status(uint32_t total_hint);
void ui_set_status_text(const char *text);

#endif /* UI_H */
