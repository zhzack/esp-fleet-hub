# 版本变更历史与里程碑记录 (Changelog & Milestones)

> **当前版本**：v2.1.0-industrial  
> **更新日期**：2026-10-26  
> **规范依据**：Keep a Changelog / Semantic Versioning

---

## [v2.1.0] - 2026-10-26

### 🌟 核心突破与重要特性
- **主机平台标记全链路感知**：
  - 新增 `device_host_meta` 数据表与 ORM 模型，支持对受控端打标（`win` / `mac` / `linux`）。
  - 控制台输出目标下拉列表动态识别受控机平台，自动自适应系统快捷键矩阵（Mac Cmd+Tab / 桌面切换 vs Win Win+D / Alt+Tab）。
  - 真实键鼠接管面板组合键按钮组按受控平台动态切换。
- **宏跨平台变体独立分支**：
  - 宏数据模型支持 `variants: {win: [...], mac: [...], linux: [...]}`。
  - 宏编辑器新增通用/Windows/macOS/Linux 独立积木流水线标签页。
  - 宏运行器执行时，按槽位目标主机平台自动分选对应变体下发，结果表回显具体执行配置。
- **BLE 多连接镜像广播（支持 3 台主机）**：
  - 固件支持最多 3 台主机同时连接（`s_conns[3]`），连接未满时保持持续广播。
  - 消除第二台电脑搜索不到外设广播的历史痛点，默认支持多主机同屏镜像遥控。
  - 控制台顶部新增在线主机数实时徽标与悬浮地址提示。
- **控制台快捷设置抽屉重构**：
  - 控制中枢顶部直接复用设备设置抽屉（免去跳转），长文本全面收纳为 `el-tooltip` 提示。

---

## [v2.0.0] - 2026-10-25

### 🚀 稳定性攻坚与性能飞跃
- **鼠标流控三层性能调优 (628 actions/s)**：
  - 后端对流式动作（`mouse_move`）跳过数据库落库与设备查库，实现微秒级内存穿透。
  - SQLite 注入启用 WAL 模式 (`PRAGMA journal_mode=WAL`) 与 `synchronous=NORMAL`，彻底消除事件循环阻塞与读写互斥。
  - TinyUSB 端点采用累加合并缓冲与中断补发机制，提升报告率上限至 1000Hz (bInterval 1ms)。
  - 解决鼠标移动中偶发丢失按键与点击问题（`s_buttons_dirty` 脏位保障）。
- **macOS BLE 配对死循环根治**：
  - HID 报告特征与 CCCD 强制声明加密访问权限（`READ_ENC` / `WRITE_ENC`），促使 macOS 主机发起标准 SMP 配对。
  - 启用 NimBLE NVS 持久化（`ble_store_config_init`），LTK 永久固化在 Flash，重启后 3 秒内免重配即刻恢复重连。
- **消除 Tmr Svc 栈溢出崩溃循环**：
  - 解决将 BLE 通知链直接置于 FreeRTOS 软件定时器（3KB 栈）引发的系统复位循环。
  - 建立专用的 `ble_m_drain` 任务（4KB 栈）处理排空，并依据蓝牙连接间隔（15ms）实行节拍自适应调度。
- **安全加固与设备挑战鉴权**：
  - 环境变量 `.env` 彻底移出版本控制，加入 `.gitignore` 并轮换 JWT 与数据库密钥。
  - 设备 WebSocket 注册接入 HMAC-SHA256 挑战-应答机制（单次一次性 Nonce 防重放）。
- **双槽 OTA 安全自检与看门狗自动回滚**：
  - 分区表升级为 `otadata` + `ota_0` + `ota_1` 双槽架构。
  - 下载过程进行流式 SHA-256 校验，90 秒在线健康自检与超时自动回滚保护。
- **单内核统一命令路由**：
  - 重构串口 CLI（`serial_cli.c`）为薄翻译层，所有输入源统一汇聚至 `cmd_router.c`。
- **传输模式 NVS 持久化**：
  - 传输通道（`auto` / `usb` / `ble`）显式持久化至 NVS，重启与 OTA 后配置不丢失。

---

## [v1.2.0] - 2026-10-21

### 🛠️ 控制中枢升级与真实接管
- **真实键鼠直通接管 (Pointer Lock)**：
  - 基于 HTML5 Pointer Lock API 捕获原始无加速度位移，实现 1000Hz 1:1 直通目标机。
  - 支持全键盘按键码正则生成与修饰键位掩码打包。
  - 具备退出主动清空队列与按键抬起安全释放机制。
- **控制中枢多设备并发下发**：
  - 设备选择器改多选，后端使用 `asyncio.gather` 并发派发。
  - 指令流终端逐设备展示 ACK 结果与耗时。
- **统一输出目标卡片**：
  - 将 USB 直连与 BLE 槽位合并为统一的目标选择卡片。

---

## [v1.1.0] - 2026-10-20

### 🧹 工程整理与测试覆盖
- 规范文档目录为 `docs/`，移除废弃文件。
- 解决 `BIGINT AUTOINCREMENT` 跨库兼容问题（SQLite vs MySQL）。
- 修复负向业务确认（如 E007）被重试机制判定为虚假成功 ACK 的深层 Bug。
- 自动化测试用例由 6 个扩展至 23 个，全面覆盖 schemas、devices、macros 与 lifespan。
- Pydantic 模型配置由废弃的 `class Config` 迁移为 `model_config = ConfigDict(from_attributes=True)`。

