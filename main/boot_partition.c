#include "boot_partition.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "music_player.h"
#include "power_manager.h"
#include "smb_client.h"
#include "usb_msc_tf.h"
#include "wifi_manager.h"
#include "tca9555.h"
#include "burner/core/ws_server_internal.h"

static const char *BOOT_PARTITION_TAG = "boot_partition";

static void boot_partition_quiesce_services(void)
{
    esp_err_t err;

    err = music_player_stop();
    (void)err;
    (void)tca9555_pin_write(TCA9555_IO1_4, false);
    if (s_mcu_spi_ready && !burner_spi_swd_restore_blocked()) {
        burner_spi_lock_take();
        err = burner_bacon_gba_release_bus_idle();
        if (err == ESP_OK) err = burner_bacon_gba_power_cmd(false, false);
        burner_spi_lock_give();
        if (err != ESP_OK) ESP_LOGW(BOOT_PARTITION_TAG, "cart park failed: %s", esp_err_to_name(err));
    }

    smb_client_disconnect();

    err = web_ws_stop();
    (void)err;

    err = wifi_maneger_shutdown_for_reboot();
    (void)err;

    if (usb_msc_tf_ready() && usb_msc_tf_enabled()) {
        err = usb_msc_tf_set_enabled(false);
        (void)err;
    }
}

esp_err_t boot_partition_switch_to(const char *partition_label)
{
    const esp_partition_t *target = NULL;
    esp_err_t err;
    bool perf_lock_acquired = false;

    if (partition_label == NULL || partition_label[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (burner_task_is_running_snapshot()) {
        return ESP_ERR_INVALID_STATE;
    }

    target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_ANY,
        partition_label);
    if (target == NULL) {
        ESP_LOGE(BOOT_PARTITION_TAG, "partition '%s' not found", partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    if (power_manager_perf_lock_acquire("boot_partition") == ESP_OK) {
        perf_lock_acquired = true;
    }

    ESP_LOGI(BOOT_PARTITION_TAG, "quiescing services before boot switch to '%s'", partition_label);
    boot_partition_quiesce_services();

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(
            BOOT_PARTITION_TAG,
            "set boot partition '%s' failed: %s",
            partition_label,
            esp_err_to_name(err));
        if (perf_lock_acquired) {
            power_manager_perf_lock_release("boot_partition");
        }
        return err;
    }

    esp_restart();
    return ESP_OK;
}
