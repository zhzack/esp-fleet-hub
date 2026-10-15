#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动交互式串口命令行维护任务。
 * 支持通过 USB CDC 虚拟串口或 UART 终端直接配置 Wi-Fi、服务器 IP 与测试 HID。
 */
esp_err_t serial_cli_start(void);

#ifdef __cplusplus
}
#endif
