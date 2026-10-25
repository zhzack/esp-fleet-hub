"""tests/test_ws_device.py - ESP32 设备 WebSocket 协议与派发链路测试

覆盖 /ws/device 的核心协议：register（V2 envelope + V1 legacy 兼容）、
幂等去重、heartbeat/status 状态上报、重连踢旧连接、
action 下发-ACK 闭环（E2E）与离线设备 E002 快速失败。
"""
import threading
import time
import uuid

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.database.crud import device as crud_device
from app.main import app
from app.services.device_registry import device_registry
from tests.conftest import TestingSessionLocal, test_engine


def _env(msg_type: str, payload: dict, msg_id: str | None = None, **header_extra) -> dict:
    return {
        "header": {
            "msg_id": msg_id or str(uuid.uuid4()),
            "type": msg_type,
            "timestamp": 0,
            **header_extra,
        },
        "routing": {},
        "payload": payload,
    }


def _register_env(device_id: str, msg_id: str | None = None, **payload_extra) -> dict:
    return _env(
        "register",
        {
            "device_id": device_id,
            "name": f"sim-{device_id}",
            "capabilities": ["usb_keyboard", "ble_keyboard"],
            **payload_extra,
        },
        msg_id=msg_id,
    )


@pytest.fixture
def ws_client(monkeypatch):
    """共享单 portal 的 TestClient：WS 会话与 HTTP 请求必须跑在同一个事件循环上，
    否则 dispatcher 的 ack_tracker Future 会跨循环。"""
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    # lifespan 建表也打到测试引擎
    monkeypatch.setattr("app.database.session.engine", test_engine)
    with TestClient(app) as c:
        yield c


@pytest.fixture(autouse=True)
def _clean_registry():
    yield
    device_registry._live.clear()


def _register_and_login(ws_client, username):
    ws_client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": "user"},
    )
    res = ws_client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    )
    return {"Authorization": f"Bearer {res.json()['access_token']}"}


class TestRegister:
    def test_register_ack_and_persistence(self, ws_client, db):
        msg_id = str(uuid.uuid4())
        with ws_client.websocket_connect("/ws/device") as ws:
            ws.send_json(_register_env("esp32_ws_001", msg_id=msg_id))
            ack = ws.receive_json()

            # ack 信封结构
            assert ack["header"]["type"] == "ack"
            assert ack["header"]["ref_msg_id"] == msg_id
            assert ack["payload"]["status"] == "ok"
            assert ack["payload"]["server_session"]

            # 连接存活期间：设备 upsert 为 online，字段落库
            dev = crud_device.get_by_uuid(db, "esp32_ws_001")
            assert dev is not None
            assert dev.status == "online"
            assert dev.name == "sim-esp32_ws_001"
            assert dev.capabilities == ["usb_keyboard", "ble_keyboard"]
            assert dev.last_online_at is not None
            # 连接记录尚未回填断开时间
            conns = (
                db.query(crud_device.DeviceConnection)
                .filter_by(device_id=dev.id)
                .all()
            )
            assert len(conns) == 1
            assert conns[0].disconnected_at is None

        # 断开后：设备标记 offline，连接记录回填 disconnected_at
        db.expire_all()
        dev = crud_device.get_by_uuid(db, "esp32_ws_001")
        assert dev.status == "offline"
        conns = (
            db.query(crud_device.DeviceConnection).filter_by(device_id=dev.id).all()
        )
        assert conns[0].disconnected_at is not None

    def test_register_missing_device_id_rejected(self, ws_client):
        with ws_client.websocket_connect("/ws/device") as ws:
            ws.send_json(_env("register", {"capabilities": ["usb_keyboard"]}))
            err = ws.receive_json()
            assert err["header"]["type"] == "error"
            assert err["payload"]["code"] == "E004"
            # 服务端以 1008（策略违规）关闭
            with pytest.raises(WebSocketDisconnect) as exc:
                ws.receive_json()
            assert exc.value.code == 1008

    def test_legacy_v1_frame_compatibility(self, ws_client, db):
        """旧 V1 帧 {type, payload} 应被 _wrap_legacy 包成 envelope 处理。"""
        with ws_client.websocket_connect("/ws/device") as ws:
            ws.send_json(
                {
                    "type": "register",
                    "payload": {"device_id": "esp32_legacy_001", "capabilities": ["ble_mouse"]},
                }
            )
            ack = ws.receive_json()

            assert ack["header"]["type"] == "ack"
            assert ack["payload"]["status"] == "ok"
            dev = crud_device.get_by_uuid(db, "esp32_legacy_001")
            assert dev.status == "online"
            assert dev.capabilities == ["ble_mouse"]

    def test_duplicate_msg_id_ignored(self, ws_client):
        """相同 msg_id 的重复上行被幂等去重：中间那条不产生任何回复。"""
        dup_id = str(uuid.uuid4())
        fresh_id = str(uuid.uuid4())
        with ws_client.websocket_connect("/ws/device") as ws:
            ws.send_json(_register_env("esp32_dup_001", msg_id=dup_id))
            ack1 = ws.receive_json()
            assert ack1["header"]["ref_msg_id"] == dup_id

            # 重复 msg_id（被忽略，不回 ack），紧跟一条新 msg_id
            ws.send_json(_register_env("esp32_dup_001", msg_id=dup_id))
            ws.send_json(_register_env("esp32_dup_001", msg_id=fresh_id))
            ack2 = ws.receive_json()
            # 若去重失效，这里会先收到重复 register 的 ack（ref=dup_id）
            assert ack2["header"]["ref_msg_id"] == fresh_id


class TestStateReporting:
    def test_heartbeat_and_status_updates(self, ws_client, db):
        with ws_client.websocket_connect("/ws/device") as ws:
            ws.send_json(_register_env("esp32_hb_001"))
            ws.receive_json()  # register ack

            ws.send_json(_env("heartbeat", {}))
            ws.send_json(_env("status", {"state": "busy"}))
            # 第三帧 heartbeat 验证新语义：心跳不得把设备上报的 busy 踩回 online
            ws.send_json(_env("heartbeat", {}))

            # 三帧均无回复，轮询等待服务端处理完成（消除帧未处理完就断言的竞态）
            dev = None
            deadline = time.time() + 5
            while time.time() < deadline:
                db.expire_all()
                dev = crud_device.get_by_uuid(db, "esp32_hb_001")
                if dev is not None and dev.status == "busy":
                    break
                time.sleep(0.05)
            # 连接存活期间断言（断开后 status 会被 mark_offline 覆盖）
            assert dev is not None and dev.status == "busy"
            assert dev.last_online_at is not None


class TestDispatch:
    def test_action_dispatch_and_ack_e2e(self, ws_client, db):
        """核心链路：WS 注册 → POST /api/v2/action → 设备收 envelope → 回 ack → HTTP 返回 acked。"""
        auth = _register_and_login(ws_client, "ws_dispatch_user")
        got_envelope = threading.Event()

        def device_respond(ws):
            env = ws.receive_json()  # 阻塞等 action envelope
            got_envelope.set()
            ws.send_json(
                {
                    "header": {
                        "msg_id": str(uuid.uuid4()),
                        "ref_msg_id": env["header"]["msg_id"],
                        "type": "ack",
                    },
                    "payload": {"status": "ok", "exec_time_ms": 7},
                }
            )

        with ws_client.websocket_connect("/ws/device") as ws:
            ws.send_json(_register_env("esp32_disp_001"))
            ws.receive_json()  # register ack

            responder = threading.Thread(target=device_respond, args=(ws,))
            responder.start()
            try:
                res = ws_client.post(
                    "/api/v2/action",
                    headers=auth,
                    json={
                        "device_id": "esp32_disp_001",
                        "payload": {
                            "type": "keyboard_tap",
                            "key": "KEY_A",
                            "need_ack": True,
                        },
                        "timeout_ms": 3000,
                    },
                )
            finally:
                responder.join(timeout=5)

        assert res.status_code == 200
        body = res.json()
        assert body["dispatched_to"] == ["esp32_disp_001"]
        result = body["results"][0]
        assert result["status"] == "acked"
        assert result["exec_time_ms"] == 7
        assert got_envelope.is_set()

        # action_log 落库为 acked
        logs = ws_client.get("/api/v2/logs?limit=10", headers=auth).json()
        tap_logs = [l for l in logs if l["msg_id"] == result["msg_id"]]
        assert len(tap_logs) == 1
        assert tap_logs[0]["status"] == "acked"
        assert tap_logs[0]["exec_time_ms"] == 7

    def test_offline_device_fails_fast_with_e002(self, ws_client):
        """设备存在于 DB 但不在线：无需等超时，立即 E002 failed。"""
        auth = _register_and_login(ws_client, "ws_offline_user")
        created = ws_client.post(
            "/api/v1/devices",
            headers=auth,
            json={"device_uuid": "esp32_ghost_001", "name": "Ghost"},
        )
        assert created.status_code == 201

        res = ws_client.post(
            "/api/v2/action",
            headers=auth,
            json={
                "device_id": "esp32_ghost_001",
                "payload": {"type": "keyboard_tap", "key": "KEY_A", "need_ack": True},
            },
        )
        assert res.status_code == 200
        result = res.json()["results"][0]
        assert result["status"] == "failed"
        assert result["error_code"] == "E002"
        assert result["error_message"] == "device offline"


class TestReconnect:
    def test_reconnect_kicks_stale_session(self, ws_client, db):
        """同设备重连：旧连接被 1012 关闭，registry 指向新连接。"""
        with ws_client.websocket_connect("/ws/device") as ws1:
            ws1.send_json(_register_env("esp32_recon_001"))
            ws1.receive_json()

            with ws_client.websocket_connect("/ws/device") as ws2:
                ws2.send_json(_register_env("esp32_recon_001"))
                ws2.receive_json()

                # 新连接是 registry 中的当前连接
                assert device_registry.get("esp32_recon_001") is not None
                assert device_registry.is_online("esp32_recon_001")

            # 旧连接被服务端主动关闭（1012 = 服务端要求重连）
            with pytest.raises(WebSocketDisconnect) as exc:
                ws1.receive_json()
            assert exc.value.code == 1012
