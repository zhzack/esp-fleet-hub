"""tests/test_main_lifespan.py - main.py lifespan 启动逻辑测试

验证 on_event -> lifespan 迁移后的启动行为：
进入 TestClient 上下文时自动建表，应用可正常服务请求。
"""
import sqlalchemy as sa
from fastapi.testclient import TestClient

from app.database.base import Base
from app.main import app
from tests.conftest import test_engine


def test_lifespan_creates_tables_and_serves(monkeypatch):
    # 模拟空数据库：清掉 conftest autouse 夹具预先建的表
    Base.metadata.drop_all(bind=test_engine)
    assert sa.inspect(test_engine).get_table_names() == []

    # lifespan 函数体内 `from app.database.session import engine` 在执行时
    # 读取模块属性，monkeypatch 替换为测试引擎，避免触碰真实 app.db
    monkeypatch.setattr("app.database.session.engine", test_engine)

    # with 语法触发 lifespan（等价于旧 on_event startup）
    with TestClient(app) as c:
        tables = set(sa.inspect(test_engine).get_table_names())
        assert {"users", "devices", "macros", "device_groups", "device_bond_slots"} <= tables

        # 应用可用：health 端点正常响应
        res = c.get("/api/v1/health")
        assert res.status_code == 200
        assert res.json()["status"] == "ok"

    # 退出 with（shutdown）无异常即通过
