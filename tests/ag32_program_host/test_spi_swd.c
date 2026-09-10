#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#define BURNER_SPI_ENABLE 1
#define MORI_PIN_MCU_SWDIO 1
#define MORI_PIN_MCU_SWCLK 2
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_FLOATING 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static bool s_swd_mode, s_swd_restore_blocked;
static int locks, releases, restores, initializations, invalidations;
static int pin_error, restore_error, init_error;
static void burner_spi_lock_take(void) { assert(locks == 0); ++locks; }
static void burner_spi_lock_give(void) { assert(locks == 1); --locks; }
static void burner_spi_release_cs(void) { ++releases; }
static int gpio_config(const gpio_config_t *c) { assert(c->pin_bit_mask == 6); return pin_error; }
static int gpio_set_direction(int a, int b) { (void)a; (void)b; return pin_error; }
static int gpio_set_pull_mode(int a, int b) { (void)a; (void)b; return pin_error; }
static int burner_spi_restore_cs_pins(void) { ++restores; return restore_error; }
static int burner_spi_init(void) { ++initializations; return init_error; }
static void burner_reset_cart_probe_state(void) { ++invalidations; }
static void ag32_mcu_link_invalidate(void) { ++invalidations; }
#include "../../main/burner/core/burner_spi_swd.inc"
int main(void)
{
    assert(burner_spi_enter_swd_mode() == ESP_OK && locks == 1 && s_swd_mode);
    assert(burner_spi_leave_swd_mode(true) == ESP_OK && !locks && !s_swd_mode && invalidations == 2);
    pin_error = ESP_FAIL;
    assert(burner_spi_enter_swd_mode() == ESP_FAIL && !locks && !s_swd_mode && !s_swd_restore_blocked);
    restore_error = ESP_ERR_NO_MEM;
    assert(burner_spi_enter_swd_mode() == ESP_FAIL && !locks && s_swd_mode && s_swd_restore_blocked);
    pin_error = restore_error = 0;
    int released_before = releases, restored_before = restores;
    assert(burner_spi_enter_swd_mode() == ESP_OK && releases == released_before);
    assert(burner_spi_leave_swd_mode(true) == ESP_OK && restores == restored_before && s_swd_mode && !locks);
    assert(burner_spi_enter_swd_mode() == ESP_OK);
    burner_spi_allow_swd_restore();
    assert(burner_spi_leave_swd_mode(true) == ESP_OK && !s_swd_mode);
    assert(burner_spi_enter_swd_mode() == ESP_OK);
    init_error = ESP_ERR_NO_MEM;
    assert(burner_spi_leave_swd_mode(true) == ESP_ERR_NO_MEM && !locks && s_swd_mode && s_swd_restore_blocked);
    puts("SPI/SWD ownership: acquisition, rollback, blocked recovery and restore failure passed");
}
