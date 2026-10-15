# HID Gateway 项目交付与交接文档 (Project Handover Document)

> **交付对象**：接手本项目的后续 AI Agent 或研发工程师  
> **更新时间**：2026-10-16  
> **当前状态**：后端服务正常运行中、ESP32 硬件已在线连入、前端已编译上线、端到端链路验证打通。

---

## 一、 系统架构与核心工程全貌

本项目为**软硬件协同的局域网外设控制与中继中枢系统**，实现“Web 控制台 / 移动端触控板 $\rightarrow$ FastAPI 后端网关 $\rightarrow$ ESP32-S3 硬件 $\rightarrow$ 目标电脑 (USB / BLE 蓝牙)”的全链路 HID 遥控控制。

```
┌────────────────────────────────────────────────────────┐
│            Vue 3 前端系统 (vue-project/)                │
│  总览仪表盘 | 设备管理 | 动作控制台 | 虚拟触控板 | 宏库  │
└───────────────────────────┬────────────────────────────┘
                            │ HTTP REST / WebSocket (/ws/client)
┌───────────────────────────▼────────────────────────────┐
│          FastAPI 后端服务 (app/ - Port 8000)            │
│  - 调度派发器 (dispatcher.py) & ACK 追踪器              │
│  - 设备注册管理 (device_registry.py)                   │
│  - 数据库：SQLite (app.db) / MySQL (生产)              │
│  - 静态资源托管：挂载托管 vue-project/dist/            │
└───────────────────────────┬────────────────────────────┘
                            │ 双向 WebSocket (/ws/device)
┌───────────────────────────▼────────────────────────────┐
│         ESP32-S3 固件系统 (ble_hid/ - ESP-IDF v5.4)    │
│  - 协议编解码 (protocol.c) & 命令路由器 (cmd_router.c) │
│  - 动作执行流水线 (executor.c)                         │
│  - USB HID 驱动 (TinyUSB 复合设备: 键盘 + 鼠标)        │
│  - BLE HID 驱动 (NimBLE 协议栈: 键盘 + 鼠标广播)       │
└───────────────────────────┬────────────────────────────┘
                            │ USB 物理插头 / 蓝牙 2.4GHz 无线
┌───────────────────────────▼────────────────────────────┐
│                    受控目标主机电脑                     │
└────────────────────────────────────────────────────────┘
```

---

## 二、 运行环境与实时状态速查

### 1. 服务端与网络
- **后端服务**：FastAPI 后端已作为守护进程启动，监听 `http://0.0.0.0:8000`
  - 启动命令：`.\\.venv\\Scripts\\python -m uvicorn app.main:app --host 0.0.0.0 --port 8000`
  - 服务端局域网 IP：`192.168.68.159`
- **前端托管**：由 FastAPI 直接接管 `vue-project/dist`，访问 `http://127.0.0.1:8000/` 即是最新编译的前端。
- **数据库**：`app.db`（SQLite），已全部拉齐模型结构。
- **内置测试账号**：
  - 管理员：`admin` / `admin123` (角色：`admin`)
  - 普通控制员：`alice` / `secret123` (角色：`user`)

### 2. ESP32-S3 硬件端
- **硬件芯片**：ESP32-S3 (8MB PSRAM, 16MB Flash)
- **本地连接端口**：`COM5`（板载 CH343 USB-TTL，波特率 `115200` 串口日志 / `921600` 烧录）
- **Wi-Fi 连接**：已连接至路由器（SSID: `1`），芯片分配 IP: `192.168.68.142`
- **长连接链路**：已通过 WebSocket 连入 `ws://192.168.68.159:8000/ws/device`
- **设备标识**：`esp32_001`（状态：`online`，声明能力：`["ble_keyboard", "ble_mouse"]`）
- **当前传输模式**：`ble`（通过 `GET /api/v2/devices/esp32_001/transport` 可实时确认）

---

## 三、 本次已攻克的关键技术修复 (Fixes & History)

后续 Agent 可根据以下记录快速了解已有改动，避免二次踩坑：

### 1. 数据库跨库自增主键兼容 (`app/database/models/`)
- **根因**：SQLite 原生不支持 `BIGINT AUTOINCREMENT`，导致 ESP32 首次上报注册时抛出 `sqlite3.IntegrityError: NOT NULL constraint failed: devices.id`。
- **解决**：在所有 ORM 模型中统一切换为：
  ```python
  id = Column(BigInteger().with_variant(Integer, "sqlite"), primary_key=True, autoincrement=True)
  ```
  在本地 SQLite 自动使用 `INTEGER` 自增，在生产 MySQL 自动使用 `BIGINT`，零配置平滑切换。

### 2. 指令下发重试致虚假 ACK 的深层 Bug 修复 (`app/services/dispatcher.py`)
- **根因**：此前当下发指令但蓝牙未连上时，ESP32 硬件第一遍返回了失败 `E007: no BLE peer connected`；但服务端的网络重试逻辑携带相同的 `msg_id` 再次投递，ESP32 固件的去重防重放机制（dedup）误判定“该消息已处理”，返回了 `status: ok`。导致服务端误判为成功并在页面显示绿色的“执行成功”，使用户误以为指令发出了。
- **解决**：在 `dispatcher.py` 中增加断言：当硬件明确返回负向业务确认（`ack_ok == False`）时，立即 `break` 终止重试并如实向上汇报错误码，不再掩盖硬件的真实错误。

### 3. 前端 UI 设计规范与色彩对比度彻底重构 (`vue-project/`)
- **CSS 变量与样式库补齐**：在 `src/styles/index.css` 补齐 `--color-text-*`、`--color-bg-*` 等基础变量，移除导致文字发虚的毛玻璃模糊属性。
- **消除 Tailwind 400 荧光色浅色误用**：全面替换表格标签与状态徽标，提升对比度至 WCAG AA/AAA 标准（消除白底黄字、白底浅青字）。
- **页面组件风格统一**：重构了 `console.vue`（浅色拟物 3D 机械键帽）、`dashboard.vue`（品牌高科技蓝紫渐变 Banner）、`device-control.vue`（纯白卡片抽屉与关闭按钮）。
- **抽屉后台查询静默优化**：将 `DeviceControl.vue` 中打开抽屉时触发的状态轮询改为静默模式（`silent: true`），消除了此前关闭抽屉后突然“跳出延迟弹窗”的逻辑体验问题。

### 4. 运行历史日志归档保存
- 已完整导出此前所有的 WebSocket 握手报文、心跳、动作日志和错误详情至根目录 **`saved_device_logs.txt`**（82.5 KB），任何时候都可用于追溯历史行为。

### 5. 全链路 ESP32 双分区 OTA 升级系统落地
- **固件层 (`ble_hid/`)**：
  - 分区表升级为双备份架构（[partitions.csv](file:///f:/py_web/ble_hid/partitions.csv)），划分 `otadata`（引导标记）以及 `ota_0` / `ota_1`（双 1800KB 应用程序槽）。
  - 新增独立 FreeRTOS 异步升级任务 [ota_mgr.c](file:///f:/py_web/ble_hid/main/ota_mgr.c) / [ota_mgr.h](file:///f:/py_web/ble_hid/main/ota_mgr.h)，支持分块流式写入、自动对齐哈希、进度向 WebSocket 实时回传，并在校验成功后调用 `esp_ota_set_boot_partition` 自动重启切换。
  - 在 [cmd_router.c](file:///f:/py_web/ble_hid/main/cmd_router.c) 与 [serial_cli.c](file:///f:/py_web/ble_hid/main/serial_cli.c) 中分别注册了 `ota.start` 与 `ota.status` 调度命令。
- **服务端 (`app/`)**：
  - 新增 `firmwares` 数据表与 ORM 模型（[models/firmware.py](file:///f:/py_web/app/database/models/firmware.py)），支持安全记录版本、文件名、SHA-256、文件大小及描述。
  - 开放 REST 接口（[api/v2/firmwares.py](file:///f:/py_web/app/api/v2/firmwares.py)）：固件上传、版本列表、二进制直接下载（`FileResponse`）、一键推送升级及设备 live OTA 状态轮询。
  - 在 `app/api/ws/device.py` 中解析 `ota_progress` 上行帧，实时记录至 `ota_tracker` 内存服务。
- **前端系统 (`vue-project/`)**：
  - 新建独立路由页面 [views/firmware.vue](file:///f:/py_web/vue-project/src/views/firmware.vue)，集成固件拖拽上传、版本库管理、在线设备多选一键推送、环形进度条与实时升级日志流水。
  - 在侧边栏导航（[DefaultLayout.vue](file:///f:/py_web/vue-project/src/layouts/DefaultLayout.vue)）常驻“固件与 OTA”入口。

---

## 四、 遗留关键问题与接手 Agent 任务指引 (Next Steps)

当前最核心的待解决事项是**受控电脑连接 ESP32 蓝牙时的自动断连问题**：

### 1. 现象分析：蓝牙反复自动连接与断开 (Bluetooth Flapping Loop)
- **原因机理**：
  1. 电脑首次配对 `esp32_001` 后，操作系统在本地存储了配对长效密钥（LTK）。
  2. ESP32 固件中虽然开启了配对（`sm_bonding = 1`），但底层没有调用 NimBLE 的 NVS 永久存储模块（`ble_store_config_init`）。
  3. 当 ESP32 重启或烧录后，其内存中的密钥全部丢失。
  4. 电脑发现已配对设备广播后自动重连，但尝试启动加密（`LL_ENC_REQ`）时 ESP32 查无此密钥并拒绝；电脑因此判定握手失败立即断开；随后电脑再次扫描到广播又重试，陷入连上 $\rightarrow$ 拒绝 $\rightarrow$ 断开 $\rightarrow$ 重连的死循环。
- **电脑端即时恢复步骤**：
  1. 打开电脑 **系统设置 $\rightarrow$ 蓝牙**。
  2. 找到 `esp32_001`，点击 **“删除设备” (Remove device)** 彻底清除无效密钥。
  3. 重新搜索并完成全新配对。
- **固件端根治改造建议 (`ble_hid/main/hid_ble.c`)**：
  1. 引入 `#include "store/config/ble_store_config.h"`，并在 `hid_ble_init()` 中调用 `ble_store_config_init();`，让 NimBLE 自动将配对密钥保存至 Flash 的 `nvs` 分区。
  2. 在 `gap_event` 事件分发中增加 `BLE_GAP_EVENT_REPEAT_PAIRING` 处理，若对端电脑发起重配，自动调用 `ble_store_util_delete_peer(&desc.peer_id_addr)` 并返回 `BLE_GAP_REPEAT_PAIRING_RETRY`。

---

## 五、 常用开发与维护指令备忘

```powershell
# 1. 启动后端服务 (守护进程)
.\.venv\Scripts\python -m uvicorn app.main:app --host 0.0.0.0 --port 8000

# 2. 编译前端生产包
npm --prefix vue-project run build

# 3. 运行全量自动化测试
.\.venv\Scripts\pytest

# 4. 固件编译 (Docker 环境)
docker run --rm -v "${PWD}/ble_hid:/project" espressif/idf:release-v5.4 idf.py build

# 5. 固件串口烧录 (COM5)
.\.venv\Scripts\python ble_hid/flash.py --port COM5 --app-only
```
