#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"

#define wifi_manager_tag "wifi_manager"

typedef enum {
    WIFI_STATE_CONNECTED = 0,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_PROVISIONING,
    WIFI_STATE_PROVISIONING_CONNECTED,
} WIFI_STATE;

typedef void (*p_wifi_state_cb)(WIFI_STATE);
typedef void (*p_wifi_scan_cb)(int num, wifi_ap_record_t *ap_records);

esp_err_t wifi_maneger_init(p_wifi_state_cb f);
bool wifi_maneger_ready(void);
void wifi_maneger_connect(const char *ssid, const char *password);
esp_err_t wifi_maneger_ap(void);
esp_err_t wifi_maneger_scan(p_wifi_scan_cb f);
void wifi_maneger_disconnect(void);

bool wifi_maneger_has_saved_sta(void);
esp_err_t wifi_maneger_save_sta_config(const char *ssid, const char *password);
esp_err_t wifi_maneger_clear_sta_config(void);
esp_err_t wifi_maneger_connect_saved(uint32_t timeout_ms);
esp_err_t wifi_maneger_get_sta_ip(char *ip, size_t ip_len);
bool wifi_maneger_provisioning_waiting_confirm(void);
esp_err_t wifi_maneger_provisioning_confirm(void);
esp_err_t wifi_maneger_provisioning_keep_ap(void);

#endif
