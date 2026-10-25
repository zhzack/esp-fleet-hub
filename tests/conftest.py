"""tests/conftest.py - 共享测试环境配置与数据库夹具"""
import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker
from sqlalchemy.pool import StaticPool

from app.database.base import Base
from app.database.session import get_db
from app.main import app

# 统一的 SQLite 内存测试数据库引擎（使用 StaticPool 保持全进程连接共享）
TEST_DB_URL = "sqlite:///:memory:"

test_engine = create_engine(
    TEST_DB_URL,
    connect_args={"check_same_thread": False},
    poolclass=StaticPool,
)
TestingSessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=test_engine)


def override_get_db():
    db = TestingSessionLocal()
    try:
        yield db
    finally:
        db.close()


# 全局注入 FastAPI 依赖替换
app.dependency_overrides[get_db] = override_get_db


@pytest.fixture(autouse=True)
def db_session():
    """在每个测试用例前统一建表，用例结束后重置"""
    Base.metadata.create_all(bind=test_engine)
    yield
    Base.metadata.drop_all(bind=test_engine)


@pytest.fixture(autouse=True)
def _device_auth_off(monkeypatch):
    """设备 WS 鉴权默认关闭（本地 .env 可能配置了 DEVICE_SHARED_SECRET，
    测试默认保持旧行为；鉴权专项测试自行开启）。"""
    from app.core.config.config import settings
    monkeypatch.setattr(settings, "DEVICE_SHARED_SECRET", "")


@pytest.fixture
def client():
    """FastAPI TestClient 夹具"""
    return TestClient(app)


@pytest.fixture
def db():
    """独立数据库会话夹具"""
    session = TestingSessionLocal()
    try:
        yield session
    finally:
        session.close()

