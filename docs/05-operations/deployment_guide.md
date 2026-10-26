# 系统部署与运维全景指南 (Deployment & Operations Guide)

> **文档版本**：V2.1 工业级标准  
> **更新时间**：2026-10-26  
> **适用环境**：本地开发环境 / Docker 容器化编排 / 生产服务器环境

---

## 1. 部署模式总览与选型

系统支持两种主要运行形态：
- **形态 A：本地极速轻量运行 (Native Local)**：基于 Python 虚拟环境 + SQLite (WAL 模式)，适合快速开发、单机验证与无需 Docker 的嵌入式调试。
- **形态 B：Docker Compose 容器化编排 (Containerized)**：
  - **开发环境 (`compose.dev.yml`)**：FastAPI 后端 + Vite 前端热更新 + MySQL 8.0 + Redis。
  - **生产环境 (`compose.prod.yml`)**：单镜像聚合（前端构建产物烘焙进 FastAPI 静态目录）+ MySQL 8.0 数据持久化。

---

## 2. 本地极速轻量运行 (Native Local)

### 2.1 环境准备
- **操作系统**：Linux / macOS / Windows
- **Python 版本**：Python 3.10+ / Python 3.11+（代码使用了 PEP 604 `X | None` 联合类型）
- **Node.js**：Node.js 18+ 与 npm

### 2.2 基础配置 (`.env`)
从模板复制配置文件：
```bash
cp .env.example .env
```
配置说明（针对本地 SQLite 极速模式）：
```ini
PROJECT_NAME="HID Gateway Platform"
VERSION="1.0.0"
DEBUG=True

# 本地直接使用 SQLite (WAL 模式)，自动免去安装 MySQL 成本
DATABASE_URL=sqlite:///./app.db

# 必须提供非空字符串满足 Pydantic 校验 (本地 SQLite 下可填占位符)
MYSQL_PASSWORD=local-placeholder
MYSQL_DB=blue_db

# 设备 WS 鉴权密钥 (空 = 关闭；若需验证鉴权填写 64 字节十六进制字符)
DEVICE_SHARED_SECRET=
QUEUE_BACKEND=memory
```

### 2.3 启动后端网关服务
```bash
# 激活虚拟环境
source .venv/bin/activate    # Linux / macOS
# .\.venv\Scripts\activate   # Windows

# 安装依赖
pip install -r requirements.txt

# 启动后端
python run.py
# 或显式指定 IP 与端口 (推荐)：
python -m uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

> [!WARNING]
> #### 端口 8000 占用与冲突避坑指南
> 1. **Windows 微信（Weixin.exe）冲突陷阱**：微信 PC 端常以 `Bound` 状态占住 `0.0.0.0:8000`，导致 uvicorn 绑定 `0.0.0.0` 时抛出 `[WinError 10048]`。**解决方案**：显式指定局域网物理 IP（如 `--host 192.168.68.159 --port 8000`）。切勿起两个实例分别绑 127.0.0.1 和局域网 IP，否则内存连接池会发生脑裂。
> 2. **Linux 服务器端口占用**：若宿主机 8000 端口已被外部进程占用，可直接指定备用端口（例如 `--port 8008`），并在前端 `vite.config.js` 或浏览器中同步修改。

### 2.4 编译与运行前端
```bash
cd vue-project
npm install
npm run build    # 编译生产包直接供 FastAPI 挂载托管
# 或者启动 Vite 开发调试服务器
npm run dev      # 监听 http://localhost:5173，自动反向代理 /api 与 /ws 至 8000
```

---

## 3. Docker Compose 容器化部署

### 3.1 核心配置文件结构
```text
py_web/
├── compose.dev.yml       # 开发环境：FastAPI + Vite dev server + MySQL + Redis
├── compose.prod.yml      # 生产环境：hid-gateway 单镜像 (包含前端产物) + MySQL
├── Dockerfile            # 生产多阶段构建 Dockerfile
├── Dockerfile.backend    # 后端开发镜像定义
└── Dockerfile.frontend.dev # 前端 Vite 热更新镜像定义
```

### 3.2 开发环境编排 (`compose.dev.yml`)
适合前后端联调及需要 MySQL 真实数据的场景：
```bash
# 1. 启动全套开发容器 (自动挂载本地目录实现热重载)
docker compose -f compose.dev.yml up -d

# 2. 查看实时日志流
docker compose -f compose.dev.yml logs -f

# 3. 停止开发容器
docker compose -f compose.dev.yml down
```
- **前端开发入口**：`http://localhost:5173`
- **后端 Swagger 文档**：`http://localhost:8000/docs`
- **健康检查**：`http://localhost:8000/api/v1/health`

### 3.3 生产环境编排 (`compose.prod.yml`)
适合直接部署在 NAS、局域网服务器或云主机上的单机生产环境：
```bash
# 1. 重新构建并启动生产环境
docker compose -f compose.prod.yml up -d --build

# 2. 检查运行状态
docker compose -f compose.prod.yml ps

# 3. 停止并清理临时容器
docker compose -f compose.prod.yml down
```
- **生产统一入口**：`http://服务器IP:8000/`（FastAPI 自带静态托管 `vue-project/dist`）

---

## 4. 自动化运维脚本速查表

项目在 `scripts/` 目录下提供了跨平台的 Python（Windows 推荐）与 Shell（Linux/Mac 推荐）运维助手：

```bash
# 使用 Python 跨平台运维脚本 (Windows / Linux 通用)
python scripts/py/start.py --dev              # 启动开发编排并等待健康检查
python scripts/py/start.py --prod --build     # 重新编译镜像并启动生产编排
python scripts/py/stop.py --prod --rm         # 停止生产编排并清理孤立容器
python scripts/py/logs.py --prod --all        # 查看所有容器的日志
python scripts/py/clean.py --dev --all        # 清理未使用的构建缓存与临时镜像

# 单独启动本地后端 (支持指定端口与自动 kill 占位进程)
python scripts/py/start_backend.py --port 8008 --kill

# 构建前端生产包并拷贝至后端静态目录
python scripts/py/build_frontend.py
```

---

## 5. 数据库备份与恢复实操

### 5.1 数据库备份脚本 (`scripts/mysql/backup.sh`)
备份脚本会自动在项目根目录的 `backups/` 目录下生成带时间戳的 SQL 压缩包：
```bash
bash scripts/mysql/backup.sh
# 生成文件格式：backups/backup_20261026_100000.sql.gz
```

### 5.2 数据库恢复脚本 (`scripts/mysql/restore.sh`)
```bash
bash scripts/mysql/restore.sh backups/backup_20261026_100000.sql.gz
```

---

## 6. 常见故障诊断与排查清单 (Troubleshooting)

| 故障现象 | 潜在根因 | 排查与修复措施 |
|---|---|---|
| **后端报错 `IntegrityError: NOT NULL constraint failed: devices.id`** | 本地 SQLite 无法识别 `BIGINT AUTO_INCREMENT` | 检查 ORM 模型主键是否声明为 `BigInteger().with_variant(Integer, "sqlite")`。 |
| **鼠标移动时出现显著停顿卡顿** | 数据库同步写导致事件循环阻塞，或 TinyUSB 端点忙丢包 | 检查 `dispatcher.py` 是否已开启流式免落库；检查 `session.py` 是否已启用 SQLite WAL 模式。 |
| **macOS 连接蓝牙后反复转圈断连** | macOS 要求 HOGP 特征强制加密；设备未持久化 LTK | 确认固件是否启用了 `READ_ENC` / `WRITE_ENC` 特征加密；确认设备已在 NVS 挂载 `nimble_bond`。 |
| **固件推送后设备长时间处于 `upgrading`** | 设备网络无法下载固件 URL 或 SHA-256 不匹配 | 检查 `ota.status` 中的错误信息；确认 ESP32 能否访问服务端的二进制下载地址。 |
