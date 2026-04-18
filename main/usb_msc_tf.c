#include "usb_msc_tf.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "file_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

static const char *TAG = "usb_msc_tf";
static SemaphoreHandle_t s_usb_msc_lock = NULL;
static bool s_usb_msc_ready = false;
static bool s_usb_msc_driver_installed = false;
static bool s_usb_msc_enabled = false;

static void usb_msc_mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event == NULL) {
        return;
    }

    ESP_LOGI(
        TAG,
        "MSC local FS mount changed: %s",
        event->mount_changed_data.is_mounted ? "mounted" : "unmounted");
}

static esp_err_t usb_msc_install_locked(void)
{
    tinyusb_config_t tusb_cfg = {0};
    tinyusb_msc_sdmmc_config_t msc_sdmmc_cfg = {0};
    bool storage_inited = false;
    esp_err_t err = ESP_OK;

    if (s_usb_msc_driver_installed) {
        return ESP_OK;
    }

    if (card == NULL) {
        ESP_LOGW(TAG, "TF card is not ready, skip USB MSC install");
        return ESP_ERR_INVALID_STATE;
    }

    msc_sdmmc_cfg.card = card;
    msc_sdmmc_cfg.callback_mount_changed = usb_msc_mount_changed_cb;

    err = tinyusb_msc_storage_init_sdmmc(&msc_sdmmc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init msc sdmmc failed: %s", esp_err_to_name(err));
        return err;
    }
    storage_inited = true;

    tusb_cfg.device_descriptor = NULL;
    tusb_cfg.string_descriptor = NULL;
    tusb_cfg.string_descriptor_count = 0;
    tusb_cfg.external_phy = false;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.fs_configuration_descriptor = NULL;
    tusb_cfg.hs_configuration_descriptor = NULL;
    tusb_cfg.qualifier_descriptor = NULL;
#else
    tusb_cfg.configuration_descriptor = NULL;
#endif

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install tinyusb driver failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_usb_msc_driver_installed = true;
    ESP_LOGI(TAG, "USB MSC pass-through enabled");
    return ESP_OK;

fail:
    if (storage_inited) {
        tinyusb_msc_storage_deinit();
    }
    return err;
}

static esp_err_t usb_msc_uninstall_locked(void)
{
    esp_err_t err = ESP_OK;

    if (!s_usb_msc_driver_installed) {
        return ESP_OK;
    }

    err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uninstall tinyusb driver failed: %s", esp_err_to_name(err));
        return err;
    }

    tinyusb_msc_storage_deinit();
    s_usb_msc_driver_installed = false;
    ESP_LOGI(TAG, "USB MSC pass-through disabled");
    return ESP_OK;
}

esp_err_t usb_msc_tf_init(void)
{
    if (s_usb_msc_ready) {
        return ESP_OK;
    }

    if (card == NULL) {
        ESP_LOGW(TAG, "TF card is not ready, skip USB MSC manager init");
        return ESP_ERR_INVALID_STATE;
    }

    s_usb_msc_lock = xSemaphoreCreateMutex();
    if (s_usb_msc_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_usb_msc_ready = true;
    s_usb_msc_driver_installed = false;
    s_usb_msc_enabled = false;
    ESP_LOGI(TAG, "USB MSC manager ready (default: disabled)");
    return ESP_OK;
}

bool usb_msc_tf_ready(void)
{
    return s_usb_msc_ready;
}

bool usb_msc_tf_enabled(void)
{
    return s_usb_msc_enabled;
}

esp_err_t usb_msc_tf_set_enabled(bool enabled)
{
    esp_err_t err = ESP_OK;

    if (!s_usb_msc_ready || s_usb_msc_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_usb_msc_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (enabled == s_usb_msc_enabled) {
        xSemaphoreGive(s_usb_msc_lock);
        return ESP_OK;
    }

    if (enabled) {
        err = usb_msc_install_locked();
    } else {
        err = usb_msc_uninstall_locked();
    }

    if (err == ESP_OK) {
        s_usb_msc_enabled = enabled;
    }

    xSemaphoreGive(s_usb_msc_lock);
    return err;
}

bool usb_msc_tf_in_use_by_host(void)
{
    /*
     * Pass-through is treated as "TF busy for firmware".
     * This keeps application-side file operations safe from concurrent USB host writes.
     */
    return s_usb_msc_enabled;
}
