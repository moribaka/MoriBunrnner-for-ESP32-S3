#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define portMAX_DELAY 0
static int s_backlight_lock=1;
static bool s_backlight_suspended;
static uint8_t s_backlight_brightness=128, actual;
static int init_error;
static void xSemaphoreTake(int lock,int timeout){(void)lock;(void)timeout;}
static void xSemaphoreGive(int lock){(void)lock;}
static esp_err_t lcd_backlight_pwm_init(void){return init_error;}
static void lcd_backlight_set(bool on){actual=on?s_backlight_brightness:0;}
#include "../../main/lcd_backlight_api.inc"
int main(void){
    assert(lcd_display_set_brightness(128)==ESP_OK && actual==128);
    assert(lcd_display_set_backlight_suspended(true)==ESP_OK && actual==0);
    // Volume persistence reads this getter while the screen is asleep.
    assert(lcd_display_get_brightness()==128);
    assert(lcd_display_set_brightness(77)==ESP_OK && actual==0);
    assert(lcd_display_get_brightness()==77);
    assert(lcd_display_set_backlight_suspended(false)==ESP_OK && actual==77);
    for(int i=0;i<100;i++){
        lcd_display_set_backlight_suspended(true);
        assert(lcd_display_get_brightness()==77 && actual==0);
        lcd_display_set_backlight_suspended(false);
        assert(actual==77);
    }
    // Explicit zero remains a valid user setting, not silently replaced.
    lcd_display_set_brightness(0);lcd_display_set_backlight_suspended(true);
    lcd_display_set_backlight_suspended(false);assert(actual==0);
    init_error=2;assert(lcd_display_set_brightness(50)==2);
    assert(lcd_display_get_brightness()==0);
    puts("Backlight sleep, persistence, sleeping adjustment and wake passed");
}
