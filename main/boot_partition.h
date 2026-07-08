#ifndef BOOT_PARTITION_H
#define BOOT_PARTITION_H

#include "esp_err.h"

esp_err_t boot_partition_switch_to(const char *partition_label);

#endif /* BOOT_PARTITION_H */
