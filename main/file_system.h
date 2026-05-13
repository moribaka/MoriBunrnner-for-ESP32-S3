#ifndef FILE_SYSTEM_H
#define FILE_SYSTEM_H
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "pin_map.h"

// 挂载点
#define mount_point "/sdcard"
#define assets_mount_point "/assets"
#define assets_partition_label "assets"
#define sdcard_tag "SDCARD"
// 全局变量
#define sdmmc_tag  "sdmmc"

// 引脚定义
#define SDMMC_PIN_CLK MORI_PIN_TF_CLK
#define SDMMC_PIN_CMD MORI_PIN_TF_CMD
#define SDMMC_PIN_D0  MORI_PIN_TF_D0
#define SDMMC_PIN_D1  MORI_PIN_TF_D1
#define SDMMC_PIN_D2  MORI_PIN_TF_D2
#define SDMMC_PIN_D3  MORI_PIN_TF_D3

extern sdmmc_card_t *card;

// 函数声明
esp_err_t sdmmc_init(void);
esp_err_t sdmmc_unmount(void);
esp_err_t assets_fs_init(void);
void assets_fs_print_info(void);
#endif
