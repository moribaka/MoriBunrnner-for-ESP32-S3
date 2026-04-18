#ifndef WS_SERVER_HTTP_DEVICE_H
#define WS_SERVER_HTTP_DEVICE_H

#include "ws_server_internal.h"

esp_err_t burner_power_charge_current_handler(httpd_req_t *req);
esp_err_t burner_power_status_handler(httpd_req_t *req);
esp_err_t burner_device_info_handler(httpd_req_t *req);
esp_err_t burner_device_restart_handler(httpd_req_t *req);
esp_err_t burner_device_brightness_get_handler(httpd_req_t *req);
esp_err_t burner_device_brightness_post_handler(httpd_req_t *req);
esp_err_t burner_storage_status_handler(httpd_req_t *req);
esp_err_t burner_storage_usb_msc_handler(httpd_req_t *req);
esp_err_t burner_lang_handler(httpd_req_t *req);
esp_err_t burner_lang_list_handler(httpd_req_t *req);
esp_err_t burner_lang_apply_handler(httpd_req_t *req);
esp_err_t burner_wifi_status_handler(httpd_req_t *req);
esp_err_t burner_wifi_scan_handler(httpd_req_t *req);
esp_err_t burner_wifi_connect_handler(httpd_req_t *req);
esp_err_t burner_wifi_ap_handler(httpd_req_t *req);
esp_err_t burner_wifi_disconnect_handler(httpd_req_t *req);
esp_err_t burner_wifi_forget_handler(httpd_req_t *req);
esp_err_t burner_web_upload_file(httpd_req_t *req, const char *default_name, bool require_name_query);

#endif
