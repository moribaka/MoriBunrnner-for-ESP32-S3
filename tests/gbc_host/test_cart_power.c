#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
enum { BURNER_SPI_CS_MODE_0, BURNER_SPI_CS_MODE_1 };
static unsigned s_bacon_power_settle_ms = 100, s_mbc5_power_5v_enabled;
static int events[32], count, fail_at = -1;
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ms) { assert(ms == 100); events[count++] = 1000 + ms; }
static uint8_t burner_bacon_option_byte0(uint8_t batch, bool a, bool ad, bool cs2, bool cs1, bool rd, bool wr) {
    return batch << 6 | a << 5 | ad << 4 | cs2 << 3 | cs1 << 2 | rd << 1 | wr;
}
static esp_err_t burner_spi_transfer_cs(int mode, const uint8_t *tx, void *rx, size_t bytes) {
    assert(rx == NULL && bytes == 1);
    events[count++] = mode * 256 + tx[0];
    return count == fail_at ? ESP_FAIL : ESP_OK;
}
esp_err_t burner_bacon_gba_release_bus_idle(void);
#include "../../main/burner/core/burner_cart_power.inc"
int main(void) {
    s_mbc5_power_5v_enabled = 1;
    assert(burner_bacon_mbc5_prepare_power() == ESP_OK);
    int expected[] = {0x0f, 0x104, 1100, 0x164, 1100};
    assert(count == 5);
    for (int i=0; i<5; ++i) assert(events[i] == expected[i]);
    assert(burner_bacon_cart_power_mv() == 5000);
    count = 0;
    assert(burner_bacon_finish_cart_access() == ESP_OK && count == 1 && events[0] == 0x0f);
    assert(burner_bacon_cart_power_mv() == 5000);
    count = 0;
    assert(burner_bacon_gba_prepare_power() == ESP_OK);
    assert(count == 6 && events[1] == 0x104 && events[2] == 1100 && events[3] == 0x154);
    assert(burner_bacon_cart_power_mv() == 3300);
    count = 0;
    assert(burner_bacon_gba_power_cmd(true, true) == ESP_ERR_INVALID_ARG && count == 0);
    assert(burner_bacon_gba_power_cmd(false, false) == ESP_OK && burner_bacon_cart_power_mv() == 0);
    count = 0; fail_at = 2;
    assert(burner_bacon_mbc5_prepare_power() == ESP_FAIL && count == 2);
    assert(burner_bacon_cart_power_mv() == -1);
    puts("Power sequence: released bus, off/wait/on, preserved 5V, exclusive rails and stop-on-error passed");
}
