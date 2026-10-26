# 后端网关系统详细设计说明书 (Backend Gateway Subsystem)

> **模块路径**：`app/`  
> **文档版本**：V2.1 工业级标准  
> **更新时间**：2026-10-26  
> **技术栈**：Python 3.10+ / FastAPI / SQLAlchemy 2.0 / Pydantic v2 / Uvicorn

---

## 1. 模块定位与职责边界

后端网关是整个 HID Gateway Platform 的**调度中枢与业务引擎**，承担多客户端与多硬件设备之间的双向会话维持、安全鉴权、指令分发、状态追踪、多目标并发与系统审计。

### 1.1 核心职责
1. **统一长连接枢纽**：通过 `/ws/device` 维持硬件的长效状态，通过 `/ws/client` 提供前端低延迟流式接入。
2. **高速指令调度与分级流控**：对高频鼠标位移实行零磁盘 IO 穿透，对审计指令通过 WAL 模式实现微秒级异步落库。
3. **精准 ACK 追踪器**：基于 Python 异步 Future 机制，将硬件端的物理执行结果无缝映射为上层 REST 响应。
4. **多目标并发编排**：支持将动作并发派发至多台物理设备（`asyncio.gather` 并发调度），实现同屏集群控制。
5. **设备与固件生命周期**：管理设备配对槽位、WiFi 凭证加密存储、目标平台标记（`device_host_meta`）及双备份 OTA 升级流。

---

## 2. 后端核心架构与目录规划

```
app/
├── api/                  # 接口表现层
│   ├── router.py         # 顶层 API 路由汇总 (/api/v1, /api/v2, /dev)
│   ├── v1/               # V1 基础业务 CRUD (auth, devices, macros, scripts)
│   ├── v2/               # V2 增强与控制路由 (actions, commands, slots, host_meta, firmwares)
│   ├── ws/               # WebSocket 长连接端点
│   │   ├── device.py     # 设备长连接：HMAC挑战鉴权、心跳维持、ACK解析、OTA上报
│   │   └── client.py     # 客户端长连接：虚拟触控板、键鼠接管流式入口
│   └── dev/              # 开发者探针与调试端点
├── core/                 # 核心基础设施
│   ├── config/config.py  # Pydantic BaseSettings 强类型环境配置
│   ├── logger/           # colorlog 彩色日志管道与按等级过滤机制
│   ├── security.py       # JWT 生成校验与密码 Bcrypt 哈希
│   └── wifi_crypto.py    # AES-CBC 算法加解密数据库 WiFi 凭据
├── database/             # 数据持久层
│   ├── base.py           # ORM 模型统一汇聚导出
│   ├── session.py        # 数据库引擎与 WAL 模式注入
│   ├── models/           # SQLAlchemy 数据表模型 (双库自增兼容)
│   ├── schemas/          # Pydantic 数据契约与 WsEnvelope 信封
│   └── crud/             # 领域数据访问对象 (CRUD)
├── services/             # 核心领域业务服务
│   ├── dispatcher.py     # 核心调度分发器 (流式旁路、重试控制、错误透传)
│   ├── ack_tracker.py    # 基于 asyncio.Future 的精准物理 ACK 跟踪器
│   ├── multi_target.py   # 多设备/多槽位并发分发与结果聚合器
│   ├── ota_tracker.py    # 内存态设备 OTA 升级进度监视器
│   ├── device_registry.py# 进程内活跃 WebSocket 连接池管理
│   └── routing.py        # 消息路由寻址解析器
└── main.py               # FastAPI 启动入口、CORS、生命周期 lifespan、SPA 静态托管
```

---

## 3. 核心机制与关键代码实现

### 3.1 高频鼠标流式穿透与 WAL 事务优化 ([`dispatcher.py`](file:///data/code/blue/py_web/app/services/dispatcher.py))

#### 瓶颈定位
常规动作下发会在 `action_logs` 表插入 `pending` 状态并在收到 ACK 时更新为 `acked`。在高速鼠标甩动时（~100 包/秒），每次请求产生 2 次数据库事务提交。传统数据库配置每次 `commit` 触发磁盘 `fsync`，耗时 1~15ms，彻底阻塞 FastAPI 的 asyncio 单线程事件循环，导致严重的网络堆积和鼠标卡顿。

#### 优化实现
1. **流式免落库旁路 (Streaming Bypass)**：
   ```python
   def _payload_needs_ack(payload: dict[str, Any] | None) -> bool:
       if not payload:
           return False
       action_type = payload.get("action_type") or payload.get("type")
       # 高频流式位移瞬态数据跳过落库
       if action_type in ("mouse_move", "mouse_scroll"):
           return False
       return True
   ```
   在 `deliver_to_device` 调度链路中，检测到流式动作直接通过 WebSocket 发送，不查 `devices` 表、不写 `action_logs` 表，完全内存化直通。

2. **SQLite WAL 模式极速提交 ([`session.py`](file:///data/code/blue/py_web/app/database/session.py))**：
   ```python
   @event.listens_for(engine, "connect")
   def _set_sqlite_pragma(dbapi_connection, connection_record):
       if "sqlite" in str(engine.url):
           cursor = dbapi_connection.cursor()
           cursor.execute("PRAGMA journal_mode=WAL")
           cursor.execute("PRAGMA synchronous=NORMAL")
           cursor.close()
   ```
   `WAL`（Write-Ahead Logging）将随机写转化为顺序追加，`synchronous=NORMAL` 免除了全量 fsync，写入延迟降低到微秒级，且彻底解除了 SQLite 的读写锁互斥。

### 3.2 精准 ACK 异步跟踪器 ([`ack_tracker.py`](file:///data/code/blue/py_web/app/services/ack_tracker.py))

为实现 HTTP REST 请求同步等待设备端硬件物理执行完成，后端构建了基于 `asyncio.Future` 的事件关联机制：

```python
class AckTracker:
    def __init__(self):
        self._pending: Dict[str, asyncio.Future] = {}

    def create_pending(self, msg_id: str) -> asyncio.Future:
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        self._pending[msg_id] = fut
        return fut

    def resolve(self, msg_id: str, payload: Any):
        fut = self._pending.pop(msg_id, None)
        if fut and not fut.done():
            fut.set_result(payload)
```
- **时序流程**：`POST /api/v2/action` 生成唯一 `msg_id` 并调用 `create_pending`；调用 `await asyncio.wait_for(fut, timeout=2.0)` 挂起当前协程；设备执行完成后上报带 `ref_msg_id` 的 ACK 帧；WebSocket 接收协程调用 `resolve(ref_msg_id, payload)` 唤醒 HTTP 协程并返回真实硬件执行延迟。

### 3.3 设备 WebSocket 挑战应答鉴权 ([`device.py`](file:///data/code/blue/py_web/app/api/ws/device.py))

```python
# 挑战帧下发
if settings.DEVICE_SHARED_SECRET and not auth_verified:
    nonce = secrets.token_hex(16)
    conn.auth_nonce_issued = nonce
    challenge_envelope = {
        "header": {"type": "auth_challenge", "msg_id": str(uuid4()), "timestamp": int(time.time())},
        "payload": {"nonce": nonce}
    }
    await websocket.send_text(json.dumps(challenge_envelope))
    return

# 凭据校验
expected_proof = hmac.new(
    settings.DEVICE_SHARED_SECRET.encode("utf-8"),
    conn.auth_nonce_issued.encode("utf-8"),
    hashlib.sha256
).hexdigest()

if not hmac.compare_digest(proof.lower(), expected_proof.lower()):
    await websocket.send_text(json.dumps(_error_env(msg_id, "E401", "invalid auth proof")))
    await websocket.close(code=4401)
    return
```

### 3.4 数据库跨引擎自增主键兼容 ([`models/`](file:///data/code/blue/py_web/app/database/models))

#### 问题背景
SQLite 原生不支持 `BIGINT AUTOINCREMENT`，若直接在模型中定义 `BigInteger, primary_key=True, autoincrement=True`，在本地 SQLite 环境插入记录时将抛出 `NOT NULL constraint failed: id` 错误；而在生产 MySQL 中若使用 `Integer` 则存在键溢出风险。

#### 解决方案
在所有 ORM 模型中统一切换为 SQLAlchemy 变体机制：
```python
id = Column(
    BigInteger().with_variant(Integer, "sqlite"),
    primary_key=True,
    autoincrement=True
)
```
- 本地开发：无需安装配置 MySQL，零配置直接运行 SQLite WAL。
- 生产环境：Docker 一键拉起 MySQL 8.0，字段自动映射为标准的 64 位 `BIGINT AUTO_INCREMENT`。

---

## 4. 接口版本演进与兼容性策略

1. **V1 与 V2 路由共存**：
   - `/api/v1/`：提供基础用户注册登录（`auth.py`）、设备基础信息、设备分组（`device_groups.py`）及脚本运行器。
   - `/api/v2/`：遵循 V2 信封标准，负责核心控制链路。包括动作直接派发（`actions.py`）、固件升级（`firmwares.py`）、蓝牙槽位（`slots.py`）与平台标记（`host_meta.py`）。
2. **SPA 前端单镜像托管**：
   - 生产环境中，FastAPI 通过挂载 `vue-project/dist` 并配置 `/{full_path:path}` 的 SPA fallback，实现单一后端容器同时提供 API 服务与 Web 控制台静态资源，免去独立 Nginx 代理运维成本。
