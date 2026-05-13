#include "file_system.h"

sdmmc_card_t *card = NULL;
static uint8_t sdmmc_mount_flag = 0x00;
static bool assets_mount_flag = false;

esp_err_t sdmmc_init(void)
{
    esp_err_t ret = ESP_OK;

    if (sdmmc_mount_flag == 0x01 && card != NULL) {
        sdmmc_unmount();
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 4 * 1024,
    };

    sdmmc_host_t sdmmc_host = SDMMC_HOST_DEFAULT();
    sdmmc_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t sdmmc_config = {
        .clk = SDMMC_PIN_CLK,
        .cmd = SDMMC_PIN_CMD,
        .d0 = SDMMC_PIN_D0,
        .d1 = SDMMC_PIN_D1,
        .d2 = SDMMC_PIN_D2,
        .d3 = SDMMC_PIN_D3,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };

    ret = esp_vfs_fat_sdmmc_mount(mount_point, &sdmmc_host, &sdmmc_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(
                sdmmc_tag,
                "Failed to mount filesystem. If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(
                sdmmc_tag,
                "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors in place.",
                esp_err_to_name(ret));
        }

        sdmmc_mount_flag = 0xFF;
        return ESP_FAIL;
    }

    sdmmc_mount_flag = 0x01;
    return ESP_OK;
}

esp_err_t sdmmc_unmount(void)
{
    ESP_ERROR_CHECK(esp_vfs_fat_sdcard_unmount(mount_point, card));
    sdmmc_mount_flag = 0x00;
    return ESP_OK;
}

esp_err_t assets_fs_init(void)
{
    if (assets_mount_flag) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = assets_mount_point,
        .partition_label = assets_partition_label,
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE("assets", "SPIFFS partition '%s' not found", assets_partition_label);
        } else if (ret == ESP_FAIL) {
            ESP_LOGE("assets", "failed to mount SPIFFS partition '%s'", assets_partition_label);
        } else {
            ESP_LOGE("assets", "failed to initialize SPIFFS partition '%s': %s", assets_partition_label, esp_err_to_name(ret));
        }
        return ret;
    }

    assets_mount_flag = true;
    assets_fs_print_info();
    return ESP_OK;
}

void assets_fs_print_info(void)
{
    size_t total = 0;
    size_t used = 0;
    esp_err_t ret = esp_spiffs_info(assets_partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW("assets", "failed to read SPIFFS info: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI("assets", "mounted %s: total=%u used=%u", assets_mount_point, (unsigned)total, (unsigned)used);
}
