# ESP32-S3 固件系统详细设计说明书 (Firmware Subsystem)

> **模块路径**：`ble_hid/`  
> **文档版本**：V2.1 工业级标准  
> **更新时间**：2026-10-26  
> **硬件芯片**：ESP32-S3 (8MB Octal PSRAM + 16MB Flash)  
> **SDK 版本**：ESP-IDF v5.4.1 (NimBLE + TinyUSB)

---

## 1. 固件定位与核心设计准则

固件系统作为整个平台的**物理执行终端**，直接与受控电脑物理接口（USB 总线或 2.4GHz BLE 蓝牙空口）对接。其设计遵循以下核心原则：

1. **单内核命令路由 (Single Kernel)**：统一通过 [`cmd_router.c`](file:///data/code/blue/py_web/ble_hid/main/cmd_router.c) 调度所有指令，杜绝不同入口（串口 vs 网络）行为不一致。
2. **极速无阻塞执行 (Non-Blocking Pacing)**：高频位移采用累加缓冲与连接间隔节拍自适应；真实按键采用独立旁路通道与脏位重试，严禁丢点击。
3. **内存与调用栈安全 (Stack Safety)**：将 BLE 通知链与流控排空任务分配独立 4KB 任务栈，严禁在 FreeRTOS 软件定时器（`Tmr Svc` 默认 3KB）或中断中调用深层 BLE 协议栈。
4. **高可用防变砖 (Self-Healing OTA)**：双槽位引导分区，流式 SHA-256 完整性核对，90s 在线自检与看门狗自动回滚机制。

---

## 2. 固件架构与模块分布

```
ble_hid/
├── CMakeLists.txt           # 项目级 CMake 构建配置
├── partitions.csv           # 双槽位 OTA 分区表 (otadata, ota_0, ota_1)
├── sdkconfig.defaults       # 编译默认配置 (NimBLE, TinyUSB, PSRAM, Rollback)
├── flash.py                 # 主机端固件构建、烧录与串口监视脚本
└── main/                    # 源码目录
    ├── ble_hid_main.c       # 固件主入口：NVS 初始化、外设初始化与启动调度
    ├── cmd_router.c/h       # 单内核命令路由器：统一命令注册、参数校验与分发
    ├── serial_cli.c/h       # 维护串口 CLI：薄翻译层，解析命令转 JSON 调 router
    ├── executor.c/h         # 动作执行器：FreeRTOS 独立执行队列与按键流水线
    ├── hid_usb.c/h          # TinyUSB 驱动：1000Hz 轮询率、位移累加、按键防丢
    ├── hid_ble.c/h          # NimBLE 驱动：3 连接并发、镜像广播、15ms 节拍、强制加密
    ├── ota_mgr.c/h          # OTA 管理器：双槽交替写入、SHA-256 校验、90s 健康自检回滚
    ├── ws_client.c/h        # WebSocket 客户端：esp_websocket_client 与 HMAC 挑战应答
    ├── wifi_mgr.c/h         # WiFi 管理器：连接、自动重连与扫描
    ├── app_config.c/h       # NVS 配置管理：UUID、设备密钥、WiFi 列表、传输模式
    ├── heartbeat.c/h        # 心跳协程：定时向服务端同步硬件健康与连接数
    ├── keymap.c/h           # 键位映射表：US 键盘布局与修饰键位掩码解析
    └── protocol.c/h         # 协议编解码：cJSON 解析 V2 envelope 信封
```

---

## 3. 核心子系统与关键技术实现

### 3.1 单内核命令路由架构 ([`cmd_router.c`](file:///data/code/blue/py_web/ble_hid/main/cmd_router.c))

在 V2.1 架构中，所有外部指令输入（服务端 WebSocket 帧、局域网 HTTP 请求、本地 CH343 维护串口）全部收拢汇聚为：
```c
esp_err_t cmd_router_dispatch(const char *type, const cJSON *payload, cJSON **out_result);
```
- **核心命令注册表**：
  - `action`：投递给 `executor` 任务执行物理输入。
  - `get_status`：查询硬件运行时间、自由内存、网络连接与当前输出通道。
  - `wifi.connect` / `wifi.scan`：WiFi 管理。
  - `server.set`：修改网关 IP/端口并持久化至 NVS。
  - `transport.set`：显式切换输出通道（`auto` / `usb` / `ble`）并写入 NVS。
  - `ota.start` / `ota.status`：触发与查询在线 OTA 升级。
  - `auth.secret`：更新与脱敏查询设备预共享密钥。
  - `reboot`：优雅重启芯片。
- **串口 CLI 彻底薄化 ([`serial_cli.c`](file:///data/code/blue/py_web/ble_hid/main/serial_cli.c))**：不再私自调用底层驱动，仅将串口字符命令（如 `transport usb`）打包为 JSON 载荷调用 `cmd_router_dispatch("transport.set", payload)`，确保多端行为高度一致且一并持久化。

### 3.2 USB HID 高性能驱动 ([`hid_usb.c`](file:///data/code/blue/py_web/ble_hid/main/hid_usb.c))

#### 三层性能瓶颈突破
1. **端点轮询率提升**：TinyUSB HID 描述符将 `bInterval` 从 10ms 调整为 **1ms (1000Hz)**，彻底消除物理轮询上限对高 DPI 快速甩动的吞吐制约。
2. **位移增量累加合并缓冲**：
   ```c
   // 累加保护
   portENTER_CRITICAL(&s_mouse_mux);
   s_pending_dx += dx;
   s_pending_dy += dy;
   s_pending_wheel += wheel;
   portEXIT_CRITICAL(&s_mouse_mux);
   ```
   端点忙时增量安全暂存在内存中，端点空闲时截取 $\pm 127$ 边界发送；在 `tud_hid_report_complete_cb` 中断回调中立即唤醒补发剩余余量，实现位移零丢弃。
3. **按键脏位标记机制 (`s_buttons_dirty`)**：
   按键动作（`mouse_down` / `mouse_up`）若因端点瞬时拥堵发送失败，系统置位 `s_buttons_dirty`。下一次移动报告发送时无条件将最新按键状态并入报告，彻底杜绝了“移动中松开左键导致目标机粘键或丢点击”的问题。

### 3.3 BLE HID 多连接与 macOS 兼容 ([`hid_ble.c`](file:///data/code/blue/py_web/ble_hid/main/hid_ble.c))

#### 1. 多主机并发连接（3 连接镜像广播）
- 引入连接表 `s_conns[3]` 维护所有已连接主机的 `conn_handle`。
- 连接数 $< 3$ 时，保持持续广播，允许第二台电脑（如 Windows PC）在第一台电脑（Mac）保持连接时随时被搜到并接入。
- 输入信号下发时，遍历 `s_conns` 并为每个对端分配独立的 `os_mbuf` 进行通知发送，单台断开或拥塞不影响其他主机。

#### 2. 解决 macOS 配对死循环
- **强制加密特征**：为 HID 报告特征添加 `BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC` 权限。
- **CCCD 权限锁定**：在 GATT 注册前调用 `ble_gatts_set_clt_cfg_perm_flags(BLE_GATT_CHR_F_WRITE_ENC)`。未配对前禁止订阅通知，促使 macOS 主机发起标准 SMP 配对。
- **配对密钥 NVS 永久存储**：调用 `ble_store_config_init()`，将配对生成的 LTK 保存在 Flash 分区的 `nimble_bond` 命名空间。重启后 Mac 主机在 3 秒内免重配即刻恢复加密链路。

#### 3. 栈溢出崩溃防护与节拍自适应
- **Tmr Svc 栈溢出真凶**：FreeRTOS 软件定时器任务（`Tmr Svc`）栈仅 3KB，在回调中跑 NimBLE 通知链会瞬间爆栈。固件为此设立专用的 `ble_m_drain` 任务（4KB 栈）处理排空。
- **连接间隔节拍自适应**：监听 `BLE_GAP_EVENT_CONN_UPDATE` 事件读取 `desc.conn_itvl`（如 Mac 协商为 15ms），排空节拍动态设为 $15\text{ms} - 1\text{ms} = 14\text{ms}$，发后清空位移积压，延迟严格收敛在单个蓝牙间隔内。

### 3.4 双槽位 OTA 升级与健康自检 ([`ota_mgr.c`](file:///data/code/blue/py_web/ble_hid/main/ota_mgr.c))

#### 分区规划 (`partitions.csv`)
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xf000,  0x2000,
phy_init, data, phy,     0x11000, 0x1000,
ota_0,    app,  ota_0,   0x20000, 0x1C0000,
ota_1,    app,  ota_1,   0x1E0000,0x1C0000,
storage,  data, spiffs,  0x3A0000,0xC60000,
```

#### 健康自检与自动回滚时序
1. **SHA-256 流式校验**：下载循环中调用 `mbedtls_sha256_update` 累加计算哈希，下载完成后若与期望哈希不符，直接报错并销毁写入句柄，**不切换启动分区，不重启**。
2. **观察期状态机 (`PENDING_VERIFY`)**：
   - 固件重启首次引导时处于 `ESP_OTA_IMG_PENDING_VERIFY` 状态。
   - 启动 `ota_health_task`（90 秒倒计时）。
   - **健康确认准则**：设备在 90s 内成功连入服务端 WebSocket 完成注册，或本地串口 CLI 接收到调试指令，且连续在线保持 5 秒，调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认固件正式生效。
   - **自动回滚准则**：若 90s 超时或发生看门狗复位/Panic，调用 `esp_ota_mark_app_invalid_rollback_and_reboot()` 立即回滚并引导旧分区重启。

---

## 4. FreeRTOS 任务与调度优先级表

| 任务名称 | 优先级 | 栈大小 | 运行核心 | 职责描述 |
|---|:---:|:---:|:---:|---|
| **`main`** | 1 | 4096 B | Core 0 | 初始化 NVS、WiFi、TinyUSB、NimBLE 并启动子任务 |
| **`ws_client`** | 6 | 6144 B | Core 0 | 处理与网关长连接的收发、HMAC 挑战应答与重连 |
| **`exec_task`** | 5 | 4096 B | Core 1 | 动作执行流水线，出队 action 载荷并调用驱动 |
| **`ble_m_drain`**| 4 | 4096 B | Core 1 | 独立排空 BLE 鼠标增量与滚轮，自适应蓝牙节拍 |
| **`ota_task`** | 3 | 8192 B | Core 0 | 异步拉取 HTTP 固件流并执行流式 SHA-256 校验写入 |
| **`ota_health`** | 2 | 3072 B | Core 0 | 升级后 90s 健康观察期与自动回滚监测任务 |
| **`serial_cli`** | 2 | 4096 B | Core 1 | 监听 UART0 字符输入，翻译为 JSON 后投递命令内核 |
| **`heartbeat`** | 1 | 2048 B | Core 0 | 定期 30s 向服务端推送心跳帧与诊断指标 |
