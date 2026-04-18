#ifndef PIN_MAP_H
#define PIN_MAP_H

#include "driver/gpio.h"

/*
 * MORI burner pin mapping from:
 * Netlist_Schematic1_3_2026-03-06.tel
 * (ESP32-S3-WROOM-1, U6.* pins)
 */

/* TF card (SDMMC 4-bit) */
#define MORI_PIN_TF_CLK GPIO_NUM_9    /* ESP32_SDIO_CLK  (U6.17) */
#define MORI_PIN_TF_CMD GPIO_NUM_10   /* ESP32_SDIO_CMD  (U6.18) */
#define MORI_PIN_TF_D0 GPIO_NUM_46    /* ESP32_SDIO_D0   (U6.16) */
#define MORI_PIN_TF_D1 GPIO_NUM_3     /* ESP32_SDIO_D1   (U6.15) */
#define MORI_PIN_TF_D2 GPIO_NUM_12    /* ESP32_SDIO_D2   (U6.20) */
#define MORI_PIN_TF_D3 GPIO_NUM_11    /* ESP32_SDIO_D3   (U6.19) */

/* ESP32 SPI2 <-> MCU */
#define MORI_PIN_MCU_SPI_CS GPIO_NUM_21    /* ESP32_SPI2_CS#   (U6.23) */
#define MORI_PIN_MCU_SPI_CS1 GPIO_NUM_1    /* ESP32_SPI_CS1    (user wiring, dual-CS mode) */
#define MORI_PIN_MCU_SPI_CLK GPIO_NUM_47   /* ESP32_SPI2_CLK   (U6.24) */
#define MORI_PIN_MCU_SPI_MISO GPIO_NUM_48  /* ESP32_SPI2_MISO  (U6.25) */
#define MORI_PIN_MCU_SPI_MOSI GPIO_NUM_45  /* ESP32_SPI2_MOSI  (U6.26) */

/* USB */
#define MORI_PIN_USB_DN GPIO_NUM_19   /* ESP32_DN (U6.13) */
#define MORI_PIN_USB_DP GPIO_NUM_20   /* ESP32_DP (U6.14) */

/* I2C / control */
#define MORI_PIN_I2C_IRQ GPIO_NUM_17   /* I2C_IRQ#   (U6.10) */
#define MORI_PIN_I2C_SCL GPIO_NUM_8    /* I2C_SCL    (U6.12) */
#define MORI_PIN_I2C_SDA GPIO_NUM_18   /* I2C_SDA    (U6.11) */
#define MORI_PIN_KEY_BOOT GPIO_NUM_0   /* KEY_BOOT   (U6.27) */
#define MORI_PIN_MCU_RESET GPIO_NUM_38 /* MCU_RESET# (U6.31) */
#define MORI_PIN_IP5306_KEY GPIO_NUM_39 /* IP5306_KEY (U6.32) */

/* Debug/UART */
#define MORI_PIN_UART0_RX GPIO_NUM_44  /* RXD0      (U6.36) */
#define MORI_PIN_UART0_TX GPIO_NUM_43  /* TXD0      (U6.37) */
#define MORI_PIN_MCU_SWCLK GPIO_NUM_2  /* MCU_SWCLK (U6.38) */
#define MORI_PIN_MCU_SWDIO GPIO_NUM_1  /* MCU_SWDIO (U6.39), SWD disabled in firmware by default */

/* Optional display/audio lines from netlist (reserved for future use) */
#define MORI_PIN_LCD_MOSI GPIO_NUM_4  /* LCD_MOSI (U6.4)  */
#define MORI_PIN_LCD_CS GPIO_NUM_5    /* LCD_CS#  (U6.5)  */
#define MORI_PIN_LCD_CLK GPIO_NUM_6   /* LCD_CLK  (U6.6)  */
#define MORI_PIN_I2S_SD GPIO_NUM_7    /* I2S_SD   (U6.7)  */
#define MORI_PIN_I2S_BCK GPIO_NUM_15  /* I2S_BCK  (U6.8)  */
#define MORI_PIN_I2S_WS GPIO_NUM_16   /* I2S_WS   (U6.9)  */
#define MORI_PIN_LCD_DC GPIO_NUM_13   /* LCD_DC   (U6.21) */
#define MORI_PIN_LCD_BK GPIO_NUM_14   /* LCD_BK   (U6.22) */

#endif /* PIN_MAP_H */
