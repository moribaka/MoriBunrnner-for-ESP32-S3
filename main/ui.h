#ifndef UI_H
#define UI_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    UI_WIFI_STATE_UNKNOWN = 0,
    UI_WIFI_STATE_DISCONNECTED,
    UI_WIFI_STATE_PROVISIONING,
    UI_WIFI_STATE_CONNECTED,
} ui_wifi_state_t;

esp_err_t ui_init(void);
void ui_process(void);
void ui_set_wifi_state(ui_wifi_state_t state);
void ui_set_ip_text(const char *ip);
void ui_set_burn_progress(int progress, uint32_t processed, uint32_t total);
void ui_set_status_text(const char *text);

#endif /* UI_H */
