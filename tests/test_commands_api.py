"""tests/test_commands_api.py - 设备控制指令 (/api/v2/devices/*) 与能力守卫全面回归测试"""

import threading
import uuid

import pytest
from fastapi.testclient import TestClient

from app.main import app
from app.services.device_registry import device_registry
from tests.conftest import TestingSessionLocal, test_engine


def _reg_login(client, username: str, role: str = "user") -> dict[str, str]:
    client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": role},
    )
    tok = client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    ).json()["access_token"]
    return {"Authorization": f"Bearer {tok}"}


def test_c3_without_ble_capability_rejected_immediately(client):
    """未上报 BLE 能力的设备（如 C3 监控节点）调 /ble/* 与 /transport 立即返回 400，绝不卡死超时。"""
    hdrs = _reg_login(client, "cmd_c3_user")
    client.post(
        "/api/v1/devices",
        headers=hdrs,
        json={
            "device_uuid": "esp32c3-no-ble",
            "name": "C3 Monitor",
            "capabilities": ["gpio", "display", "ota"],
        },
    )

    for method, path, body in (
        ("get", "/api/v2/devices/esp32c3-no-ble/ble/info", None),
        ("get", "/api/v2/devices/esp32c3-no-ble/ble/bonds", None),
        ("post", "/api/v2/devices/esp32c3-no-ble/ble/scan", {"duration_ms": 4000}),
        ("post", "/api/v2/devices/esp32c3-no-ble/ble/disconnect", None),
        ("post", "/api/v2/devices/esp32c3-no-ble/ble/unpair_all", None),
        ("post", "/api/v2/devices/esp32c3-no-ble/ble/unpair", {"addr_type": 0, "addr": [1, 2, 3, 4, 5, 6]}),
        ("post", "/api/v2/devices/esp32c3-no-ble/ble/switch", {"addr_type": 0, "addr": [1, 2, 3, 4, 5, 6]}),
        ("get", "/api/v2/devices/esp32c3-no-ble/transport", None),
        ("post", "/api/v2/devices/esp32c3-no-ble/transport", {"mode": "auto"}),
    ):
        kwargs = {"headers": hdrs}
        if body is not None:
            kwargs["json"] = body
        resp = getattr(client, method)(path, **kwargs)
        assert resp.status_code == 400, f"{method} {path} expected 400, got {resp.status_code}"
        assert "未启用" in resp.json()["detail"]

    # usb/info 对明确实现该接口的统一固件节点可正常走到离线/WS 派发流程（非能力拦截）
    usb_resp = client.get("/api/v2/devices/esp32c3-no-ble/usb/info", headers=hdrs)
    assert usb_resp.status_code == 200
    assert usb_resp.json()["error_code"] == "E002"


def test_commands_ble_wifi_transport_e2e_ws_and_known_wifi(monkeypatch):
    """覆盖 status/wifi/ble/usb/transport/reboot、wifi/known CRUD + apply 与 slots/send 全闭环。"""
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)

    try:
        with TestClient(app) as tc:
            u1_hdrs = _reg_login(tc, "cmd_owner")
            u2_hdrs = _reg_login(tc, "cmd_other")

            # 创建支持完整 USB+BLE HID 的模拟设备
            tc.post(
                "/api/v1/devices",
                headers=u1_hdrs,
                json={
                    "device_uuid": "esp32_hid_full",
                    "name": "Full HID",
                    "capabilities": ["usb_keyboard", "usb_mouse", "ble_keyboard", "ble_mouse"],
                },
            )

            with tc.websocket_connect("/ws/device") as ws:
                ws.send_json({
                    "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                    "payload": {
                        "device_id": "esp32_hid_full",
                        "name": "Full HID",
                        "capabilities": ["usb_keyboard", "usb_mouse", "ble_keyboard", "ble_mouse"],
                    },
                })
                assert ws.receive_json()["payload"]["status"] == "ok"

                current_peer = "AA:BB:CC:DD:EE:01"

                def _device_loop():
                    nonlocal current_peer
                    while True:
                        try:
                            msg = ws.receive_json()
                        except Exception:
                            break
                        mt = msg["header"]["type"]
                        mid = msg["header"]["msg_id"]
                        pl = msg.get("payload") or {}
                        res_pl = {"ok": True, "status": "ok", "exec_time_ms": 3}

                        if mt == "get_status":
                            res_pl.update({"ip": "192.168.68.88", "ssid": "LabWiFi", "heap_free": 180000})
                        elif mt == "wifi.status":
                            res_pl.update({"connected": True, "ssid": "LabWiFi", "ip": "192.168.68.88", "rssi": -42})
                        elif mt == "wifi.scan":
                            res_pl.update({"networks": [{"ssid": "LabWiFi", "rssi": -42, "auth": "WPA2"}]})
                        elif mt == "wifi.connect":
                            res_pl.update({"connected_ssid": pl.get("ssid"), "has_pass": bool(pl.get("pass"))})
                        elif mt == "ble.info":
                            res_pl.update({"connected": True, "peer": current_peer})
                        elif mt == "ble.list_bonds":
                            res_pl.update({"bonds": [{"addr_type": 0, "addr": [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01]}]})
                        elif mt == "ble.scan":
                            res_pl.update({"devices": [{"name": "MacBook", "addr": "AA:BB:CC:DD:EE:01", "rssi": -45}]})
                        elif mt == "ble.switch":
                            addr = ":".join(f"{b:02X}" for b in pl.get("addr", []))
                            current_peer = addr
                        elif mt in ("ble.unpair", "ble.unpair_all", "ble.disconnect", "reboot"):
                            pass
                        elif mt == "usb.info":
                            res_pl.update({"enabled": True, "mounted": True})
                        elif mt in ("transport.get", "transport.set"):
                            res_pl.update({"mode": pl.get("mode", "auto"), "active": "ble", "ble_connected": True})
                        elif mt == "action":
                            res_pl.update({"echo_action": pl.get("type")})

                        ws.send_json({
                            "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                            "routing": {},
                            "payload": res_pl,
                        })

                t = threading.Thread(target=_device_loop, daemon=True)
                t.start()

                # 1. 状态 & Wi-Fi 链路
                assert tc.get("/api/v2/devices/esp32_hid_full/status", headers=u1_hdrs).json()["result"]["ssid"] == "LabWiFi"
                assert tc.get("/api/v2/devices/esp32_hid_full/wifi/status", headers=u1_hdrs).json()["result"]["rssi"] == -42
                assert tc.post("/api/v2/devices/esp32_hid_full/wifi/scan", headers=u1_hdrs).json()["result"]["networks"][0]["ssid"] == "LabWiFi"

                wconn = tc.post(
                    "/api/v2/devices/esp32_hid_full/wifi/connect",
                    headers=u1_hdrs,
                    json={"ssid": "LabWiFi", "password": "wifi-secret-pw"},
                )
                assert wconn.status_code == 200
                assert wconn.json()["status"] == "acked"

                # 成功后自动记录到 wifi/known，密码脱敏仅返回 has_password=True，并支持 apply 回放
                known = tc.get("/api/v2/devices/esp32_hid_full/wifi/known", headers=u1_hdrs).json()
                assert len(known) == 1 and known[0]["ssid"] == "LabWiFi" and known[0]["has_password"] is True
                applied = tc.post(
                    "/api/v2/devices/esp32_hid_full/wifi/apply",
                    headers=u1_hdrs,
                    json={"ssid": "LabWiFi"},
                )
                assert applied.json()["status"] == "acked"
                assert applied.json()["result"]["has_pass"] is True

                assert tc.delete("/api/v2/devices/esp32_hid_full/wifi/known/LabWiFi", headers=u1_hdrs).status_code == 200

                # 2. BLE 全套控制链路 & 校验
                assert tc.get("/api/v2/devices/esp32_hid_full/ble/info", headers=u1_hdrs).json()["result"]["peer"] == "AA:BB:CC:DD:EE:01"
                assert len(tc.get("/api/v2/devices/esp32_hid_full/ble/bonds", headers=u1_hdrs).json()["result"]["bonds"]) == 1
                scan = tc.post("/api/v2/devices/esp32_hid_full/ble/scan", headers=u1_hdrs, json={"duration_ms": 2000})
                assert scan.json()["status"] == "acked"
                assert scan.json()["result"]["devices"][0]["name"] == "MacBook"

                assert tc.post("/api/v2/devices/esp32_hid_full/ble/switch", headers=u1_hdrs, json={"addr_type": 0, "addr": [17, 34, 51, 68, 85, 102]}).json()["status"] == "acked"
                assert tc.post("/api/v2/devices/esp32_hid_full/ble/unpair", headers=u1_hdrs, json={"addr_type": 0, "addr": [17, 34, 51, 68, 85, 102]}).json()["status"] == "acked"
                assert tc.post("/api/v2/devices/esp32_hid_full/ble/disconnect", headers=u1_hdrs).json()["status"] == "acked"
                assert tc.post("/api/v2/devices/esp32_hid_full/ble/unpair_all", headers=u1_hdrs).json()["status"] == "acked"

                # 非法参数校验
                assert tc.post("/api/v2/devices/esp32_hid_full/ble/unpair", headers=u1_hdrs, json={"addr_type": 0, "addr": [1, 2]}).status_code == 400
                assert tc.post("/api/v2/devices/esp32_hid_full/transport", headers=u1_hdrs, json={"mode": "invalid"}).status_code == 400

                # 3. USB / Transport / Reboot & 多目标 /send
                assert tc.get("/api/v2/devices/esp32_hid_full/usb/info", headers=u1_hdrs).json()["result"]["mounted"] is True
                assert tc.get("/api/v2/devices/esp32_hid_full/transport", headers=u1_hdrs).json()["result"]["mode"] == "auto"
                assert tc.post("/api/v2/devices/esp32_hid_full/transport", headers=u1_hdrs, json={"mode": "ble"}).json()["result"]["mode"] == "ble"
                assert tc.post("/api/v2/devices/esp32_hid_full/reboot", headers=u1_hdrs).json()["status"] == "acked"

                tc.put(
                    "/api/v2/devices/esp32_hid_full/slots/1",
                    headers=u1_hdrs,
                    json={"addr_type": 0, "addr": "AA:BB:CC:DD:EE:01", "name": "Host1"},
                )
                send_res = tc.post(
                    "/api/v2/devices/esp32_hid_full/send",
                    headers=u1_hdrs,
                    json={"slots": [1], "action": {"type": "keyboard_tap", "key": "KEY_A"}},
                )
                assert send_res.status_code == 200

                # 4. 其他用户越权访问被拒绝 (403)
                assert tc.post("/api/v2/devices/esp32_hid_full/ble/scan", headers=u2_hdrs).status_code == 403
                assert tc.get("/api/v2/devices/esp32_hid_full/wifi/known", headers=u2_hdrs).status_code == 403
    finally:
        device_registry._live.clear()
