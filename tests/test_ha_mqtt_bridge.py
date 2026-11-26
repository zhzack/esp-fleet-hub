"""Tests for Home Assistant MQTT Auto-Discovery bridge, /api/v2/ha/* routes & WOL."""

import asyncio
import json
import threading
import uuid

from fastapi.testclient import TestClient

from app.main import app
from app.services import unified_bridge as ub
from app.services.device_registry import device_registry
from app.services.ha_mqtt_bridge import (
    build_device_discovery_specs,
    generate_xiaoai_automations_yaml,
    ha_bridge,
    parse_cn_time_text,
    send_wol,
)
from tests.conftest import TestingSessionLocal, test_engine


def _auth_hdrs(client, username: str, role: str = "admin") -> dict[str, str]:
    client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": role},
    )
    tok = client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    ).json()["access_token"]
    return {"Authorization": f"Bearer {tok}"}


class _FakeMqttClient:
    def __init__(self):
        self.published: list[tuple[str, object, bool]] = []
        self.subscribed: list[str] = []

    def publish(self, topic, payload=None, qos=0, retain=False):
        self.published.append((topic, payload, retain))

    def subscribe(self, topic, qos=0):
        self.subscribed.append(topic)

    def disconnect(self):
        pass

    def loop_stop(self):
        pass


def test_parse_cn_time_text_and_wol(monkeypatch):
    assert parse_cn_time_text("取消定时关机") == 0
    assert parse_cn_time_text("别关电脑了") == 0
    assert parse_cn_time_text("半小时后关电脑") == 30
    assert parse_cn_time_text("2小时后关机") == 120
    assert parse_cn_time_text("十五分钟后关") == 15
    assert parse_cn_time_text("45") == 45
    assert parse_cn_time_text("随便说句话") is None

    sent_packets = []

    class _FakeSock:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def setsockopt(self, *_args):
            pass

        def sendto(self, data, addr):
            sent_packets.append((data, addr))

    monkeypatch.setattr("app.services.ha_mqtt_bridge.socket.socket", lambda *_a, **_kw: _FakeSock())
    assert send_wol("00:E2:69:9C:9A:35", "192.168.68.255") is True
    assert len(sent_packets) == 2
    assert sent_packets[0][0] == b"\xff" * 6 + bytes.fromhex("00E2699C9A35") * 16
    assert send_wol("invalid-mac") is False


def test_build_discovery_specs_capability_filtering():
    c3_specs = build_device_discovery_specs(
        device_uuid="esp32c3-72f320",
        device_name="Desk Monitor",
        chip="ESP32-C3",
        caps={"gpio", "display", "pc_monitor"},
        target={
            "enabled": True,
            "expose_hid": True,
            "expose_gpio": True,
            "expose_display": True,
            "expose_camera": True,
            "pc_enabled": False,
        },
        gpio_pins=[{"pin": 2, "label": "LED", "pwm": False}],
    )
    topics = [t for t, _ in c3_specs]
    assert any("/switch/esp32c3-72f320/gpio_2/config" in t for t in topics)
    assert any("/text/esp32c3-72f320/display_text/config" in t for t in topics)
    assert any("/select/esp32c3-72f320/display_mode/config" in t for t in topics)
    assert not any("/button/esp32c3-72f320/mute/config" in t for t in topics)
    assert not any("/camera/" in t for t in topics)

    s3_specs = build_device_discovery_specs(
        device_uuid="esp32s3-aa68ec",
        device_name="PC Ctrl",
        chip="ESP32-S3",
        caps={"usb_keyboard", "usb_mouse", "gpio"},
        target={
            "enabled": True,
            "expose_hid": True,
            "expose_gpio": True,
            "expose_display": False,
            "expose_camera": False,
            "pc_enabled": True,
            "pc_name": "台式电脑",
            "pc_mac": "00:E2:69:9C:9A:35",
            "pc_power_pin": 12,
        },
        macros=[{"id": 7, "name": "一键静音"}],
    )
    s3_topics = [t for t, _ in s3_specs]
    assert any("/switch/esp32s3-aa68ec/pc_power/config" in t for t in s3_topics)
    assert any("/button/esp32s3-aa68ec/wol/config" in t for t in s3_topics)
    assert any("/button/esp32s3-aa68ec/power_pulse/config" in t for t in s3_topics)
    assert any("/button/esp32s3-aa68ec/mute/config" in t for t in s3_topics)
    assert any("/number/esp32s3-aa68ec/shutdown_timer/config" in t for t in s3_topics)
    assert any("/button/esp32s3-aa68ec/macro_7/config" in t for t in s3_topics)

    disabled = build_device_discovery_specs(
        device_uuid="x",
        device_name="x",
        caps={"gpio"},
        target={"enabled": False},
    )
    assert disabled == []

    yml = generate_xiaoai_automations_yaml("esp32s3-aa68ec", {}, topic_prefix="esp_hub")
    assert "esp_hub/dev/esp32s3-aa68ec/timer/parse_text" in yml


def test_ha_api_and_mqtt_command_dispatch(client, db, monkeypatch):
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)
    monkeypatch.setattr("app.services.device_executor.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.services.ha_mqtt_bridge.SessionLocal", TestingSessionLocal)

    ub.init_unified_hub()
    fake_mqtt = _FakeMqttClient()
    ha_bridge._client = fake_mqtt
    ha_bridge._connected = True
    ha_bridge._shutdown_deadlines.clear()
    ha_bridge._gpio_cache.clear()
    received_ws = []

    monkeypatch.setattr(ha_bridge, "request_reload", lambda: None)
    monkeypatch.setattr(ha_bridge, "request_sync", lambda: None)

    try:
        with client as tc:
            adm_hdrs = _auth_hdrs(tc, "ha_adm", role="admin")
            usr_hdrs = _auth_hdrs(tc, "ha_usr", role="user")

            # Non-admin blocked from broker config mutation
            assert tc.put("/api/v2/ha/config", headers=usr_hdrs, json={"broker_host": "10.0.0.2"}).status_code == 403

            # Admin saves configurable MQTT broker settings
            cfg_res = tc.put(
                "/api/v2/ha/config",
                headers=adm_hdrs,
                json={
                    "enabled": True,
                    "broker_host": "192.168.50.8",
                    "broker_port": 1884,
                    "mqtt_user": "ha_bot",
                    "mqtt_pass": "mqtt_secret_123",
                    "discovery_prefix": "homeassistant",
                    "topic_prefix": "my_hub",
                    "sync_interval_sec": 8,
                },
            )
            assert cfg_res.status_code == 200
            cfg_body = cfg_res.json()["config"]
            assert cfg_body["broker_host"] == "192.168.50.8"
            assert cfg_body["has_mqtt_pass"] is True
            assert "mqtt_pass" not in cfg_body

            # Register a device in Hub & attach via WebSocket
            dev_id = "esp32s3_ha_e2e"
            ub.hub_db.register_or_update_device({
                "device_id": dev_id,
                "chip": "ESP32-S3",
                "ip": "",
                "capabilities": {"usb_otg": {"enabled": True}, "gpio": {"enabled": True}},
                "config": {"config_ver": 1},
            })

            with tc.websocket_connect("/ws/device") as ws:
                ws.send_json({
                    "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                    "routing": {},
                    "payload": {"device_id": dev_id, "name": dev_id, "capabilities": ["usb_keyboard", "usb_mouse", "gpio"]},
                })
                assert ws.receive_json()["payload"]["status"] == "ok"

                def _dev_loop():
                    while True:
                        try:
                            msg = ws.receive_json()
                        except Exception:
                            break
                        received_ws.append(msg)
                        mt = msg["header"]["type"]
                        mid = msg["header"]["msg_id"]
                        pl = msg.get("payload") or {}
                        if mt == "gpio.pins":
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {"ok": True, "pins": [{"pin": 12, "label": "Lamp", "pwm": True, "duty": 0}]},
                            })
                        elif mt == "gpio.set":
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {"ok": True, "pin": pl.get("pin"), "duty": pl.get("duty")},
                            })
                        elif mt == "action":
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {"ok": True, "status": "ok", "type": pl.get("type")},
                            })

                threading.Thread(target=_dev_loop, daemon=True).start()

                # Configure PC target + legacy pc_ctrl compatibility
                tgt_res = tc.put(
                    f"/api/v2/ha/devices/{dev_id}",
                    headers=adm_hdrs,
                    json={
                        "enabled": True,
                        "display_name": "Studio PC S3",
                        "area_name": "Studio",
                        "pc_enabled": True,
                        "pc_name": "Studio Desktop",
                        "pc_mac": "00:E2:69:9C:9A:35",
                        "pc_power_pin": 12,
                        "legacy_topic_prefix": "pc_ctrl",
                        "exposed_pins": [{"pin": 12, "name": "Studio Lamp", "pwm": True}],
                    },
                )
                assert tgt_res.status_code == 200
                assert tgt_res.json()["target"]["pc_enabled"] is True

                # Trigger discovery sync
                sync_res = tc.post("/api/v2/ha/sync", headers=adm_hdrs)
                assert sync_res.status_code == 200
                assert sync_res.json()["entities"] >= 10
                pub_topics = {t for t, _, _ in fake_mqtt.published}
                assert f"homeassistant/switch/{dev_id}/pc_power/config" in pub_topics
                assert f"homeassistant/light/{dev_id}/gpio_12/config" in pub_topics
                assert "pc_ctrl/#" in fake_mqtt.subscribed

                # Simulate incoming MQTT commands from HA
                asyncio.run(ha_bridge.handle_mqtt_message(f"my_hub/dev/{dev_id}/button/mute/set", "PRESS"))
                asyncio.run(ha_bridge.handle_mqtt_message(f"my_hub/dev/{dev_id}/gpio/12/brightness/set", "75"))
                asyncio.run(ha_bridge.handle_mqtt_message("pc_ctrl/timer/parse_text", "20分钟后关电脑"))
                assert dev_id in ha_bridge._shutdown_deadlines
                asyncio.run(ha_bridge.handle_mqtt_message("pc_ctrl/timer/parse_text", "取消关机"))
                assert dev_id not in ha_bridge._shutdown_deadlines

                # WOL endpoint & automation YAML check
                sent_wol = []
                monkeypatch.setattr("app.api.v2.ha.send_wol", lambda mac, bcast: sent_wol.append((mac, bcast)) or True)
                wol_res = tc.post(f"/api/v2/ha/devices/{dev_id}/wol", headers=adm_hdrs, json={"wake_key": True})
                assert wol_res.status_code == 200
                assert sent_wol == [("00:E2:69:9C:9A:35", "255.255.255.255")]

                yml_res = tc.get(f"/api/v2/ha/devices/{dev_id}/automations", headers=adm_hdrs)
                assert yml_res.status_code == 200
                assert f"my_hub/dev/{dev_id}/timer/parse_text" in yml_res.json()["yaml"]

                actions_seen = [f for f in received_ws if f["header"]["type"] == "action"]
                assert any(f["payload"].get("type") == "consumer" and f["payload"].get("key") == "KEY_MUTE" for f in actions_seen)
                assert any(f["payload"].get("type") == "keyboard_tap" and f["payload"].get("key") == "KEY_SPACE" for f in actions_seen)
                gpio_sets = [f for f in received_ws if f["header"]["type"] == "gpio.set"]
                assert any(f["payload"].get("pin") == 12 and f["payload"].get("duty") == 75 for f in gpio_sets)
    finally:
        ha_bridge._client = None
        ha_bridge._connected = False
        ha_bridge._shutdown_deadlines.clear()
        ha_bridge._gpio_cache.clear()
        device_registry._live.clear()
