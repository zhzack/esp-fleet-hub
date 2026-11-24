#ifndef MOD_PC_MONITOR_H
#define MOD_PC_MONITOR_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t mod_pc_monitor_init(void);
esp_err_t mod_pc_monitor_register_routes(httpd_handle_t server);

// Update screen metrics from internal or external sources
void mod_pc_monitor_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *time_str);
void mod_pc_monitor_show_status(const char *title, const char *sub);

#endif // MOD_PC_MONITOR_H
