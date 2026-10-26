# 核心应用场景与业务交互流程 (User Scenarios & Sequence Flows)

> **文档版本**：V2.1  
> **适用模块**：全链路业务流  
> **编写日期**：2026-10-26  

---

## 1. 场景一：硬件上电配网与长连接安全鉴权

本场景描述 ESP32-S3 设备从物理加电、建立 Wi-Fi 连接，到向后端网关发起 WebSocket 握手并完成 HMAC-SHA256 挑战-应答鉴权的全流程。

```mermaid
sequenceDiagram
    autonumber
    participant ESP as ESP32-S3 硬件
    participant Server as FastAPI 网关 (/ws/device)
    participant DB as SQLite / MySQL

    ESP->>ESP: 上电读取 NVS (WiFi, device_uuid, device_secret)
    ESP->>ESP: 连接局域网路由器并获取 IP (如 192.168.68.142)
    ESP->>Server: WebSocket 连接请求 ws://gateway:8000/ws/device
    Server-->>ESP: WebSocket 握手建立成功
    ESP->>Server: 发送注册请求帧 (type: "register", payload: {device_uuid: "esp32_001"})
    
    alt 服务端配置了 DEVICE_SHARED_SECRET
        Server->>Server: 生成随机一次性 auth_nonce
        Server-->>ESP: 下发挑战帧 (type: "auth_challenge", nonce: "abc123xyz...")
        ESP->>ESP: 使用 mbedtls 计算 HMAC-SHA256(device_secret, nonce)
        ESP->>Server: 重发注册帧 (payload: {device_uuid: "esp32_001", auth: {nonce, proof}})
        Server->>Server: 比对 HMAC proof，校验合法并使 nonce 作废
    end

    Server->>DB: 更新设备状态 online，刷新 last_online_at
    Server-->>ESP: 返回注册确认 (status: "ok", device_id: 1)
    
    loop 保持心跳 (每 30 秒)
        ESP->>Server: 心跳帧 (uptime, free_heap, ble_peers)
        Server->>DB: 刷新设备存活时间戳
    end
```

---

## 2. 场景二：Web 控制台低延迟击键与平台感知快捷键

操作员在 Web 前端控制台点击机械键帽或系统快捷键，指令通过网关实时投递至受控主机，并根据受控机操作系统自动适配键位。

```mermaid
sequenceDiagram
    autonumber
    participant User as 操作员 (浏览器 Web UI)
    participant Vue as 前端 Console.vue
    participant Server as FastAPI 调度器 (dispatcher.py)
    participant ESP as ESP32-S3 (cmd_router / executor)
    participant Host as 受控主机 (Mac / PC)

    User->>Vue: 在“输出目标”下拉选择目标电脑 (例如已标记为 Mac 的槽位)
    Vue->>Vue: 动态切换快捷键视图为 macOS 键位 (Cmd+Tab, 桌面切换, Cmd+Space)
    User->>Vue: 点击「应用切换」快捷键按钮
    Vue->>Server: POST /api/v2/action (type: "combo", keys: ["KEY_LEFTMETA", "KEY_TAB"])
    Server->>Server: 查询目标设备连接，构造 WsEnvelope (need_ack: true)
    Server->>ESP: WS 下发 action 帧
    ESP->>ESP: cmd_router 路由 -> executor 动作队列入队
    ESP->>Host: HID 键盘报告 (Modifiers: 0x08, Keycode: 0x2B Tab)
    ESP->>Host: 清空 HID 键盘报告 (全释放)
    ESP-->>Server: 回传 ACK (msg_id: "...", status: "ok", exec_time_ms: 3)
    Server->>Server: ack_tracker resolve Future 并释放追踪
    Server-->>Vue: HTTP 响应 200 OK (latency: 3ms)
    Vue->>User: 控制台实时流水终端打印绿色成功条目与毫秒级延迟
```

---

## 3. 场景三：Pointer Lock 真实键鼠流式接管 (1:1 直通)

操作员点击控制台“接管键鼠”面板，浏览器锁定鼠标光标，捕获物理鼠标原始增量与真实键盘按压，高频低延迟直通受控机。

```mermaid
sequenceDiagram
    autonumber
    participant Mouse as 操作员物理鼠标
    participant Browser as 浏览器 (PointerLockPanel)
    participant Server as 后端网关 (/ws/client)
    participant ESP as ESP32-S3 (hid_usb.c)
    participant Host as 受控电脑

    Browser->>Browser: 请求 requestPointerLock(unadjustedMovement: true)
    Mouse->>Browser: 鼠标极速甩动 (连续产生 mousemove 事件)
    Browser->>Browser: 10ms 聚合缓冲微批处理 (合并 dx, dy, 超 ±127 自动分片)
    Browser->>Server: WebSocket /ws/client 发送 mouse_move (二进制/精简信封)
    Server->>Server: dispatcher 识别流式动作：跳过数据库落库，直接转发
    Server->>ESP: /ws/device 高频下发 mouse_move (dx, dy)
    ESP->>ESP: dx, dy 累加至 pending 增量缓冲 (临界区保护)
    
    alt TinyUSB 中断端点空闲
        ESP->>Host: tud_hid_n_report 发送鼠标报告
    else 端点忙 (上一报告未轮询走)
        ESP->>ESP: 增量保留在 pending 缓冲中
        Host-->>ESP: tud_hid_report_complete_cb 触发中断回调
        ESP->>Host: 立即补发剩余积压增量 (零丢失)
    end

    Note over Mouse,Host: 按键防丢保障 (s_buttons_dirty)
    Mouse->>Browser: 按下左键并拖拽
    Browser->>Server->>ESP: mouse_down (buttons: 0x01)
    ESP->>ESP: 标记 s_buttons_dirty = true
    ESP->>Host: 随下一次移动报告合并发送鼠标按键按下状态 (支持拖拽，不粘键)
```

---

## 4. 场景四：BLE 多连接镜像广播与定向切换

ESP32-S3 同时与多台电脑（如 Mac 笔记本与 Windows 台式机）建立蓝牙连接，实现一套外设同屏镜像遥控。

```mermaid
sequenceDiagram
    autonumber
    participant Mac as Mac 笔记本 (Grey-2)
    participant PC as Windows PC
    participant ESP as ESP32-S3 (hid_ble.c)
    participant Web as 前端控制台

    ESP->>ESP: 启动 NimBLE 广播 (Name: "esp32_001")
    Mac->>ESP: 连接请求 -> 协商连接间隔 15ms -> 配对建链 (CONN 0)
    Note over ESP: 当前连接数 1 < 3，继续保持蓝牙广播
    PC->>ESP: 扫描到广播并发起连接 -> 连接建立 (CONN 1)
    ESP->>ESP: 记录 s_conns[2] 连接表，取最小连接间隔 15ms 设定自适应节拍
    ESP->>Web: 上报 ble.info (count: 2, peers: [Mac_Addr, PC_Addr])
    Web->>Web: 控制台顶部徽标点亮「×2 镜像广播」，显示 2 台在线
    
    Web->>ESP: 下发按键或鼠标移动动作
    ESP->>ESP: 循环遍历 s_conns 活跃连接
    par 镜像通知 Mac
        ESP->>Mac: notify_input_report (独立 mbuf)
    and 镜像通知 PC
        ESP->>PC: notify_input_report (独立 mbuf)
    end
    Note over Mac,PC: Mac 与 PC 屏幕光标完全同步移动、同步点击
```

---

## 5. 场景五：跨平台宏流水线编排与智能分发

用户创建自动化脚本（例如“打开浏览器控制台并全屏”），由于 Mac 与 Windows 的快捷键差异，宏引擎在运行时根据被控机系统类型自动选取对应变体。

```mermaid
sequenceDiagram
    autonumber
    participant User as 操作员
    participant Editor as 前端宏编辑器 (macros.vue)
    participant API as 后端 REST (/api/v2/macros)
    participant Multi as 多目标并发器 (multi_target.py)
    participant ESP as ESP32-S3

    User->>Editor: 编辑宏流水线，配置【通用】、【Windows 变体】、【macOS 变体】
    Note over Editor: macOS 步骤: Cmd+Option+I -> 延时 500ms -> Cmd+Ctrl+F<br>Win 步骤: F12 -> 延时 500ms -> F11
    Editor->>API: 保存宏定义 (content_json: {steps, variants: {mac: [...], win: [...]}})
    
    User->>Editor: 勾选目标设备执行宏
    Editor->>Multi: 执行请求 (POST /api/v2/devices/{uuid}/commands 或 actions)
    Multi->>Multi: 检查目标主机平台标记 (device_host_meta)
    
    alt 目标机标记为 mac
        Multi->>ESP: 派发 macOS 变体步骤流水线
    else 目标机标记为 win
        Multi->>ESP: 派发 Windows 变体步骤流水线
    else 未标记平台
        Multi->>ESP: 降级回退执行通用步骤
    end

    ESP->>ESP: executor 任务依次执行步骤与精确延时
    ESP-->>Multi: 全部步骤执行完成 ACK
    Multi-->>Editor: 返回执行报表，标识“已使用 macOS 变体”
```

---

## 6. 场景六：双槽 OTA 安全升级与 90 秒自动回滚

管理员在前端页面推送新固件，设备在线下载并校验，若新固件异常无法联网，系统触发自动回滚，确保设备永不失联。

```mermaid
sequenceDiagram
    autonumber
    participant Admin as 管理员 (Web 固件中心)
    participant Server as FastAPI 网关 (/api/v2/firmwares)
    participant ESP as ESP32-S3 (ota_mgr.c)
    participant BL as ESP32 Bootloader

    Admin->>Server: 上传固件 build.bin (计算 SHA-256 并入库)
    Admin->>Server: 点击「一键推送升级」(选定 esp32_001)
    Server->>ESP: 下发 ota.start (url: "http://.../download", sha256: "9f8a...")
    ESP->>ESP: 启动异步 ota_task，流式拉取二进制数据
    ESP->>ESP: 写入待升级分区 (如当前跑 ota_0，写入 ota_1)
    ESP->>ESP: mbedtls_sha256 实时累加计算哈希
    ESP-->>Server: 实时上报进度 ota_progress (10%, 50%, 100%)
    
    alt SHA-256 校验失败
        ESP->>ESP: 报错 sha256 mismatch，拒绝切换分区并继续稳定运行旧固件
    else SHA-256 校验成功
        ESP->>ESP: esp_ota_set_boot_partition(ota_1) 并调用 esp_restart()
        ESP->>BL: 芯片重启，Bootloader 检测到分区标记为 PENDING_VERIFY
        BL->>ESP: 启动 ota_1 新固件，启动 ota_health_task (90s 倒计时)
        
        alt 正常情况：90s 内成功连入服务端
            ESP->>Server: WS 鉴权连接并完成注册
            ESP->>ESP: 保持在线 5s 判定健康，调用 esp_ota_mark_app_valid_cancel_rollback()
            ESP->>Server: 上报 ota.status (health_check: passed, rollback_state: valid)
        else 异常情况：固件 Panic 崩溃死机 或 90s 超时无法联网
            ESP->>ESP: 90s 倒计时结束，调用 esp_ota_mark_app_invalid_rollback_and_reboot()
            ESP->>BL: 硬件重启，Bootloader 自动回退引导 ota_0 旧固件！
            BL->>ESP: 旧固件重新上线，设备零变砖，保持正常运行
        end
    end
```
