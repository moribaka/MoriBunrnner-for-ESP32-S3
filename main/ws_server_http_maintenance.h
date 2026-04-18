#ifndef WS_SERVER_HTTP_MAINTENANCE_H
#define WS_SERVER_HTTP_MAINTENANCE_H

#include "ws_server_internal.h"

esp_err_t burner_web_main_upload_handler(httpd_req_t *req);
esp_err_t burner_web_upload_handler(httpd_req_t *req);
esp_err_t burner_fw_upgrade_handler(httpd_req_t *req);
esp_err_t burner_mcu_probe_handler(httpd_req_t *req);
esp_err_t burner_cart_id_debug_handler(httpd_req_t *req);
esp_err_t burner_cart_id_handler(httpd_req_t *req);
esp_err_t burner_cart_unlock_ppb_handler(httpd_req_t *req);
esp_err_t burner_status_handler(httpd_req_t *req);
esp_err_t burner_cancel_handler(httpd_req_t *req);
void burner_schedule_restart(void);

#endif
