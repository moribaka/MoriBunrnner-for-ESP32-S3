#ifndef WS_SERVER_HTTP_BURN_H
#define WS_SERVER_HTTP_BURN_H

#include "ws_server_internal.h"

esp_err_t burner_upload_handler(httpd_req_t *req);
esp_err_t burner_write_handler(httpd_req_t *req);
esp_err_t burner_read_handler(httpd_req_t *req);
esp_err_t burner_verify_handler(httpd_req_t *req);
esp_err_t burner_ram_write_handler(httpd_req_t *req);
esp_err_t burner_ram_read_handler(httpd_req_t *req);
esp_err_t burner_ram_verify_handler(httpd_req_t *req);
esp_err_t burner_cart_erase_handler(httpd_req_t *req);

#endif
