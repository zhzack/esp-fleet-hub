# HID Gateway 项目交付与交接文档 (Project Handover Document)

> **交付对象**：接手本项目的后续 AI Agent 或研发工程师  
> **更新时间**：2026-10-20  
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
│  - 协议编解码 (protocol.c) & 单内核命令路由 (cmd_router.c) │
│  - 串口维护终端 (serial_cli.c 薄翻译层 → cmd_router) │
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
- **后端服务**：FastAPI 后端已作为守护进程启动，监听 `http://192.168.68.159:8000`
  - 启动命令：`.\\.venv\\Scripts\\python -m uvicorn app.main:app --host 192.168.68.159 --port 8000`
  - **端口 8000 冲突陷阱（2026-10-25）**：微信（Weixin.exe）会以 `Bound`（bind 未 listen）状态占住 `0.0.0.0:8000`，导致 `--host 0.0.0.0` 启动报 `[winerror 10048]`；通配绑定被挡但具体 IP 绑定不受影响，故绑定 `192.168.68.159`。不要起两个实例分别绑 127.0.0.1 / LAN IP——`device_registry` / `ack_tracker` 是进程内存态，会脑裂；本机访问前端/接口也走 `http://192.168.68.159:8000/`。
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
- **当前传输模式**：`usb`（显式 `transport.set` 已持久化到 NVS，重启/OTA 后不丢；通过 `GET /api/v2/devices/esp32_001/transport` 可实时确认）

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
- 已完整导出此前所有的 WebSocket 握手报文、心跳、动作日志和错误详情至 **`logs/saved_device_logs.txt`**（82.5 KB），任何时候都可用于追溯历史行为。

### 5. 全链路 ESP32 双分区 OTA 升级系统落地
- **固件层 (`ble_hid/`)**：
  - 分区表升级为双备份架构（[partitions.csv](file:///f:/py_web/ble_hid/partitions.csv)），划分 `otadata`（引导标记）以及 `ota_0` / `ota_1`（双 1800KB 应用程序槽）。
  - 新增独立 FreeRTOS 异步升级任务 [ota_mgr.c](file:///f:/py_web/ble_hid/main/ota_mgr.c) / [ota_mgr.h](file:///f:/py_web/ble_hid/main/ota_mgr.h)，支持分块流式写入、自动对齐哈希、进度向 WebSocket 实时回传，并在校验成功后调用 `esp_ota_set_boot_partition` 自动重启切换。
  - `ota.start` / `ota.status` 均注册于 [cmd_router.c](file:///f:/py_web/ble_hid/main/cmd_router.c) 命令内核（2026-10-25 起 serial_cli 不再单独实现命令，见第六节单内核重构）。
- **服务端 (`app/`)**：
  - 新增 `firmwares` 数据表与 ORM 模型（[models/firmware.py](file:///f:/py_web/app/database/models/firmware.py)），支持安全记录版本、文件名、SHA-256、文件大小及描述。
  - 开放 REST 接口（[api/v2/firmwares.py](file:///f:/py_web/app/api/v2/firmwares.py)）：固件上传、版本列表、二进制直接下载（`FileResponse`）、一键推送升级及设备 live OTA 状态轮询。
  - 在 `app/api/ws/device.py` 中解析 `ota_progress` 上行帧，实时记录至 `ota_tracker` 内存服务。
- **前端系统 (`vue-project/`)**：
  - 新建独立路由页面 [views/firmware.vue](file:///f:/py_web/vue-project/src/views/firmware.vue)，集成固件拖拽上传、版本库管理、在线设备多选一键推送、环形进度条与实时升级日志流水。
  - 在侧边栏导航（[DefaultLayout.vue](file:///f:/py_web/vue-project/src/layouts/DefaultLayout.vue)）常驻“固件与 OTA”入口。

### 6. 项目结构与 Git 跟踪整理 (2026-10-20)
- **文档目录更名**：`makerdown/` → `docs/`（修正拼写，README 引用同步更新）。
- **根目录遗留文件清理**：删除未被任何代码使用的根目录 `package.json` / `package-lock.json`（仅含 axios-mock-adapter），删除与 `compose.prod.yml` 完全重复的根目录 `docker-compose.yml`。
- **sh 脚本行为对齐**：`scripts/sh/` 下所有 compose 调用改为显式 `-f "$COMPOSE_FILE"`（默认 `compose.prod.yml`，可用环境变量 `COMPOSE_FILE` 覆盖），与旧根目录 `docker-compose.yml` 行为完全等价；`scripts/py/*.py` 本就显式指定，不受影响。
- **顺手修复 `_common.sh` 预存 Bug**：`PROJECT_ROOT` 原先只向上回溯一级（解析到 `scripts/` 而非仓库根目录），导致 sh 脚本的 `cd "$PROJECT_ROOT"` 与 compose 文件定位一直指向错误路径；已改为 `../..` 并验证 `COMPOSE_FILE` 正确解析到仓库根的 `compose.prod.yml`。
- **运行时产物移出 Git 跟踪**：SQL 备份统一归档至根目录 `backups/`（已加入 `.gitignore`，原 `scripts/mysql/backups/` 下的历史备份已迁入）；`saved_device_logs.txt` 移至 `logs/`。文件均保留在磁盘，历史版本仍可从 Git 历史追溯。
- **备份脚本路径对齐**：`scripts/mysql/backup.sh` 备份输出目录统一为根目录 `backups/`，与 `restore.sh` 的示例路径一致。
- **工作区缓存清理**：删除 `tools/`、`scripts/py/`、`tests/`、`ble_hid/` 下的 `__pycache__` 与根目录 `.pytest_cache`。
- **`.dockerignore` 同步修正**：原 `makedown/` 排除项（拼写错误且目录已更名）改为 `docs/`，并新增 `backups/`、`uploads/` 运行时数据目录排除，避免 SQL 备份与固件二进制进入镜像构建上下文；同时移除 `docs/docker_usage.md` 中指向不存在的 `ble_hid/README.md` 的死链。
- **框架弃用警告清零**：7 处 Pydantic `class Config:` 迁移为 `model_config = ConfigDict(from_attributes=True)`（schemas 下 6 个 + `api/v2/slots.py`）；`app/main.py` 的 `@app.on_event("startup")` 迁移为 `lifespan` 上下文管理器（启动逻辑不变）；`app/database/base_class.py` 的 `as_declarative` import 从弃用的 `sqlalchemy.ext.declarative` 移至 `sqlalchemy.orm`；`app/services/ping_service.py` 的 `datetime.utcnow()` 改为时区感知的 `datetime.now(timezone.utc)`。pytest 由 10 warnings 降为 0。
- **测试补齐（6 → 23 个用例）**：新增 `tests/test_schemas.py`（7 个迁移 schema 的 from_attributes 序列化 + `WsEnvelope` 协议信封解析）、`tests/test_devices_api.py`（设备 CRUD/归属过滤/认领 + BLE 槽位绑定校验）、`tests/test_device_groups_api.py`（分组 CRUD/成员管理/越权 403）、`tests/test_macros_api.py`（宏 CRUD/config 归一化/跨用户隔离）、`tests/test_vehicles_api.py`（车辆列表与 chip 过滤）、`tests/test_main_lifespan.py`（lifespan 建表与 health 可用性，通过 monkeypatch 测试引擎隔离真实 DB）。
- **测试期间发现的预存问题（已修复）**：① `crud/device_group.py` 的 `add_devices_to_group` 在单次请求中传入重复 device_id 会触发 `IntegrityError`（autoflush=False 导致查重看不到未提交的行），已用 `dict.fromkeys` 保序去重输入修复，测试覆盖单请求与跨请求两种重复场景；② `VehicleModel.current_status` 列无默认值，ORM 层插入 NULL 而 `VehicleOut` 要求 str，已为该列增加 `default="空闲"`（与 `VehicleBase` schema 的默认值对齐），测试验证端到端生效。
- **控制中枢升级：多设备并发 + 统一输出目标（2026-10-21）**：`vue-project/src/views/console.vue` 两项改造，后端零改动（协议早已支持）。① 设备选择器改多选（`target_devices` 数组下发，后端 `asyncio.gather` 并发派发），指令流终端逐设备展示 ack 结果（`× N 台设备` 摘要行 + 每设备一行 `device_id / status / 延迟 / 错误`）；② 左侧"BLE 目标主机"卡片升级为"输出目标主机"：下拉合并 `🔌 USB 主机（线缆直连）`（选中 = `transport.set → usb`）与 BLE 配对槽位（选中 = 必要时 `transport.set → ble` + `ble.switch`），当前输出通道由 `transport.get` + `ble.info` + `usb.info` + `ble.bonds` 四路并行拉取展示；注意固件 `usb.info` 仅返回 `{mounted: bool}`（无主机名），USB 连接状态取 `mounted ?? usb_mounted`（`transport.get` 亦含 `usb_mounted`）。语义约束：触控板与输出目标卡片仅在**恰好单选一台设备**时可用（`soleSelected` computed），多选时卡片禁用并提示。前端已重新构建（dist hash 更新），浏览器需 Ctrl+F5 强刷一次绕过缓存的旧 index.html。
- **传输模式持久化 + AUTO 默认（2026-10-25）**：修复"OTA/重启后 USB 失联"问题——`transport.set` 此前只写 RAM，设备重启后退回编译期默认 BLE，而 BLE 无对端连接时所有 HID 动作被 E007 拒绝。两处修复：① [executor.h](file:///f:/py_web/ble_hid/main/executor.h) 编译期默认从 BLE 改为 AUTO（USB 挂载则走 USB，否则 BLE）；② `transport.set` 显式选择持久化到 NVS `app` 命名空间（键 `transport_mode`），`cmd_router_init` 开机恢复。已通过 OTA 真机验证：`set usb → 断电重启 → mode=usb` 保持。固件版本 `1.2.0-transport-persist`。
- **单内核多前端：serial_cli 收编进 cmd_router（2026-10-25）**：串口 CLI 重构为薄翻译层（[serial_cli.c](file:///f:/py_web/ble_hid/main/serial_cli.c)），不再直接调用 `wifi_mgr` / `hid_*` / `executor` / `ota_mgr` / `keymap`，而是把 `cmd args` 语法翻译成 `(type, JSON payload)` 走 `cmd_router_dispatch()`，与云端 WS、LAN HTTP 前端共享同一份命令注册表、参数校验与错误码。命令映射：`status→get_status`、`wifi→wifi.connect`、`wifi_scan→wifi.scan`、`server→server.set`、`transport→transport.set`、`tap/text/move/click→action`、`ota→ota.status/ota.start`、`reboot→reboot`；`status` 与 `wifi_scan` 保留人类可读格式化，其余成功时回显紧凑 JSON。由此修复三处行为不一致：① 串口 `transport` 命令此前只写 RAM（重启丢失），现与云端一致持久化 NVS；② 串口 `tap/move/click/text` 此前在 USB 未挂载 / BLE 未连接时**静默丢弃**（直接调 hid_* 返回 void），现在统一被内核以 E007 拒绝并回显 `ERROR 500: no USB host`；③ `server <host> [port]` 从 CLI 私有实现收编为内核命令 `server.set`（写 NVS，云端/LAN 前端亦可用）。内核侧配套增强：`get_status` 新增 `transport_mode`（配置模式）与 `free_heap` 字段。CLI 接入方式不变：UART 口 115200（PuTTY 或 `python -m serial.tools.miniterm COM5 115200`），回车出 `esp32>` 提示符，`help` 列全部命令。
- **鼠标流式性能优化：三层瓶颈修复（2026-10-25）**：现象为透传时"鼠标移动速度慢于实际手的移动"。逐层剖析（浏览器 Pointer Lock → 10ms 合并 → WS → 后端派发 → Wi-Fi → 设备 cJSON → executor 队列 → TinyUSB → USB 中断端点）定位三个瓶颈并全部修复：① **后端每包 2 次 fsync 落库**——`deliver_to_device` 对每个 mouse_move（~100 包/秒）执行 `create_pending`(INSERT+COMMIT) 与 `mark_sent`(UPDATE+COMMIT)，SQLite 默认 journal=DELETE + synchronous=FULL 每次 commit fsync 1~15ms 且同步阻塞 asyncio 事件循环；修复：[dispatcher.py](file:///f:/py_web/app/services/dispatcher.py) 对流式动作类型（`mouse_move`/`mouse_scroll`）跳过 action_log 落库与 device 表查询（瞬态透传数据无审计价值），同时 [session.py](file:///f:/py_web/app/database/session.py) 引擎 connect 事件开启 `PRAGMA journal_mode=WAL` + `synchronous=NORMAL`（commit 只追加 WAL 不 fsync 主库，微秒级；WAL 还消除读写互斥）。② **固件端点忙时静默丢报告**——TinyUSB `tud_hid_n_report` 在端点忙（上一报告未轮询走）时 claim 失败直接丢弃，exec_task 排空队列无节流，突发时同轮询间隔内多个 move 只发出第 1 个；修复：[hid_usb.c](file:///f:/py_web/ble_hid/main/hid_usb.c) 鼠标路径改为**累积合并**——dx/dy/wheel 增量累积进 pending 缓冲（临界区保护），端点空闲时取 clamp(±127) 发送、余量保留，`tud_hid_report_complete_cb` 报告送达后立即补发剩余，按键状态变化走 `s_buttons_dirty` 标记并入下份报告（零丢失、杜绝丢点击）。③ **USB 报告率上限 100Hz**——HID 中断端点 bInterval=10ms，吞吐上限 127×100=12,700 counts/s/轴，高 DPI 快速甩动饱和且加剧丢弃；修复：bInterval 10ms → 1ms（1000Hz，与真实鼠标一致）。实测：HTTP+WS 全链路 200 连发 mouse_move 达 **628 actions/s（1.6ms/包）**，16 项 need_ack 键鼠全量用例 20/20 acked（平均执行 3.4ms），拖拽/修饰键组合正常。
- **安全加固第一轮：.env 出库 + 设备 WS 注册鉴权（2026-10-25）**：① **`.env` 出库与密钥轮换**——`.env` 此前被 git 跟踪且已推远端（含 JWT_SECRET 默认占位值与 MySQL 弱密码），已 `git rm --cached` 出库、加入 `.gitignore`、新增 `.env.example` 模板；本地完成 JWT_SECRET（64 hex）与 MySQL 密码轮换，旧 token 全部失效。排查中顺带发现并修复 `GET /api/v2/vehicles` 无鉴权泄漏车辆数据（补 `get_current_user` + 匿名 401 回归测试）；另澄清一个误判：`GET /api/v2/devices` 对不存在路径返回 200 是 SPA 静态回退（index.html），并非鉴权通过。② **设备 WS 注册鉴权（HMAC-SHA256 挑战-应答，防重放）**——后端 [device.py](file:///f:/py_web/app/api/ws/device.py) register 分支：配置了 `DEVICE_SHARED_SECRET`（[config.py](file:///f:/py_web/app/core/config/config.py)，空=关闭兼容旧部署）时，首次 register 下发 `auth_challenge`（随机 nonce，每连接一次性），设备须以 `payload.auth={nonce, proof: HMAC-SHA256(secret, nonce)}` 重发 register，校验通过才注册；错误 proof / 重放旧 nonce → `E401` + close 4401。固件侧：[app_config](file:///f:/py_web/ble_hid/main/app_config.c) 新增 `device_secret` NVS 字段；[ws_client.c](file:///f:/py_web/ble_hid/main/ws_client.c) 收到 auth_challenge 后用 mbedtls 计算 HMAC 携带 proof 重发 register（nonce 一次性，用后即清；未配置 secret 时拒绝应答并打 ERROR 提示走串口 CLI 配置，避免 register/challenge 死循环）；[protocol.c](file:///f:/py_web/ble_hid/main/protocol.c) 路由 `auth_challenge` 帧；[cmd_router.c](file:///f:/py_web/ble_hid/main/cmd_router.c) 新增 `auth.secret` 命令（写 NVS，查询时脱敏显示前 4 位）；串口 CLI 新增 `secret [<value>]`（无参=查询）。测试：`tests/test_ws_device_auth.py` 4 用例（挑战后合法 proof 注册成功 / 错误 proof E401+断连 / 旧 nonce 重放拒绝 / 空 secret 兼容直通），conftest autouse 关闭鉴权保持旧用例行为；E2E 实测：设备无 secret 时守卫拒绝 → 串口 `secret <64hex>` + `reboot` → 串口见 `auth challenge received, re-registering with proof` → 后端 `registered: esp32_001` → action acked。**部署注意**：新部署须在 `.env` 设 `DEVICE_SHARED_SECRET` 并通过串口 CLI `secret <同值>` 写入设备，两端一致方可注册；git 历史中的旧密钥未清理（filter-repo 会改写历史，视需要另行处理）。
- **OTA 可靠性：健康自检 + sha256 校验 + 自动回滚（2026-10-25）**：修复升级链路三个缺陷。① **回滚机制此前根本不存在**——`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 在 sdkconfig.defaults 声明了但当前生效 sdkconfig 未启用（构建缓存早于 defaults），`esp_ota_mark_app_valid_cancel_rollback()` 一直是空操作，新固件起来即死只能线刷救；已在 sdkconfig 显式启用。② **无条件取消回滚 → 健康自检后才确认**——[ota_mgr.c](file:///f:/py_web/ble_hid/main/ota_mgr.c) init 检测运行分区状态：`ESP_OTA_IMG_PENDING_VERIFY`（OTA 首启）时启动 `ota_health_task`，健康判据 = WS 注册在线 **或** 串口 CLI 有过命令（有人在场兜底，防服务器临时不在线误回滚），连续保持 5s 才 `mark_app_valid_cancel_rollback`；90s 超时仍不健康则 `esp_ota_mark_app_invalid_rollback_and_reboot()` 自动回滚旧分区。新增固件 crash 场景天然覆盖：PENDING_VERIFY 状态下任何复位（看门狗/panic）bootloader 都会自动回滚。③ **sha256 参数存而不校 → 下载流完整性校验**——下载循环 `mbedtls_sha256` 累积计算，`esp_ota_end` 后与期望值比对，不匹配报 `sha256 mismatch` 且**不切换启动分区不重启**（当前固件继续运行）。配套：`ota.status` 新增 `health_check`（monitoring/passed/rollback_timeout）与 `rollback_state`（pending_verify/valid）诊断字段；串口 CLI `ota <url> [sha256]` 支持可选校验参数；Kconfig 新增 `APP_OTA_TEST_UNHEALTHY`（真机回滚链路验证用，默认关闭）。后端测试（`tests/test_firmware_api.py`，53 passed）：upload 计算的 sha256 与文件真实哈希一致（信任根）、push 下发信封必含 url/version/sha256 三元组且 sha256 正确、push 仅 admin、未知设备逐台报错。真机 E2E 三场景全过：**A 正常升级**（push 1.2MB → `sha256 verified OK` → 重启 ota_1 → `PENDING_VERIFY — health check started` → WS 鉴权注册 → `health check PASSED (5s)` → `rollback_state: valid`）；**B 不健康回滚**（推送 UNHEALTHY 固件 → 90s `health check TIMEOUT — rolling back` → 自动回滚 ota_1 → 设备恢复在线，`ota.status` 确认 `running_partition: ota_1, rollback_state: valid`）；**C 错误 sha256**（串口注入 `ota <url> deadbeef...` → 下载 100% 后 `sha256 mismatch` → 拒绝升级不重启，设备持续在线）。**运维提示**：升级后 90s 内完成 WS 注册才视为健康；服务器维护窗口期升级需有人在串口敲任意命令兜底；pyserial 打开 COM5 前须置 `dtr=False rts=False`，否则 DTR 电平会把设备带进下载模式。
- **按键对账体系：Mac 端接收侧地面真值（2026-10-25 接手日）**：工具沉淀于 `tools/click*.m`（Mac 上 `/tmp/blefix/` 编译运行）。**CGEventTap（kCGSessionEventTap / kCGHIDEventTap）经 SSH 无图形会话/无输入监控授权时静默收不到任何事件**（tap 创建成功但投放被过滤，勿再踩）；免权限方案：① `CGEventSourceSecondsSinceLastEventType` 轮询（10ms，时刻回退检测，`clickpoll.m`，快速连点会同窗合并欠计）② `CGEventSourceCounterForEventType` 系统累计计数器（`clickcount.m`，**精确对账首选**，起止差值=窗口内真实事件数）。对账结果：四档模式（基线 10 / 快速连点 20 / 移动中 10 / 拖拽 3）共发送 43 次点击，Mac 实收 **43/43 零丢失**、down/up 计数守恒（无粘键）；移动流 90 包合并为 56 份报告（符合节拍设计）。发送侧走 `POST /api/v2/action`（mouse_down/mouse_up 显式对，比 mouse_click 单动作便于精确计数）。
- **移动中点击丢失修复：BLE 按键状态补发机制（2026-10-25 接手日，`8ebc80a`）**：按键报告若遇 mbuf 池瞬时耗尽（移动突发后高发）而 `force` 发送失败，此前**没有任何重试路径**——按键状态不在位移积压缓冲里，10ms 排空任务因"无增量"提前返回，手停时失败的按下报告永不补发，抬起时发出的已是"无按键"，整次点击凭空消失。补 `s_buttons_dirty` 脏标记（与 USB 路径 `s_buttons_dirty` 机制对齐）：置位后排空任务每 10ms 重试直到成功，成功才清除；断连重置。
- **鼠标死亡真凶：Tmr Svc 栈溢出崩溃循环（2026-10-25 接手日，`2e662ff`）**：`74f2802` 起的 10ms FreeRTOS 软件定时器回调里调 `ble_gatts_notify_custom`，在 Tmr Svc 任务（栈仅 3KB，`CONFIG_TIMER_TASK_STACK_DEPTH` 默认）里跑 BLE 通知调用链（2-3KB），约 10 次通知即 `***ERROR*** A stack overflow in task Tmr Svc` 复位。设备崩溃循环 + WS/Mac 反复自动重连，API 监控看着"在线"，实际鼠标死亡——**凡是把 BLE 调用放进小栈上下文（软件定时器回调/ISR）都会踩**。修复：专用 `ble_m_drain` 任务（4KB 栈，10ms 周期）替代软件定时器；验证：100 连发 + uptime 连续无复位。**测试陷阱**：光标探针连发正向位移会把 macOS 光标钉在屏幕右边缘（宽 2560），后续同向移动全部零位移，看起来像固件死了——探针测试前先记录位置/双向验证。
- **BLE 鼠标流控终版：节拍自适应连接间隔 + 按键旁路优先（2026-10-25 接手日，`e588279`/`c048e4e`）**：真机反馈两个问题——① 持续移动时光标延迟越用越大：`74f2802` 的积压缓冲只进不出，"零丢失"退化为排队（输入 ~100 包/s > BLE 容量 33-66 报告/s，pending 无界增长）；② 移动中点击可能丢失：移动流打满 mbuf 池后按键报告也发不出，按下/抬起都赶不上成功发送即凭空消失。终版设计：**位移流按协商出的实际连接间隔节拍发送（CONNECT/CONN_UPDATE 时 `ble_gap_conn_find` 读 `desc.conn_itvl`，节拍=间隔-1ms，限 7~40ms），发后清空 dx/dy 积压**——超速部分丢弃（真实鼠标编码器同样不排队），延迟有界为 1 个间隔；**按键事件（down/up）`force` 旁路节拍立即发送**；滚轮是事件流（丢步=丢滚动）保留结转。实测：macOS 先以 30ms 连接，我方 7.5-15ms 请求被接受 → 15ms/66.67fps，串口日志 `conn itvl 15.00ms -> mouse pacing 66.67 fps`；光标探针突发 1500 counts 无冻结无积压（位移小于输入值属 macOS 低速增益曲线+超速截断的合成，停手即停）。前端保持 100Hz 采样不动——固件是唯一知道当前传输介质的层次（USB 容量 628/s 无需节流），降采样在固件按介质自适应。
- **BLE 鼠标冻结回归修复：NOTIFY_TX 节流不可用（2026-10-25 接手日，`74f2802`）**：`a4ee1ad` 试图用 `BLE_GAP_EVENT_NOTIFY_TX` 做"发送完成节流"，真机表现为**鼠标只动一帧即冻结**。根因：本 NimBLE 版本的 NOTIFY_TX 在 `ble_gatts_notify_custom` **入队时同步回调**（ble_gattc.c 直接调 `ble_gap_notify_tx_event(rc,...)`，非空口发送完成），时序为"入队→事件→置忙"，置忙时能清除标记的事件已经跑完，此后再无发送、再无事件，标记永久卡死。修法：去掉事件节流，改 10ms FreeRTOS 定时器周期排空积压（输入停止后 <127 的余量也能发完），背压由 mbuf 池 ENOMEM 天然承担（发送失败即累积、下轮重试，无死锁可能）。**教训：NimBLE 服务端 notification 没有"发送完成"事件可用，节流不要挂在 NOTIFY_TX 上**。远程验证方法沉淀：Mac 编译 CGEvent 光标探针（`/tmp/blefix/cursor`），API 连发 12×40 mouse_move 实测光标位移 520px（Mac 指针加速曲线下数值合理）——无需用户在场即可客观验证 HID 链路。
- **macOS 配对转圈根因修复：HID 服务强制加密（2026-10-25 接手日，`13fefbc`）**：受控机为 Mac（macOS 27，SSH 已配好账号可远程排查）时配对永远转圈。双端日志定位（Mac `log stream` bluetoothd + 设备串口）：**Mac 连接后无需配对即可读 HID 特征并订阅 CCCD**（设备端 `subscribe cur_notify=1` 且无任何 SMP 事件），macOS 的 HOGP 实现因此拒绝建键并陷入 ~5s 重连循环；Windows 宽松不要求加密所以从未暴露。[hid_ble.c](file:///f:/py_web/ble_hid/main/hid_ble.c) 修复：HID 全部特征加 `READ_ENC`/`WRITE_ENC`（加密不足返回 Insufficient Authentication 促使主机发起 SMP 配对）+ CCCD 全局 ATT 权限加 `WRITE_ENC`（`ble_gatts_set_clt_cfg_perm_flags`，NimBLE 私有 API，前置声明，**必须在 GATT 注册前调用**）；不可用 `AUTHEN`（Just Works 无 MITM，要求认证直接配对失败）。配套修复：① `CONFIG_BT_NIMBLE_NVS_PERSIST` 此前未启用（`a286d1c`）——ble_store_config 持久化层整个编译为空操作桩，bond 只存 RAM；② 设备 NVS 有更早时代残留的旧 bond，`ble.unpair_all` 现已能真正擦除 NVS。真机验证：Mac 配对成功 → bond 落盘 → `reboot` 后 3 秒内 Mac 自动重连（bond 从 NVS 恢复，密钥匹配）。排查工具沉淀：`tools/serial_capture.py`（COM5 被动抓串口，open 后须拉低 dtr/rts）、Mac 侧 `/tmp/blefix/{blueutil,blesc}`（blueutil 需源码编译、CoreBluetooth CLI 会撞 TCC 蓝牙授权）；Mac BLE 公开地址 = WiFi MAC 末字节 +2（`30:ed:a0:aa:68:ee`）。当前输出通道 `transport auto`（USB 未挂载走 BLE）。
- **心跳语义修正：不再覆盖设备上报状态（2026-10-25 接手日）**：[device.py](file:///f:/py_web/app/api/ws/device.py) 的 heartbeat 处理器此前无条件把 `dev.status` 重置为 `"online"`，设备上报的 `busy` / `error` 会被下一个周期心跳立刻踩掉。已改为心跳只更新 `last_online_at`、仅在残留 `offline` 时归位，恢复需设备显式上报 `status` 帧（固件当前不发 status 帧，无兼容影响）。该缺陷由 `test_heartbeat_and_status_updates` 在全套运行时稳定失败暴露（单跑竞态掩盖：第三帧 heartbeat 本意是刷帧同步，实际把 busy 覆盖回 online）；测试同步改为轮询等待（帧无回复、无同步点），两轮全量 53 passed 稳定复现。
- **BLE 配对密钥持久化：根治重启后蓝牙断连循环（2026-10-25 接手日，固件 `7af52b0` 已 OTA 上机）**：落地第四节遗留问题的固件端根治方案。[hid_ble.c](file:///f:/py_web/ble_hid/main/hid_ble.c) 两处改动：① `hid_ble_init` 挂载 NimBLE config store（`ble_store_config_init()`，IDF 移植未纳入公共头文件，参照官方 bleprph 示例**前置声明**直接调用即可），配对密钥持久化到 NVS `nimble_bond` 命名空间，重启/OTA 不再丢；② `gap_event` 新增 `BLE_GAP_EVENT_REPEAT_PAIRING` 处理——`ble_gap_conn_find` 取对端地址 → `ble_store_util_delete_peer` 删旧 bond → 返回 `BLE_GAP_REPEAT_PAIRING_RETRY` 允许重新配对（对端携带失效密钥的场景不再死循环）。编译注意：从 Git Bash 调 PowerShell 时 MSYS 环境变量会泄入子进程，ESP-IDF 检测 `MSYSTEM` 拒绝运行，需在 PowerShell 内部先 `Remove-Item Env:\MSYSTEM`（`env -u` 无效，外层包装进程会重新注入）；版本号由 git describe 自动生成（无 tag 时为 commit hash），提交后构建即嵌入干净哈希。已走 OTA 全流程验证：push 1.2MB → sha256 verified → 重启 ota_0 → WS 鉴权重注册 → `health_check: passed` → `rollback_state: valid`。**待用户验证**：受控电脑需删除旧配对并重新配对一次（清掉主机侧失效 LTK），此后 ESP32 重启电脑应能自动重连成功。
- **运行验证与 ESP32 链路测试补齐（23 → 44 个用例）**：本地启动 uvicorn 验证 lifespan 迁移后启动正常；跑通 `tests/smoke_test.py` E2E 冒烟（register → WS 注册 → action 下发 → 设备 ack → logs）。期间修复三类问题：① smoke test 502——httpx 默认走 Windows 系统代理（Clash），已加 `trust_env=False` 本机直连；② 控制台日志每条重复两次——`LOG_PIPELINE` 的 default（INFO）与 debug（DEBUG）两个 handler 都挂 console，DEBUG 级 handler 放行所有 ≥DEBUG 记录，已为 debug 管道增加 `only_below: INFO` 过滤器（`app/core/logger/logger.py` 支持 `only_below` 配置项）；③ 13 处 `datetime.utcnow()` 弃用（`ws/device.py`、`ws/client.py`、`crud/device.py`、`crud/device_bond_slot.py`、`crud/device_wifi_known.py`）统一改为 `datetime.now(timezone.utc)`，同时消除了 naive `.timestamp()` 按本地时区解释的隐患。新增 `tests/test_ws_device.py`（8 用例：register ack 与持久化、缺 device_id 拒绝 E004/1008、V1 legacy 帧兼容、msg_id 幂等去重、heartbeat/status 状态上报、action 派发-ACK 全链路 E2E、离线设备 E002 快速失败、同设备重连踢旧连接 1012）与 `tests/test_device_services.py`（13 用例：`_parse_ack_payload` 各分支、AckTracker resolve/timeout、OtaTracker 进度管理、幂等去重语义）。WS 测试要点：`device_ws` 不走依赖注入（内部自建 `SessionLocal`），测试需 monkeypatch `app.api.ws.device.SessionLocal` 为 `TestingSessionLocal`；WS 会话与 HTTP 请求必须共享同一 TestClient portal（`with TestClient(app)`），否则 `ack_tracker` 的 Future 会跨事件循环。
- **真实键鼠接管：Pointer Lock 透传（2026-10-21）**：新增 `vue-project/src/components/MouseTakeoverPanel.vue` 挂载于控制中枢（触控板下方，同样仅单选在线设备时可用）。通过浏览器 Pointer Lock API 捕获控制端电脑的**真实鼠标**原始增量（优先 `unadjustedMovement: true` 绕过 OS 鼠标加速度，不支持的浏览器自动回退普通锁定），1:1 直通流式管线（10ms 合并冲刷 + 超 ±127 余量自动分片续发 + WS 直发优先 + HTTP 限流回退），与触控板共用 `/ws/client` 信封协议（`source: 'web-takeover'`）。事件映射：`mousemove.movementX/Y → mouse_move`（无加速度曲线直通）、`mousedown/up → mouse_down/mouse_up`（DOM button 0/1/2 → HID 位掩码 1/4/2，支持拖拽）、`wheel → mouse_scroll`（兼容 deltaMode=line，像素/22 换算步数）、`keydown/up → keyboard_down/keyboard_up`（`KeyboardEvent.code → KEY_*`：字母/数字/F1-F12 正则生成 + 特例表；修饰键状态打包为 `modifiers` 位掩码 1/2/4/8）。Esc 退出（浏览器内置语义）或 HUD 按钮退出；退出/失焦时清空流式队列并尽力补发仍按住键鼠的 up 动作防止目标机粘键。键盘事件绑 document（Pointer Lock 不转移键盘焦点），鼠标事件绑锁定遮罩元素。
- **配套固件增强（需重新编译烧录后生效）**：① `hid_usb.c` / `hid_ble.c` 新增 `mouse_down` / `mouse_up` 原语（维护 `s_mouse_buttons` 位掩码状态），并修正 `mouse_move` / `mouse_scroll` 报告此前硬编码 buttons=0 的隐患——拖拽场景下移动报告会把按住的键清掉；USB unmount 与 BLE GAP disconnect 时重置该状态防粘键。② `executor.c` 新增 `mouse_down` / `mouse_up` 动作分支，`keyboard_down` / `keyboard_up` 增加可选 `modifiers` 字段（与 keymap 解析出的 mods 按位 OR），任意组合键透传不再受预置组合键名表限制。③ `keymap.c` 扩充 24 条目：US 布局标点 9 键（`KEY_LBRACKET` / `KEY_SEMICOLON` / `KEY_COMMA` 等）、编辑键 7 键（`KEY_CAPSLOCK` / `KEY_DELETE` / `KEY_HOME` / `KEY_PAGEDOWN` 等）、修饰键 8 键（`KEY_LEFTCTRL` 等，code=0 + modifier 位形式，右修饰键映射到左位）。后端零改动（`ActionRequest.payload` 为纯透传 `Dict[str, Any]`）。**已知限制**：固件 HID 键盘报告是"单键 + 修饰键"模式（`hid_*_up` 清空整个报告），不支持多键同按（N-key rollover）；打字与组合键场景无碍，游戏类多键并发场景暂不支持。

---

## 四、 遗留关键问题与接手 Agent 任务指引 (Next Steps)

> **状态更新（2026-10-25 接手日）**：下方蓝牙断连问题的**固件端根治已落地并 OTA 上机**（固件 `7af52b0`，见第三节记录）——配对密钥已持久化 NVS + 重配对自动清理旧 bond。剩余动作只有用户侧一次性操作：受控电脑删除旧配对后重新配对一次，此后重启应自动重连。本节保留原分析供追溯。

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
# 1. 启动后端服务 (守护进程；0.0.0.0 会被微信占端口挡住，见第二节)
.\.venv\Scripts\python -m uvicorn app.main:app --host 192.168.68.159 --port 8000

# 2. 编译前端生产包
npm --prefix vue-project run build

# 3. 运行全量自动化测试
.\.venv\Scripts\pytest

# 4. 固件编译 (Docker 环境)
docker run --rm -v "${PWD}/ble_hid:/project" espressif/idf:release-v5.4 idf.py build
#    本机亦装有 ESP-IDF v5.4.1 (C:\Users\zz101\esp，venv 为 py3.11)：
$env:IDF_TOOLS_PATH="C:\Users\zz101\esp"; $env:IDF_PYTHON_ENV_PATH="C:\Users\zz101\esp\python_env\idf5.4_py3.11_env"; . C:\Users\zz101\esp\v5.4.1\esp-idf\export.ps1; idf.py build

# 5. 固件串口烧录 (COM5，USB 线需接在本机)
.\.venv\Scripts\python ble_hid/flash.py --port COM5 --no-monitor
#    设备在线时优先走 OTA（无需拔线）：admin 上传固件后 POST /api/v2/firmwares/{id}/push
```
