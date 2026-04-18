#ifndef USB_MSC_TF_H
#define USB_MSC_TF_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t usb_msc_tf_init(void);
bool usb_msc_tf_ready(void);
bool usb_msc_tf_enabled(void);
esp_err_t usb_msc_tf_set_enabled(bool enabled);
bool usb_msc_tf_in_use_by_host(void);

#endif
