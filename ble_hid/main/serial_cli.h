#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动交互式串口命令行维护任务。
 * 支持通过 USB CDC 虚拟串口或 UART 终端直接配置 Wi-Fi、服务器 IP 与测试 HID。
 */
esp_err_t serial_cli_start(void);

/**
 * @brief 自启动以来是否收到过完整的串口命令行（OTA 健康自检的兜底判据：
 * 有人在场敲命令说明固件可用，即便服务器不在线也不应回滚）。
 */
bool serial_cli_activity(void);

#ifdef __cplusplus
}
#endif
