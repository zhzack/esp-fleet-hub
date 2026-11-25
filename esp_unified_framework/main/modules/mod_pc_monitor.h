#ifndef MOD_PC_MONITOR_H
#define MOD_PC_MONITOR_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

esp_err_t mod_pc_monitor_init(void);
esp_err_t mod_pc_monitor_register_routes(httpd_handle_t server);

// Apply updated app_config display settings immediately
void mod_pc_monitor_apply_config(void);

// Build combined display & monitor status cJSON object (caller frees)
cJSON *mod_pc_monitor_build_status_json(void);

// Parse and persist display config / custom text patch (JSON string)
esp_err_t mod_pc_monitor_update_display_json(const char *json_str, bool switch_text_on_lines);

// Update screen metrics from internal or external sources
void mod_pc_monitor_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *time_str);
void mod_pc_monitor_show_status(const char *title, const char *sub);

#endif // MOD_PC_MONITOR_H
