"""tests/test_ws_device_auth.py - 设备 WS 注册 HMAC 挑战-应答鉴权测试"""
import hashlib
import hmac
import uuid

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.main import app
from app.services.device_registry import device_registry
from tests.conftest import TestingSessionLocal, test_engine

SECRET = "test-device-secret"


def _env(msg_type: str, payload: dict) -> dict:
    return {
        "header": {"msg_id": str(uuid.uuid4()), "type": msg_type, "timestamp": 0},
        "routing": {},
        "payload": payload,
    }


def _reg(device_id: str, **payload_extra) -> dict:
    return _env("register", {
        "device_id": device_id,
        "name": device_id,
        "capabilities": [],
        **payload_extra,
    })


def _proof(nonce: str) -> str:
    return hmac.new(SECRET.encode(), nonce.encode(), hashlib.sha256).hexdigest()


@pytest.fixture
def ws_client(monkeypatch):
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)
    with TestClient(app) as c:
        yield c


@pytest.fixture(autouse=True)
def _clean_registry():
    yield
    device_registry._live.clear()


def test_challenge_then_valid_proof_registers(ws_client, monkeypatch):
    from app.core.config.config import settings as _settings
    monkeypatch.setattr(_settings, 'DEVICE_SHARED_SECRET', SECRET)
    with ws_client.websocket_connect("/ws/device") as ws:
        ws.send_json(_reg("auth_dev_001"))
        challenge = ws.receive_json()
        assert challenge["header"]["type"] == "auth_challenge"
        nonce = challenge["payload"]["nonce"]
        assert nonce

        ws.send_json(_reg("auth_dev_001", auth={"nonce": nonce, "proof": _proof(nonce)}))
        ack = ws.receive_json()
        assert ack["header"]["type"] == "ack"
        assert ack["payload"]["status"] == "ok"
        assert device_registry.is_online("auth_dev_001")


def test_wrong_proof_rejected_and_closed(ws_client, monkeypatch):
    from app.core.config.config import settings as _settings
    monkeypatch.setattr(_settings, 'DEVICE_SHARED_SECRET', SECRET)
    with ws_client.websocket_connect("/ws/device") as ws:
        ws.send_json(_reg("auth_dev_002"))
        nonce = ws.receive_json()["payload"]["nonce"]

        ws.send_json(_reg("auth_dev_002", auth={"nonce": nonce, "proof": "0" * 64}))
        err = ws.receive_json()
        assert err["header"]["type"] == "error"
        assert err["payload"]["code"] == "E401"
        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
        assert not device_registry.is_online("auth_dev_002")


def test_replayed_stale_nonce_rejected(ws_client, monkeypatch):
    """旧 nonce 的合法 HMAC（重放）须被拒绝——每连接 nonce 一次性。"""
    from app.core.config.config import settings as _settings
    monkeypatch.setattr(_settings, 'DEVICE_SHARED_SECRET', SECRET)
    with ws_client.websocket_connect("/ws/device") as ws:
        ws.send_json(_reg("auth_dev_003"))
        stale = ws.receive_json()["payload"]["nonce"]

        # 丢弃第一个挑战，再触发一个新的
        ws.send_json(_reg("auth_dev_003"))
        fresh = ws.receive_json()["payload"]["nonce"]
        assert fresh != stale

        ws.send_json(_reg("auth_dev_003", auth={"nonce": stale, "proof": _proof(stale)}))
        err = ws.receive_json()
        assert err["header"]["type"] == "error"
        assert err["payload"]["code"] == "E401"
        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
        assert not device_registry.is_online("auth_dev_003")


def test_auth_disabled_when_secret_empty(ws_client):
    """密钥为空时保持旧行为：无挑战直接注册成功。"""
    with ws_client.websocket_connect("/ws/device") as ws:
        ws.send_json(_reg("auth_dev_004"))
        ack = ws.receive_json()
        assert ack["header"]["type"] == "ack"
        assert device_registry.is_online("auth_dev_004")
