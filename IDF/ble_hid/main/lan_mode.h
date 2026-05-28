#ifndef LAN_MODE_H
#define LAN_MODE_H

#include "esp_err.h"

esp_err_t lan_mode_start(const char *device_id);
void      lan_mode_stop(void);

#endif
