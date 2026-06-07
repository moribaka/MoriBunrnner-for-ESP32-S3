#include "ws_server_internal.h"
#include "ws_server_http_content.h"
#include "ws_server_http_device.h"
#include "ws_server_http_maintenance.h"
#include "ws_server_http_burn.h"

esp_err_t web_ws_start(ws_cfg_t *cfg)
{
    esp_err_t err;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = burner_root_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t sys_page_uri = {
        .uri = "/sys",
        .method = HTTP_GET,
        .handler = burner_sys_page_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_page_uri = {
        .uri = "/tf",
        .method = HTTP_GET,
        .handler = burner_tf_page_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t cart_uri = {
        .uri = "/cart",
        .method = HTTP_GET,
        .handler = burner_business_page_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t burner_uri = {
        .uri = "/burner",
        .method = HTTP_GET,
        .handler = burner_business_page_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t settings_uri = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = burner_settings_page_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = burner_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t cancel_uri = {
        .uri = "/api/cancel",
        .method = HTTP_POST,
        .handler = burner_cancel_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t spi_config_get_uri = {
        .uri = "/api/spi/config",
        .method = HTTP_GET,
        .handler = burner_spi_config_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t core_config_get_uri = {
        .uri = "/api/burn/core_config",
        .method = HTTP_GET,
        .handler = burner_core_config_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t core_config_post_uri = {
        .uri = "/api/burn/core_config",
        .method = HTTP_POST,
        .handler = burner_core_config_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = burner_upload_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t write_uri = {
        .uri = "/api/write",
        .method = HTTP_POST,
        .handler = burner_write_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t read_uri = {
        .uri = "/api/read",
        .method = HTTP_POST,
        .handler = burner_read_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t verify_uri = {
        .uri = "/api/verify",
        .method = HTTP_POST,
        .handler = burner_verify_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ram_write_uri = {
        .uri = "/api/ram/write",
        .method = HTTP_POST,
        .handler = burner_ram_write_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ram_read_uri = {
        .uri = "/api/ram/read",
        .method = HTTP_POST,
        .handler = burner_ram_read_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ram_verify_uri = {
        .uri = "/api/ram/verify",
        .method = HTTP_POST,
        .handler = burner_ram_verify_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t cart_id_uri = {
        .uri = "/api/cart/id",
        .method = HTTP_GET,
        .handler = burner_cart_id_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t cart_id_debug_uri = {
        .uri = "/api/cart/id_debug",
        .method = HTTP_GET,
        .handler = burner_cart_id_debug_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t cart_erase_uri = {
        .uri = "/api/cart/erase",
        .method = HTTP_POST,
        .handler = burner_cart_erase_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t cart_unlock_ppb_uri = {
        .uri = "/api/cart/unlock_ppb",
        .method = HTTP_POST,
        .handler = burner_cart_unlock_ppb_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t gbx_cache_rebuild_uri = {
        .uri = "/api/gbx/cache/rebuild",
        .method = HTTP_POST,
        .handler = burner_gbx_cache_rebuild_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t gbx_profiles_uri = {
        .uri = "/api/gbx/profiles",
        .method = HTTP_GET,
        .handler = burner_gbx_profiles_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_list_uri = {
        .uri = "/api/tf/list",
        .method = HTTP_GET,
        .handler = burner_tf_list_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_upload_uri = {
        .uri = "/api/tf/upload",
        .method = HTTP_POST,
        .handler = burner_tf_upload_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_download_uri = {
        .uri = "/api/tf/download",
        .method = HTTP_GET,
        .handler = burner_tf_download_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t system_migrate_zip_uri = {
        .uri = "/api/system/migrate_zip",
        .method = HTTP_GET,
        .handler = burner_system_migrate_zip_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t system_deploy_zip_uri = {
        .uri = "/api/system/deploy_zip",
        .method = HTTP_POST,
        .handler = burner_system_deploy_zip_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_delete_uri = {
        .uri = "/api/tf/delete",
        .method = HTTP_DELETE,
        .handler = burner_tf_delete_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_rename_uri = {
        .uri = "/api/tf/rename",
        .method = HTTP_POST,
        .handler = burner_tf_rename_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tf_mkdir_uri = {
        .uri = "/api/tf/mkdir",
        .method = HTTP_POST,
        .handler = burner_tf_mkdir_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t device_info_uri = {
        .uri = "/api/device/info",
        .method = HTTP_GET,
        .handler = burner_device_info_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t device_restart_uri = {
        .uri = "/api/device/restart",
        .method = HTTP_POST,
        .handler = burner_device_restart_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t device_brightness_get_uri = {
        .uri = "/api/device/brightness",
        .method = HTTP_GET,
        .handler = burner_device_brightness_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t device_brightness_post_uri = {
        .uri = "/api/device/brightness",
        .method = HTTP_POST,
        .handler = burner_device_brightness_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t power_status_uri = {
        .uri = "/api/power/status",
        .method = HTTP_GET,
        .handler = burner_power_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t power_charge_current_uri = {
        .uri = "/api/power/charge_current",
        .method = HTTP_POST,
        .handler = burner_power_charge_current_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t storage_status_uri = {
        .uri = "/api/storage/status",
        .method = HTTP_GET,
        .handler = burner_storage_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t storage_usb_uri = {
        .uri = "/api/storage/usb_msc",
        .method = HTTP_POST,
        .handler = burner_storage_usb_msc_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t lang_uri = {
        .uri = "/api/lang",
        .method = HTTP_GET,
        .handler = burner_lang_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t lang_list_uri = {
        .uri = "/api/lang/list",
        .method = HTTP_GET,
        .handler = burner_lang_list_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t lang_apply_uri = {
        .uri = "/api/lang/apply",
        .method = HTTP_POST,
        .handler = burner_lang_apply_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_status_uri = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = burner_wifi_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = burner_wifi_scan_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_connect_uri = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = burner_wifi_connect_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_ap_uri = {
        .uri = "/api/wifi/ap",
        .method = HTTP_POST,
        .handler = burner_wifi_ap_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_disconnect_uri = {
        .uri = "/api/wifi/disconnect",
        .method = HTTP_POST,
        .handler = burner_wifi_disconnect_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_forget_uri = {
        .uri = "/api/wifi/forget",
        .method = HTTP_POST,
        .handler = burner_wifi_forget_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t web_main_upload_uri = {
        .uri = "/api/web/main_html",
        .method = HTTP_POST,
        .handler = burner_web_main_upload_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t web_upload_uri = {
        .uri = "/api/web/upload",
        .method = HTTP_POST,
        .handler = burner_web_upload_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t mcu_probe_uri = {
        .uri = "/api/mcu/probe",
        .method = HTTP_GET,
        .handler = burner_mcu_probe_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = burner_static_handler,
        .user_ctx = NULL,
    };

    if (s_httpd != NULL) {
        return ESP_OK;
    }

    s_upload_html_override = (cfg != NULL) ? cfg->html_code : NULL;
    s_receive_cb = (cfg != NULL) ? cfg->receive_fn : NULL;

    err = burner_backend_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_reset();
    xSemaphoreGive(s_status_lock);

    config.max_uri_handlers = 61;
    config.stack_size = WEB_HTTPD_STACK_SIZE;
    config.core_id = WEB_HTTPD_CORE_ID;
    config.task_priority = WEB_HTTPD_TASK_PRIORITY;
    if (esp_psram_is_initialized()) {
        config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    // Browser WebSocket handshake headers can be longer than default.
    config.max_req_hdr_len = 4096;
    config.max_uri_len = 512;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }

    err = httpd_register_uri_handler(s_httpd, &root_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &sys_page_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_page_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &cart_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &burner_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &settings_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &status_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &cancel_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &spi_config_get_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &core_config_get_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &core_config_post_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &upload_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &write_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &read_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &verify_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &ram_write_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &ram_read_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &ram_verify_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &cart_id_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &cart_id_debug_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &cart_erase_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &cart_unlock_ppb_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &gbx_cache_rebuild_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &gbx_profiles_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_list_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_upload_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_download_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &system_migrate_zip_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &system_deploy_zip_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_delete_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_rename_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &tf_mkdir_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &device_info_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &device_restart_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &device_brightness_get_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &device_brightness_post_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &power_status_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &power_charge_current_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &storage_status_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &storage_usb_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &lang_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &lang_list_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &lang_apply_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &wifi_status_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &wifi_scan_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &wifi_connect_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &wifi_ap_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &wifi_disconnect_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &wifi_forget_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &web_main_upload_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &web_upload_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &mcu_probe_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }
    err = httpd_register_uri_handler(s_httpd, &static_uri);
    if (err != ESP_OK) {
        web_ws_stop();
        return err;
    }

    ESP_LOGI(BURNER_TAG, "Burner web portal started. Open http://<esp-ip>/");
    return ESP_OK;
}

esp_err_t web_ws_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    return ESP_OK;
}

esp_err_t web_ws_send(uint8_t *data, int len)
{
    burner_status_t snap;
    char msg[80];

    if (data == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= (int)sizeof(msg)) {
        len = (int)sizeof(msg) - 1;
    }
    memcpy(msg, data, (size_t)len);
    msg[len] = '\0';

    burner_status_snapshot(&snap);
    burner_status_update(
        BURNER_STATE_BURNING,
        snap.progress,
        snap.processed_bytes,
        snap.total_bytes,
        msg,
        NULL,
        NULL);
    return ESP_OK;
}

void web_ws_mark_activity(void)
{
    ui_mark_activity();
}
