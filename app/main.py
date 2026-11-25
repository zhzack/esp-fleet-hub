import os
import sys
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from app.api.fleet_compat import router as fleet_compat_router
from app.api.router import api_router
from app.api.ws import ws_router
from app.core.config.config import settings
from app.core.logger.logger import logger
from app.services import unified_bridge

@asynccontextmanager
async def lifespan(app: FastAPI):
    # 兜底创建新增的 ORM 表（已有的旧表保留不动）。
    # 生产建议改 alembic；开发期 create_all 足够。
    from app.database.base import Base  # noqa: F401 — 拉齐所有 model
    from app.database.session import engine
    Base.metadata.create_all(bind=engine)
    if "pytest" not in sys.modules:
        unified_port = int(os.environ.get("PORT", "8000") or 8000)
        unified_bridge.start_mdns_hub(port=unified_port)
    logger.info(
        f"🚀 {settings.PROJECT_NAME} v{settings.VERSION} 启动"
        f" | DB={settings.MYSQL_DB} | Queue={settings.QUEUE_BACKEND}"
    )
    yield
    if "pytest" not in sys.modules:
        unified_bridge.stop_mdns_hub()


app = FastAPI(
    title=settings.PROJECT_NAME,
    version=settings.VERSION,
    debug=settings.DEBUG,
    lifespan=lifespan,
)

# CORS：显式白名单优先，同时在 debug 下放行常见局域网开发源
default_cors_origins = [
    "http://localhost",
    "http://127.0.0.1",
    "http://localhost:5173",
    "http://127.0.0.1:5173",
    "http://localhost:8000",
    "http://127.0.0.1:8000",
]
configured_cors_origins = settings.BACKEND_CORS_ORIGINS or []
if isinstance(configured_cors_origins, str):
    configured_cors_origins = [configured_cors_origins]
cors_origins = list(dict.fromkeys([*default_cors_origins, *configured_cors_origins]))

cors_origin_regex = settings.BACKEND_CORS_ORIGIN_REGEX.strip() or None
if cors_origin_regex is None and settings.DEBUG:
    cors_origin_regex = (
        r"^https?://("
        r"localhost|"
        r"127\.0\.0\.1|"
        r"10(?:\.\d{1,3}){3}|"
        r"192\.168(?:\.\d{1,3}){2}|"
        r"172\.(?:1[6-9]|2\d|3[0-1])(?:\.\d{1,3}){2}"
        r")(?::\d+)?$"
    )

app.add_middleware(
    CORSMiddleware,
    allow_origins=cors_origins,
    allow_origin_regex=cors_origin_regex,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# REST API
app.include_router(api_router, prefix="/api")

# 统一 IoT 设备协议与 OTA 下载兼容端点 (/api/fleet/*)
app.include_router(fleet_compat_router, prefix="/api/fleet", tags=["fleet-compat"])

# WebSocket（与 REST 同根，但不挂 /api 前缀，符合设计文档）
app.include_router(ws_router)

# Vue 构建产物挂载
BASE_DIR = Path(__file__).resolve().parent.parent
FRONTEND_DIST = BASE_DIR / "vue-project" / "dist"
if FRONTEND_DIST.exists():
    logger.info(f"挂载前端资源目录：{FRONTEND_DIST}")
    # 仅挂载 assets/ 等静态资源；index.html 由 SPA fallback 处理
    app.mount(
        "/assets",
        StaticFiles(directory=FRONTEND_DIST / "assets"),
        name="vue-assets",
    )

    @app.get("/favicon.ico", include_in_schema=False)
    async def favicon():
        return FileResponse(FRONTEND_DIST / "favicon.ico")

    # SPA fallback：所有非 /api、/ws 路径都返回 index.html
    @app.get("/{full_path:path}", include_in_schema=False)
    async def spa_fallback(full_path: str, request: Request):
        # 已被前面的路由匹配的不会进这里；只兜底前端 history 路径
        return FileResponse(FRONTEND_DIST / "index.html")
else:
    logger.warning(f"前端构建目录不存在，跳过挂载：{FRONTEND_DIST}")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("app.main:app", host="0.0.0.0", port=8000, reload=True)
