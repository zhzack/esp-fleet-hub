# Docker 使用指南

> 本项目用 docker compose 管理两个独立编排：**开发**（前后端分容器、热更新）、**生产**（单镜像 baked 前端）。
> ESP-IDF 固件构建另有一套独立 compose（见末节）。

---

## 目录

- [一、前置条件](#一前置条件)
- [二、开发环境（推荐）](#二开发环境推荐)
- [三、生产环境](#三生产环境)
- [四、常用运维命令](#四常用运维命令)
- [五、首次安装清单](#五首次安装清单)
- [六、故障排查](#六故障排查)
- [七、ESP-IDF 固件 Docker 编译](#七esp-idf-固件-docker-编译)

---

## 一、前置条件

### 软件

- Docker Desktop ≥ 4.20 / Docker Engine ≥ 24
- compose v2（`docker compose ...` 不带连字符）

### `.env` 文件（项目根目录）

```dotenv
TZ=Asia/Shanghai

# MySQL
MYSQL_ROOT_PASSWORD=RootStrongPassword
MYSQL_DB=blue_db

# JWT
JWT_SECRET=change-me-please
JWT_ALGORITHM=HS256
JWT_EXPIRE_MINUTES=1440

# 队列
QUEUE_BACKEND=memory      # 或 redis

# 日志 / 调试
DEBUG=true

# 跨域：dev 默认走 vite 代理可以为空；如果浏览器直接打 backend 8000 才需要
BACKEND_CORS_ORIGINS=http://localhost:5173,http://127.0.0.1:5173
```

`.env` 不要提交到 git（确认 `.gitignore` 已忽略）。

---

## 二、开发环境（推荐）

### 启动

```bash
docker compose -f compose.dev.yml up
```

启完三件事：

| 服务 | 端口 | 容器名 | 行为 |
|---|---|---|---|
| `frontend` | 5173 | py_web_frontend_dev | vite dev server，HMR；`/api` `/ws` 反代到 backend |
| `backend` | 8000 | py_web_backend_dev | uvicorn `--reload`，监听 `/app/app` 改动 |
| `mysql` | 3306 | py_web_mysql_dev | dev 独立 volume `mysql_data_dev` |
| `redis` | 6379 | py_web_redis_dev | dev 独立 volume `redis_data_dev` |

**入口：http://localhost:5173**。改前后端代码都在这一个浏览器 tab 里看到效果。

### 热更新行为

| 改的文件 | 谁负责 | 反应 |
|---|---|---|
| `app/**/*.py` | uvicorn `--reload-dir /app/app` | 后端进程重启（< 1s），WS 连接会断 → 设备会重连 |
| `vue-project/src/**` | vite HMR | 浏览器**不刷新**，组件状态保留 |
| `requirements.txt` | 需要 rebuild backend | `docker compose -f compose.dev.yml build backend` |
| `vue-project/package.json` | 需要 rebuild frontend | `docker compose -f compose.dev.yml build frontend` |

### 直接打后端（绕过 vite 代理）

少数情况想看 backend 单独行为或用 curl 测：直接打 `http://localhost:8000/api/...` 即可，CORS 已在 `.env` 放行 5173。

---

## 三、生产环境

### 启动

```bash
docker compose -f compose.prod.yml up -d --build
```

或使用脚本（等价）：

```bash
python scripts/py/start.py --prod --build
```

| 服务 | 端口 | 行为 |
|---|---|---|
| `app` | 8000 | uvicorn 不带 reload；FastAPI 同时挂 `/api/*`、`/ws/*` 和 `vue-project/dist` 静态 |
| `mysql` | 3306 | volume `mysql_data` |
| `redis` | 6379 | volume `redis_data` |

**入口：http://localhost:8000**。前端走 SPA fallback；浏览器直接刷 `/devices`、`/macros` 都返回 `index.html`。

### 镜像构建过程

`Dockerfile` 是多 stage：
1. **frontend stage**（node:20-alpine）：`npm ci` + `npm run build` → `/web/dist`
2. **runtime stage**（python:3.12-slim）：装 Python deps + 拷贝 app/ + 拷贝 stage 1 的 `dist` 到 `/app/vue-project/dist`

构建时间约 90s；之后改源码再 build 用层缓存大致 30s 内完成。

### 升级生产

```bash
git pull
docker compose -f compose.prod.yml up -d --build
```

DB schema 由后端 startup 时 `Base.metadata.create_all` 自动建（仅新增表，不删字段）。如果有破坏性 schema 变更，仍建议接 alembic。

---

## 四、常用运维命令

### 看日志

```bash
docker compose -f compose.dev.yml logs -f backend
docker compose -f compose.dev.yml logs -f frontend mysql        # 多服务
docker compose -f compose.dev.yml logs --tail 200 --since 5m backend
```

### 进容器

```bash
docker compose -f compose.dev.yml exec backend bash
docker compose -f compose.dev.yml exec mysql mysql -uroot -p
docker compose -f compose.dev.yml exec frontend sh
```

### 重启单 service

```bash
docker compose -f compose.dev.yml restart backend
```

### 干净重建（清缓存）

```bash
docker compose -f compose.dev.yml down
docker compose -f compose.dev.yml build --no-cache
docker compose -f compose.dev.yml up
```

### 重置数据库

```bash
docker compose -f compose.dev.yml down -v        # ← -v 把 volumes 也删了
docker compose -f compose.dev.yml up
```

dev 和 prod 用不同的 volume 名（`mysql_data_dev` vs `mysql_data`），互不影响。

### 停止 / 删除

```bash
docker compose -f compose.dev.yml stop           # 容器停但保留
docker compose -f compose.dev.yml down           # 删容器，保留 volume
docker compose -f compose.dev.yml down -v        # 删容器 + volume（数据全清）
```

---

## 五、首次安装清单

```bash
# 1. 写 .env（参考第一节）
cp .env.example .env       # 如果有模板
# 编辑 .env

# 2. dev 环境起来
docker compose -f compose.dev.yml up -d

# 3. 创建首个用户（暂用直连 SQL）
docker compose -f compose.dev.yml exec mysql mysql -uroot -p${MYSQL_ROOT_PASSWORD} ${MYSQL_DB}
# 或者通过 register 接口：
curl -X POST http://localhost:5173/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'

# 4. 测试登录
curl -X POST http://localhost:5173/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

---

## 六、故障排查

### 启动失败：mysql healthcheck 一直不过

```bash
docker compose -f compose.dev.yml logs mysql
```

常见原因：
- `MYSQL_ROOT_PASSWORD` 与 `.env` 不一致 → 删 volume 重建
- 端口 3306 被宿主占用 → 改 `compose.dev.yml` 的 `ports`

### 后端启动报 `password decrypt failed`

`.env` 的 `JWT_SECRET` 改过，已存的 WiFi 密码 Fernet 不能解密。从 `device_wifi_known` 删那条记录或重新输入密码。

### 浏览器访问 5173 显示空白

```bash
docker compose -f compose.dev.yml logs frontend --tail 50
```

如果看到 `Cannot find module ...`，说明 `node_modules` 缺包：

```bash
docker compose -f compose.dev.yml build frontend
docker compose -f compose.dev.yml up -d frontend
```

### Vite HMR 不触发

Windows 下 docker mount 文件系统通知偶尔丢。临时方案：

```bash
docker compose -f compose.dev.yml restart frontend
```

长期方案：vite.config.js 加 `server.watch.usePolling = true`（牺牲性能换可靠）。

### 后端 `--reload` 不触发

确认改的文件在 `app/` 下；`run.py` 等顶级文件不在 reload-dir 里。

### "其他 ESP 客户端连不上 WS"

dev 模式 backend 监听的是 8000；ESP32 固件配置的 `DEFAULT_SERVER_HOST` 必须是 PC 在局域网的 IP，端口 8000。如果走 prod 模式则单镜像也是 8000，无差别。

### 容器之间 ping 不通

```bash
docker network ls
docker compose -f compose.dev.yml exec frontend ping -c 2 backend
```

确认 compose 自动创建的 default network 工作正常。如果断网，`docker compose down && up`。

---

## 七、ESP-IDF 固件 Docker 编译

`ble_hid/` 子项目有**独立的 docker-compose.yml**（基于 espressif/idf 镜像），与服务端 compose 完全无关。

### 编译

```bash
cd ble_hid
docker compose run --rm esp-build
```

### 配置（menuconfig）

```bash
cd ble_hid
docker compose run --rm menuconfig
```

### 烧录（建议在 Windows 主机用 flash.py，不走容器）

```bash
cd ble_hid
python flash.py            # 自动找口、烧、进 monitor
python flash.py --erase    # 烧前清 NVS（默认值才会生效）
```

详见 [api_reference.md §15.8](api_reference.md#158-编译时配置固件-kconfigprojbuild)。

### 清重建

```bash
cd ble_hid
docker compose run --rm esp-build sh -c "rm -rf build && idf.py build"

# 配置变更（sdkconfig.defaults）后强制重生成
docker compose run --rm esp-build sh -c "rm -f sdkconfig && rm -rf build && idf.py build"
```

---

## 维护检查清单

每次改下面任一处，必须同步更新本文档：

- [ ] `compose.dev.yml` / `compose.prod.yml` 服务定义变化
- [ ] `Dockerfile` / `Dockerfile.backend` / `Dockerfile.frontend.dev` 阶段或入口变化
- [ ] `.env` 必填项新增 / 变化
- [ ] `vue-project/vite.config.js` 端口或 proxy 变化
