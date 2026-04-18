#ifndef WS_SERVER_HTTP_CONTENT_H
#define WS_SERVER_HTTP_CONTENT_H

#include "ws_server_internal.h"

esp_err_t burner_static_handler(httpd_req_t *req);
esp_err_t burner_root_handler(httpd_req_t *req);
esp_err_t burner_sys_page_handler(httpd_req_t *req);
esp_err_t burner_business_page_handler(httpd_req_t *req);
esp_err_t burner_tf_page_handler(httpd_req_t *req);
esp_err_t burner_settings_page_handler(httpd_req_t *req);
esp_err_t burner_tf_list_handler(httpd_req_t *req);
esp_err_t burner_tf_upload_handler(httpd_req_t *req);
esp_err_t burner_tf_download_handler(httpd_req_t *req);
esp_err_t burner_system_migrate_zip_handler(httpd_req_t *req);
esp_err_t burner_system_deploy_zip_handler(httpd_req_t *req);
esp_err_t burner_tf_delete_handler(httpd_req_t *req);
esp_err_t burner_tf_mkdir_handler(httpd_req_t *req);
esp_err_t burner_tf_rename_handler(httpd_req_t *req);
const char *burner_json_bool(bool value);
uint16_t burner_ip5306_charge_current_cfg_ma(uint8_t chg_dig_ctl0);
bool burner_ip5306_battery_level_code_known(uint8_t bat_level_raw);

#endif
