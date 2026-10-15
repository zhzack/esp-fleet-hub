# HID Gateway — 接口与协议参考文档

> 版本：v2.1 — 涵盖服务端 REST API、WebSocket envelope 协议、ESP32 固件
> 暴露的 LAN API、DB 数据模型与错误码。
>
> **维护规则：每次修改任何接口必须同步更新本文档；本文档是接口的唯一权威来源。**

---

## 启动方式

完整 Docker 用法、运维命令与故障排查见 **[docker_usage.md](docker_usage.md)**。这里只列最简启动：

### 开发（前后端分离 + 热更新）

```bash
docker compose -f compose.dev.yml up
```

入口：**http://localhost:5173**（vite 把 `/api`、`/ws` 代理到 backend）。

### 生产（单镜像，前端 baked）

```bash
docker compose -f compose.prod.yml up -d --build
```

入口：**http://localhost:8000**。

---

## 目录

- [一、约定](#一约定)
- [二、鉴权](#二鉴权)
- [三、用户与设备管理（V1）](#三用户与设备管理v1)
- [四、设备命令（V2 云路径）](#四设备命令v2-云路径)
- [五、槽位（slots）与多目标 send](#五槽位slots-与多目标-send)
- [六、WiFi 已知凭据](#六wifi-已知凭据)
- [七、宏（macros）](#七宏macros)
- [八、动作日志](#八动作日志)
- [九、WebSocket envelope 协议（Server ⇄ ESP32）](#九websocket-envelope-协议server--esp32)
- [十、LAN HTTP 协议（设备本机暴露）](#十lan-http-协议设备本机暴露)
- [十一、固件命令清单（cmd_router）](#十一固件命令清单cmd_router)
- [十二、Action payload](#十二action-payload)
- [十三、数据库模型](#十三数据库模型)
- [十四、错误码](#十四错误码)
- [十五、附录](#十五附录)

---

## 一、约定

### 路径前缀

| 前缀 | 说明 |
|---|---|
| `/api/v1/...` | 旧版用户/设备/宏的 CRUD |
| `/api/v2/...` | 设备命令、槽位、多目标 send、WiFi 凭据 |
| `/ws/device` | ESP32 长连接入口 |

### 通用响应包装

非 envelope 类的 v2 设备命令路由返回 dispatcher 统一壳：

```jsonc
{
  "msg_id":      "uuid-v4",
  "device_id":   "esp32_001",
  "status":      "acked" | "failed" | "timeout",
  "exec_time_ms": 12,
  "error_code":   "E006",          // 非 acked 才出现
  "error_message":"ack timeout",
  "result": {                       // 设备 ACK 的 payload
    "ok": true,
    "exec_time_ms": 12,
    "err_code": null,
    "err_msg":  null,
    "result": { /* 业务数据，见每条命令 */ }
  }
}
```

> **两层 result**：外层是服务端编排返回（acked/failed/timeout），内层是设备 ACK 的 `payload.result`。前端用 `extractResult` 自动剥两层。

### 时间格式

所有时间字段返回 ISO-8601 字符串：`2026-05-30T12:34:56`。

### MAC / BLE 地址

字符串 `AA:BB:CC:DD:EE:FF`，大写，冒号分隔。下发到固件的 BLE 命令 payload 内 `addr` 是 6 个 byte 数组。

---

## 二、鉴权

`/api/...` 全部需要 JWT。

### `POST /api/v1/auth/login`

支持 **JSON** (`application/json`) 与 **OAuth2 Form** (`application/x-www-form-urlencoded`) 两种请求体格式：

```jsonc
// 方式 1: JSON body
{ "username": "alice", "password": "secret123" }

// 方式 2: Form data
username=alice&password=secret123

// 200 响应
{ "access_token": "eyJ...", "token_type": "bearer", "user": { "id":1, "username":"alice", "role":"user" } }
```

后续请求带 `Authorization: Bearer <token>`。

### `POST /api/v1/auth/register`

```jsonc
{ "username": "bob", "password": "..." }
```

返回同 login。

### `POST /api/v1/auth/change-password`

用户在已登录状态下修改当前账户密码：

```jsonc
// Header: Authorization: Bearer <token>
// body
{ "old_password": "old_secret", "new_password": "new_secret_123" }

// 200
{ "message": "Password updated successfully" }
```

### `GET /api/v1/auth/me`

返回当前 token 对应的用户信息。


---

## 三、用户与设备管理（V1）

### `GET /api/v1/devices`

列出当前用户拥有 + 未认领的设备。

```jsonc
[ { "id":1, "device_uuid":"esp32_001", "name":"...", "capabilities":["ble_keyboard"],
    "status":"online", "last_online_at":"...", "owner_user_id":1 }, ... ]
```

### `GET /api/v1/devices/online`

```jsonc
{ "online": ["esp32_001", "esp32_002"] }
```

### `GET /api/v1/devices/{id}`、`POST /api/v1/devices`、`PUT /api/v1/devices/{id}`

CRUD。`POST` body：

```jsonc
{ "device_uuid":"esp32_xxx", "name":"...", "capabilities":["ble_keyboard","usb_mouse"] }
```

### `POST /api/v1/devices/{id}/claim`

把无主设备认领到当前用户。

### Capabilities 枚举

```
usb_keyboard | usb_mouse | ble_keyboard | ble_mouse
```

---

## 四、设备命令（V2 云路径）

服务端把每个 HTTP 请求翻译成 envelope 经 WS 下发设备，等待 ACK 后透传给调用方。响应统一是 §一 的 dispatcher 壳。`{uuid}` 都是 `device_uuid`。

### 4.1 状态

| 路径 | 方法 | body | result.result |
|---|---|---|---|
| `/api/v2/devices/{uuid}/status` | GET | — | `{device_id, fw, uptime_ms, wifi:{...}, ble:{connected}, usb:{mounted}, transport:"ble"|"usb"}` |

### 4.2 WiFi

| 路径 | 方法 | body | result.result |
|---|---|---|---|
| `/api/v2/devices/{uuid}/wifi/status` | GET | — | `{connected, ssid, ip, rssi}` |
| `/api/v2/devices/{uuid}/wifi/scan` | POST | — | `{networks:[{ssid,rssi,auth}]}` |
| `/api/v2/devices/{uuid}/wifi/connect` | POST | `{ssid, password?}` | `{rolled_back, ip}` — 成功后**自动落库**到 `device_wifi_known`，密码 Fernet 加密 |

### 4.3 BLE

| 路径 | 方法 | body | result.result |
|---|---|---|---|
| `/api/v2/devices/{uuid}/ble/info` | GET | — | `{connected, device_name, peer?:"AA:BB:..."}` |
| `/api/v2/devices/{uuid}/ble/bonds` | GET | — | `{bonds:[{addr,addr_type}]}` |
| `/api/v2/devices/{uuid}/ble/unpair` | POST | `{addr_type, addr:[6 bytes int]}` | `{}` |
| `/api/v2/devices/{uuid}/ble/unpair_all` | POST | — | `{}` |
| `/api/v2/devices/{uuid}/ble/disconnect` | POST | — | `{}` |
| `/api/v2/devices/{uuid}/ble/switch` | POST | `{addr_type, addr:[6 bytes int]}` | `{}` — 触发 directed advertising |
| `/api/v2/devices/{uuid}/ble/scan` | POST | `{duration_ms?:4000}` | `{devices:[{addr,addr_type,rssi,name}]}` — Observer 角色，duration 上限 10000ms |

### 4.4 USB / Transport

| 路径 | 方法 | body | result.result |
|---|---|---|---|
| `/api/v2/devices/{uuid}/usb/info` | GET | — | `{mounted}` |
| `/api/v2/devices/{uuid}/transport` | GET | — | `{mode:"auto|ble|usb", active:"ble|usb", ble_connected, usb_mounted}` |
| `/api/v2/devices/{uuid}/transport` | POST | `{mode:"auto|ble|usb"}` | `{}` |

### 4.5 系统

| 路径 | 方法 | body | result.result |
|---|---|---|---|
| `/api/v2/devices/{uuid}/reboot` | POST | — | `{}` — 设备 ACK 后立即重启 |

### 4.6 直接发送 action

```
POST /api/v2/action
body: { "device_id":"esp32_001", "payload":{...action...}, "timeout_ms":3000 }
→ { results:[{msg_id, device_id, status, exec_time_ms, ...}] }
```

action 走 executor 队列；`payload` 见 §十二。

---

## 五、槽位（slots）与多目标 send

### 5.1 槽位 CRUD

每个 ESP32 可以有 1..9 个槽位，每个槽指向一个已配对的 BLE 主机地址。**槽位是服务端概念**，固件不感知。

#### `GET /api/v2/devices/{uuid}/slots`

```jsonc
[ { "slot_index":1, "addr_type":0, "addr":"AA:BB:CC:DD:EE:FF",
    "name":"PC-1", "last_used_at":"2026-05-30T08:00:00" }, ... ]
```

#### `PUT /api/v2/devices/{uuid}/slots/{idx}`  *(idx ∈ 1..9)*

```jsonc
// body
{ "addr_type":0, "addr":"AA:BB:CC:DD:EE:FF", "name":"PC-1" }
// 200
{ "slot_index":1, "addr_type":0, "addr":"AA:BB:CC:DD:EE:FF", "name":"PC-1", "last_used_at":null }
```

`name` 可空，`addr` 自动转大写。

#### `DELETE /api/v2/devices/{uuid}/slots/{idx}`

```jsonc
{ "ok": true }
```

### 5.2 多目标编排发送

#### `POST /api/v2/devices/{uuid}/send`

对每个 target：查 ble.info → 不一致则 ble.switch + 轮询连上 → 发 action。

```jsonc
// body
{
  "action": { /* §十二 */ },
  "slots":   [1, 2],                // 二选一
  "targets": [ {"addr_type":0,"addr":"AA:BB:..","name":"x"} ],
  "require_connected": true,         // 没连上是否算失败
  "switch_timeout_s": 5,
  "action_timeout_ms": 4000
}

// 200
{ "results": [
  { "slot_index":1, "addr":"AA:BB:..", "addr_type":0, "name":"PC-1",
    "switched":false, "connected":true,
    "status":"acked", "exec_time_ms":12, "msg_id":"..." },
  { "slot_index":2, "addr":"DE:AD:..", "addr_type":0, "name":"PC-2",
    "switched":true, "connected":false,
    "status":"timeout",
    "error_code":"E102", "error_message":"peer DE:AD:.. not connected after switch" }
]}
```

错误码：`E101 ble.switch 失败` / `E102 switch 后未连上` / `E103 action 调用异常`。

---

## 六、WiFi 已知凭据

### `GET /api/v2/devices/{uuid}/wifi/known`

```jsonc
[ { "ssid":"home", "label":null, "has_password":true, "last_seen_at":"..." }, ... ]
```

### `PUT /api/v2/devices/{uuid}/wifi/known`

```jsonc
// body
{ "ssid":"home", "password":"...", "label":"家用 5G" }
// 200 — 同 GET 单条
```

`password` 可空（仅记录 SSID + label）；非空时用 Fernet 加密存入 `password_enc`。

### `DELETE /api/v2/devices/{uuid}/wifi/known/{ssid}`

`ssid` 在 URL 中需 URL-encode。返回 `{ok:true}`。

### `POST /api/v2/devices/{uuid}/wifi/apply`

```jsonc
// body
{ "ssid":"home" }
// 200 — 同 wifi.connect 的 envelope 响应
```

> JWT_SECRET 改变后 `password_enc` 解密会失败，触发 `500 password decrypt failed`。需要重新输密码。

---

## 七、宏（macros）

### `GET /api/v1/macros`

```jsonc
[ { "id":1, "user_id":1, "name":"...", "content_json":{"steps":[...]}, "created_at":"..." } ]
```

### `GET /api/v1/macros/{id}`

同上单条；不存在或非自己拥有 → 404。

### `POST /api/v1/macros`

```jsonc
// body
{ "name":"复制粘贴", "steps":[ <action-payload>, ... ] }
// 201 — MacroOut
```

`steps` 为已展开的 action payload 数组（每条形如 §十二），不含前端编辑期的 `id` / `repeat`。

### `PUT /api/v1/macros/{id}`

```jsonc
// body — 任一字段可省
{ "name":"...", "steps":[...] }
// 200 — MacroOut
```

### `DELETE /api/v1/macros/{id}`

204。

---

## 八、动作日志
 
-### `GET /api/v2/logs?limit=100`
+### `GET /api/v2/logs`

支持多维度条件筛选与分页：
- `limit` (int, 默认 100，上限 1000): 返回条数
- `offset` (int, 默认 0): 分页偏移量
- `status` (string, 可选): 筛选状态 (`acked` / `failed` / `pending` / `sent`)
- `action_type` (string, 可选): 筛选动作类型 (如 `keyboard_tap`, `text`, `mouse_move`, `macro` 等)
- `device_id` (int, 可选): 筛选指定设备的日志

示例请求：
`GET /api/v2/logs?limit=50&status=acked&action_type=keyboard_tap`
 
 ```jsonc
 [ { "id":1, "msg_id":"...", "user_id":1, "device_id":1,
     "action_type":"keyboard_tap", "payload":{...},
     "status":"acked","error_code":null,"error_message":null,
     "exec_time_ms":12, "created_at":"...", "acked_at":"..." } ]
 ```
 
 `status`: `pending` | `sent` | `acked` | `failed`。


---

## 九、WebSocket envelope 协议（Server ⇄ ESP32）

### 9.1 入口

设备：连 `ws://<server>:<port>/ws/device`，无路径参数（token 可选 `?token=`，目前未强制校验）。

### 9.2 envelope 通用结构

```jsonc
{
  "header": {
    "msg_id":     "uuid-v4",
    "type":       "register | heartbeat | action | get_status | wifi.* | ble.* | usb.* | transport.* | reboot | ack | error | ping | pong",
    "device_id":  "esp32_001",
    "timestamp":  1717070000,
    "source":     "esp32 | web | tester | sim | server",
    "ref_msg_id": "uuid-v4",         // 仅 ack/error/pong
    "seq":        0,                  // 可选
    "user_id":    "1",
    "priority":   0
  },
  "routing": {
    "target_devices": ["esp32_001"],
    "broadcast": false,
    "group": null
  },
  "payload": { ... }
}
```

### 9.3 设备 → server

#### register（连上后第一帧）
```jsonc
{ "header":{"type":"register",...},
  "payload":{"device_id":"esp32_001","name":"esp32_001",
             "capabilities":["ble_keyboard"]}}
```
server 立即回 `ack`，payload `{status:"ok", server_session:"..."}`。

#### heartbeat（每 10s）
```jsonc
{ "header":{"type":"heartbeat",...},
  "payload":{"uptime":123,"wifi_rssi":-52,"queue_len":0,"ble_connected":true}}
```
35s 未收到任何帧服务端会主动断开。

#### ack（响应 server 命令）
```jsonc
{ "header":{"type":"ack","ref_msg_id":"<原 msg_id>",...},
  "payload":{"ok":true,"exec_time_ms":8,"result":{...},
             "transport":"ble","ble_connected":true,"usb_mounted":false,
             "peer":{"addr":"AA:BB:..","addr_type":0}}}
```

#### error
```jsonc
{ "header":{"type":"error","ref_msg_id":"...",...},
  "payload":{"code":"E007","message":"no BLE peer connected"}}
```

#### pong
```jsonc
{ "header":{"type":"pong","ref_msg_id":"<原 msg_id>",...} }
```

### 9.4 Server → 设备

任何 `type` 都可以下发；设备 protocol_feed 分发：
- `type=="action"` → 进 executor 队列异步执行
- `type=="ping"` → 立即回 pong
- 其它 → cmd_router 同步执行后 ACK

### 9.5 重复注册去重

同 `device_uuid` 重连：服务端把旧 WS 关掉（`close(1012)`），新 WS 替换 registry。老 WS 的 finally 用 `is` 校验自己仍指向 registry 才 unregister。

---

## 十、LAN HTTP 协议（设备本机暴露）

设备连上 WiFi 后启动 `:80` HTTP server + UDP `:42424` 广播应答。同一组 cmd_router 处理器，路径如下：

| 路径 | 方法 | cmd_type |
|---|---|---|
| `/api/status` | GET | get_status |
| `/api/wifi/status` | GET | wifi.status |
| `/api/wifi/scan` | GET | wifi.scan |
| `/api/wifi/connect` | POST | wifi.connect |
| `/api/ble/info` | GET | ble.info |
| `/api/ble/bonds` | GET | ble.list_bonds |
| `/api/ble/unpair` | POST | ble.unpair |
| `/api/ble/unpair_all` | POST | ble.unpair_all |
| `/api/ble/disconnect` | POST | ble.disconnect |
| `/api/ble/switch` | POST | ble.switch |
| `/api/ble/scan` | POST | ble.scan |
| `/api/usb/info` | GET | usb.info |
| `/api/transport` | GET | transport.get |
| `/api/transport/set` | POST | transport.set |
| `/api/action` | POST | action（**同步**走 cmd_router 而非队列） |
| `/api/reboot` | POST | reboot |
| `/api/cmd` | POST | 通用 — body `{type, payload}` |

通用响应：

```jsonc
{ "ok": true, "code": 0, "result": {...} }
// 或
{ "ok": false, "code": 400, "error": "ssid missing" }
```

### UDP discovery

发 UTF-8 字符串 `ESP32_HID_DISCOVER` 到 `255.255.255.255:42424`，设备回：

```jsonc
{ "magic":"ESP32_HID","device_id":"esp32_001","ip":"192.168.x.x","http_port":80 }
```

### Provisioning 模式

NVS 没存 SSID 时，启动 SoftAP `ESP32-HID-XXXX`（无密码），HTTP `:80`：

| 路径 | 方法 |
|---|---|
| `/` | GET — HTML 配网页 |
| `/api/scan` | GET |
| `/api/connect` | POST `{ssid, pass?, host?, port?}` |
| `/api/status` | GET |

成功后写 NVS + `esp_restart()` 进正常模式。

---

## 十一、固件命令清单（cmd_router）

每条 type 对应 `cmd_router_register` 注册的 handler。云路径（envelope）和 LAN 路径（HTTP）共用。

| type | request payload | ACK result |
|---|---|---|
| `get_status` | `{}` | `{device_id, fw, uptime_ms, wifi:{connected,ssid,ip,rssi}, ble:{connected}, usb:{mounted}, transport}` |
| `wifi.status` | `{}` | `{connected, ssid, ip, rssi}` |
| `wifi.scan` | `{}` | `{networks:[{ssid,rssi,auth}]}` |
| `wifi.connect` | `{ssid, pass?}` | `{rolled_back:bool, ip:string}` |
| `ble.info` | `{}` | `{connected, device_name, peer?}` |
| `ble.list_bonds` | `{}` | `{bonds:[{addr,addr_type}]}` |
| `ble.unpair` | `{addr_type, addr:[6]}` | `{}` |
| `ble.unpair_all` | `{}` | `{}` |
| `ble.disconnect` | `{}` | `{}` |
| `ble.switch` | `{addr_type, addr:[6]}` | `{}` |
| `ble.scan` | `{duration_ms?:4000}` | `{devices:[{addr,addr_type,rssi,name}]}` |
| `usb.info` | `{}` | `{mounted}` |
| `transport.get` | `{}` | `{mode,active,ble_connected,usb_mounted}` |
| `transport.set` | `{mode}` | `{}` |
| `reboot` | `{}` | `{}` |
| `action` | 见 §十二 | `{transport, ble_connected, usb_mounted, peer?}` |
| `ping` | — | （直接回 `pong` envelope，不是 ack） |

---

## 十二、Action payload

```jsonc
{
  "type": "keyboard_tap" | "keyboard_down" | "keyboard_up"
        | "text" | "mouse_move" | "mouse_click"
        | "delay" | "macro",

  // keyboard_*
  "key": "KEY_A | KEY_ENTER | KEY_LCTRL | ...",

  // text
  "content": "hello",
  "interval_ms": 5,                  // 字符间隔，默认 5

  // mouse_move
  "x": -127..127,                    // dx
  "y": -127..127,                    // dy

  // mouse_click
  "button": 1 | 2 | 4,               // 左 | 右 | 中

  // delay
  "delay_ms": 100,

  // macro 容器
  "steps": [ <action-payload>, ... ]
}
```

### 链路检查

`keyboard_*` / `text` / `mouse_*` 进 executor 时先校验当前 transport 链路是否存在：

- transport=ble 时检查 `hid_ble_connected()`
- transport=usb 时检查 `hid_usb_mounted()`

不存在 → ACK `ok=false, err_code="E007"`。`delay` / `macro` 容器不检查。

### Mouse

只有 `transport.active=="usb"` 才支持鼠标，BLE 传输仅键盘。否则返回 `E004 mouse needs USB transport`。

### 按键名

见固件 [main/keymap.c](../ble_hid/main/keymap.c) — `KEY_A..Z`、`KEY_0..9`、`KEY_ENTER/SPACE/TAB/ESC/BACKSPACE`、`KEY_UP/DOWN/LEFT/RIGHT`、`KEY_F1..F12`、`KEY_LCTRL/LSHIFT/LALT/LMETA` 等。

---

## 十三、数据库模型

```sql
-- 用户
users (
  id BIGINT PK,
  username VARCHAR(64) UNIQUE,
  hashed_password VARCHAR(255),
  role ENUM('admin','user'),
  created_at, updated_at
)

-- 设备
devices (
  id BIGINT PK,
  device_uuid VARCHAR(128) UNIQUE,
  owner_user_id BIGINT FK→users.id,
  name VARCHAR(128),
  capabilities JSON,
  status ENUM('offline','online','busy','error'),
  last_online_at DATETIME,
  created_at, updated_at
)

-- 设备连接历史
device_connections (
  id BIGINT PK,
  device_id BIGINT FK→devices.id,
  ip VARCHAR(64),
  session_id VARCHAR(128),
  connected_at, disconnected_at
)

-- 动作日志
action_logs (
  id BIGINT PK,
  msg_id VARCHAR(64) UNIQUE,
  user_id BIGINT, device_id BIGINT,
  action_type VARCHAR(64),
  payload JSON,
  status ENUM('pending','sent','acked','failed'),
  error_code VARCHAR(16),
  error_message VARCHAR(255),
  exec_time_ms INT,
  created_at, acked_at
)

-- 宏
macros (
  id BIGINT PK,
  user_id BIGINT FK→users.id (CASCADE),
  name VARCHAR(128),
  content_json JSON,                    -- {"steps":[...]}
  created_at, updated_at
)

-- 设备分组
device_groups (id, owner_user_id, name, created_at)
device_group_members (group_id, device_id)

-- 主机槽位（v2 新增）
device_bond_slots (
  id BIGINT PK,
  device_id BIGINT FK→devices.id (CASCADE),
  slot_index INT,                       -- 1..9
  addr_type INT,
  addr VARCHAR(20),                     -- "AA:BB:CC:DD:EE:FF"
  name VARCHAR(64),
  last_used_at DATETIME,
  created_at, updated_at,
  UNIQUE(device_id, slot_index)
)

-- WiFi 已知凭据（v2 新增）
device_wifi_known (
  id BIGINT PK,
  device_id BIGINT FK→devices.id (CASCADE),
  ssid VARCHAR(64),
  password_enc TEXT,                    -- Fernet ciphertext
  label VARCHAR(64),
  last_seen_at DATETIME,
  created_at,
  UNIQUE(device_id, ssid)
)
```

---

## 十四、错误码

| 码 | 出处 | 含义 |
|---|---|---|
| E000 | server | 通用未指定 |
| E002 | dispatcher | 设备离线 |
| E003 | dispatcher | 队列 push 失败 |
| E004 | router / executor | 参数错 / 不支持 type / unknown key |
| E005 | executor | 队列满 |
| E006 | dispatcher | ack timeout |
| E007 | executor | 当前 transport 没 host 链路 |
| E101 | multi_target | ble.switch 调用失败 |
| E102 | multi_target | switch 后超时未连接到目标 peer |
| E103 | multi_target | action 调用异常 |

HTTP 状态码：

- 400 参数错（bad json / missing field）
- 401 未鉴权
- 403 不是设备所有者
- 404 device / slot / macro 不存在
- 500 内部错误（含密码解密失败）
- 502 设备返回非 ok

---

## 十五、附录

### 15.1 设备 capabilities 枚举

```
usb_keyboard | usb_mouse | ble_keyboard | ble_mouse
```

### 15.2 BLE addr_type

| 值 | 含义 |
|---|---|
| 0 | Public address |
| 1 | Random static address |
| 2 | Resolvable private address (RPA) |
| 3 | Non-resolvable private address |

### 15.3 transport 模式

```
auto    → 优先 USB（已挂载时），否则 BLE
ble     → 强制走 BLE
usb     → 强制走 USB
active  → 当前实际生效的（auto 解析后）
```

### 15.4 宏步骤编辑器内部表示（前端 only）

编辑期：

```jsonc
{ "id":"abc12345",      // 仅前端拖拽用
  "type":"keyboard_tap",
  "key":"KEY_A",
  "repeat": 3 }         // 保存时被 flattenForSave 展开成 3 条
```

落库的 `content_json.steps[]` 不带 `id` / `repeat`。

### 15.5 WS 重连退避

固件：`reconnect_timeout_ms=5000`（固定 5s，无指数退避）。

### 15.6 BLE 扫描参数

设备 Observer 角色：50ms 间隔 / 40ms 窗口（active scan，约 80% duty）。`duration_ms` 上限 10000ms。

### 15.7 WiFi 凭据加密

Fernet 对称加密，密钥由 `JWT_SECRET` 派生（`SHA-256("wifi-cred:" + JWT_SECRET)`）。**JWT_SECRET 改变后已存密码不可解密。**

### 15.8 编译时配置（固件 Kconfig.projbuild）

| Kconfig | 默认 | 说明 |
|---|---|---|
| `APP_DEFAULT_WIFI_SSID` | `1` | 默认连接 WiFi |
| `APP_DEFAULT_WIFI_PASS` | `11111111` | |
| `APP_DEFAULT_SERVER_HOST` | `192.168.68.159` | WS 服务器 IP |
| `APP_DEFAULT_SERVER_PORT` | `8000` | |
| `APP_DEFAULT_DEVICE_ID` | `esp32_001` | |

NVS 中的值优先于 Kconfig 默认。`flash.py --erase` 可清 NVS 强制使用编译默认。

### 15.9 测试服务器

`tools/ws_tester.py` 监听 `0.0.0.0:8000/ws/device`，对设备发的 register 应 ack 并依次执行 14 项测试用例。
用 `--only` / `--skip` 过滤。

---

## 文档维护检查清单

每次改下面任一处，必须同步更新本文档：

- [ ] `app/api/v1/*.py` / `app/api/v2/*.py` 路由签名变化
- [ ] `app/database/models/*.py` 模型字段变化
- [ ] `ble_hid/main/cmd_router.c` 注册新 type 或字段变化
- [ ] `ble_hid/main/lan_mode.c` 路由表变化
- [ ] `ble_hid/main/protocol.c` envelope 处理变化
- [ ] 错误码新增
- [ ] capabilities / addr_type / transport 等枚举变化
