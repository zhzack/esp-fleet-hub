"""tests/test_timers_and_gpio_api.py - GPIO控制、自动复原脉冲、ESP8266 HTTP回退与设备定时器全闭环测试"""

from __future__ import annotations

import asyncio
from datetime import datetime
import json
import threading
import time
import uuid

from fastapi.testclient import TestClient
import pytest

from app.main import app
from app.services import device_executor
from app.services.timer_scheduler import compute_next_run, run_due_timers
from tests.conftest import TestingSessionLocal, test_engine


def _reg_login(client: TestClient, username: str, role: str = "user") -> dict[str, str]:
    client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": role},
    )
    tok = client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    ).json()["access_token"]
    return {"Authorization": f"Bearer {tok}"}


def test_compute_next_run_all_schedule_types():
    base_dt = datetime(2026, 11, 25, 10, 0, 0)  # Wednesday (weekday() == 2)
    base_ts = int(base_dt.timestamp())

    assert compute_next_run("countdown", now_ts=base_ts, delay_sec=45) == base_ts + 45
    assert compute_next_run("interval", now_ts=base_ts, interval_sec=60) == base_ts + 60
    assert compute_next_run("once", now_ts=base_ts, run_at=base_ts + 300) == base_ts + 300

    # Today later -> same day 18:30
    later_ts = compute_next_run("daily", now_ts=base_ts, time_hhmm="18:30")
    assert datetime.fromtimestamp(later_ts) == datetime(2026, 11, 25, 18, 30, 0)

    # Earlier than now -> tomorrow 08:15
    next_day_ts = compute_next_run("daily", now_ts=base_ts, time_hhmm="08:15")
    assert datetime.fromtimestamp(next_day_ts) == datetime(2026, 11, 26, 8, 15, 0)

    # Weekday filter: Friday only (weekday() == 4)
    fri_ts = compute_next_run("daily", now_ts=base_ts, time_hhmm="09:00", weekdays=[4])
    assert datetime.fromtimestamp(fri_ts).weekday() == 4
    assert datetime.fromtimestamp(fri_ts) == datetime(2026, 11, 27, 9, 0, 0)

    for bad_kwargs in (
        {"schedule_type": "countdown", "delay_sec": 0},
        {"schedule_type": "interval", "interval_sec": 2},
        {"schedule_type": "daily", "time_hhmm": "25:99"},
        {"schedule_type": "once", "run_at": None},
    ):
        with pytest.raises(ValueError):
            compute_next_run(**bad_kwargs)


def test_gpio_capability_guard_and_validation(client: TestClient):
    hdrs = _reg_login(client, "gpio_guard_u")
    client.post(
        "/api/v1/devices",
        headers=hdrs,
        json={
            "device_uuid": "esp32-display-only",
            "name": "Screen Only",
            "capabilities": ["display", "ota"],
        },
    )
    assert client.get("/api/v2/devices/esp32-display-only/gpio/pins", headers=hdrs).status_code == 400
    assert client.post(
        "/api/v2/devices/esp32-display-only/gpio/set",
        headers=hdrs,
        json={"pin": 12, "op": "on"},
    ).status_code == 400
    assert client.post(
        "/api/v2/timers",
        headers=hdrs,
        json={
            "device_uuid": "esp32-display-only",
            "name": "bad gpio timer",
            "schedule_type": "countdown",
            "delay_sec": 10,
            "action_type": "gpio_set",
            "action_payload": {"pin": 12, "op": "on"},
        },
    ).status_code == 400

    # Validation bounds on a GPIO-capable device
    client.post(
        "/api/v1/devices",
        headers=hdrs,
        json={"device_uuid": "esp32-gpio-bounds", "name": "GPIO Node", "capabilities": ["gpio"]},
    )
    assert client.post(
        "/api/v2/devices/esp32-gpio-bounds/gpio/set",
        headers=hdrs,
        json={"pin": 99, "op": "on"},
    ).status_code == 400
    assert client.post(
        "/api/v2/devices/esp32-gpio-bounds/gpio/set",
        headers=hdrs,
        json={"pin": 12, "op": "pwm", "duty": 150},
    ).status_code == 400
    assert client.post(
        "/api/v2/devices/esp32-gpio-bounds/gpio/set",
        headers=hdrs,
        json={"pin": 12, "op": "on", "duration_sec": 100000},
    ).status_code == 400


def test_gpio_ws_digital_pwm_toggle_and_pulse_restore(monkeypatch):
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.services.device_executor.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)

    received_cmds: list[tuple[str, dict]] = []
    pin_states = {12: 0, 4: 0}

    with TestClient(app) as tc:
        hdrs = _reg_login(tc, "gpio_ws_owner")
        tc.post(
            "/api/v1/devices",
            headers=hdrs,
            json={
                "device_uuid": "esp32c3-gpio-e2e",
                "name": "C3 GPIO Node",
                "capabilities": ["gpio", "display"],
            },
        )

        with tc.websocket_connect("/ws/device") as ws:
            ws.send_json({
                "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                "payload": {
                    "device_id": "esp32c3-gpio-e2e",
                    "name": "C3 GPIO Node",
                    "capabilities": ["gpio", "display"],
                },
            })
            assert ws.receive_json()["payload"]["status"] == "ok"

            stop_evt = threading.Event()

            def _responder():
                while not stop_evt.is_set():
                    try:
                        frame = ws.receive_json()
                    except Exception:
                        break
                    hdr = frame.get("header", {})
                    cmd = hdr.get("type")
                    mid = hdr.get("msg_id")
                    args = frame.get("payload") or {}
                    received_cmds.append((cmd, dict(args)))
                    res_pl = {"ok": True, "status": "ok", "exec_time_ms": 2}
                    if cmd == "gpio.pins":
                        res_pl.update({
                            "board": "ESP32-C3 Test",
                            "pins": [
                                {"pin": 12, "label": "LED_D4", "output": True, "pwm": False, "val": pin_states[12], "duty": 0},
                                {"pin": 4, "label": "PWM_4", "output": True, "pwm": True, "val": 0, "duty": pin_states[4]},
                            ],
                        })
                    elif cmd == "gpio.set":
                        p = int(args["pin"])
                        if "duty" in args:
                            pin_states[p] = int(args["duty"])
                        else:
                            pin_states[p] = int(args.get("val", 0))
                        res_pl.update({"pin": p, **args})
                    ws.send_json({
                        "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                        "routing": {},
                        "payload": res_pl,
                    })

            t = threading.Thread(target=_responder, daemon=True)
            t.start()
            try:
                # 1. GET gpio/pins
                pins_r = tc.get("/api/v2/devices/esp32c3-gpio-e2e/gpio/pins", headers=hdrs)
                assert pins_r.status_code == 200
                assert pins_r.json()["status"] == "acked"
                assert len(pins_r.json()["result"]["pins"]) == 2

                # 2. Digital ON
                on_r = tc.post(
                    "/api/v2/devices/esp32c3-gpio-e2e/gpio/set",
                    headers=hdrs,
                    json={"pin": 12, "op": "on"},
                )
                assert on_r.status_code == 200
                assert on_r.json()["status"] == "acked"
                assert pin_states[12] == 1

                # 3. Toggle (must query pins, see val==1, and send val==0)
                tog_r = tc.post(
                    "/api/v2/devices/esp32c3-gpio-e2e/gpio/set",
                    headers=hdrs,
                    json={"pin": 12, "op": "toggle"},
                )
                assert tog_r.status_code == 200
                assert pin_states[12] == 0

                # 4. PWM set
                pwm_r = tc.post(
                    "/api/v2/devices/esp32c3-gpio-e2e/gpio/set",
                    headers=hdrs,
                    json={"pin": 4, "op": "pwm", "duty": 65},
                )
                assert pwm_r.status_code == 200
                assert pin_states[4] == 65

                # 5. Pulse ON for 1s -> auto-restore to 0
                pulse_r = tc.post(
                    "/api/v2/devices/esp32c3-gpio-e2e/gpio/set",
                    headers=hdrs,
                    json={"pin": 12, "op": "on", "duration_sec": 1},
                )
                assert pulse_r.status_code == 200
                assert pulse_r.json()["pulse_restore_sec"] == 1
                assert pin_states[12] == 1

                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline and pin_states[12] != 0:
                    time.sleep(0.1)
                assert pin_states[12] == 0
            finally:
                device_executor.cancel_all_pulses()
                stop_evt.set()
                t.join(timeout=1)

    assert any(c == "gpio.set" and a == {"pin": 4, "duty": 65} for c, a in received_cmds)


def test_esp8266_http_fallback_pwm_scaling(client: TestClient, monkeypatch):
    hdrs = _reg_login(client, "esp8266_user")
    client.post(
        "/api/v1/devices",
        headers=hdrs,
        json={"device_uuid": "esp8266-sw01", "name": "8266 Switch", "capabilities": ["gpio"]},
    )

    hub_row = {
        "device_id": "esp8266-sw01",
        "chip": "ESP8266",
        "ip": "192.168.68.250",
        "port": 80,
    }
    captured_http: list[tuple[str, str, dict | None]] = []

    monkeypatch.setattr("app.services.unified_bridge.hub_db.ensure_init", lambda: None)
    monkeypatch.setattr("app.services.unified_bridge.hub_db.get_device", lambda dev_id: hub_row if dev_id == "esp8266-sw01" else None)

    def _fake_http(dev, path, method="GET", body=None, timeout=4):
        captured_http.append((method, path, body))
        if path == "/api/gpio/pins":
            payload = {"ok": True, "pins": [{"pin": 4, "label": "D2", "output": True, "pwm": True, "val": 1, "duty": 128}]}
        elif path == "/api/gpio/set":
            payload = {"ok": True, "pin": body["pin"], "duty": body.get("duty", 0)}
        else:
            payload = {"ok": True}
        return True, 200, (json.dumps(payload).encode("utf-8"), "application/json")

    monkeypatch.setattr("app.services.unified_bridge.fleet_router._direct_device_http", _fake_http)

    pins_r = client.get("/api/v2/devices/esp8266-sw01/gpio/pins", headers=hdrs)
    assert pins_r.status_code == 200
    assert pins_r.json()["via"] == "http"
    assert pins_r.json()["result"]["pins"][0]["duty"] == 50

    set_r = client.post(
        "/api/v2/devices/esp8266-sw01/gpio/set",
        headers=hdrs,
        json={"pin": 4, "op": "pwm", "duty": 50},
    )
    assert set_r.status_code == 200
    assert set_r.json()["via"] == "http"
    assert captured_http[-1] == ("POST", "/api/gpio/set", {"pin": 4, "duty": 128})
    assert set_r.json()["result"]["duty"] == 50


def test_timers_crud_ownership_and_scheduler_execution(monkeypatch):
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.services.device_executor.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)

    executed_commands: list[tuple[str, dict]] = []

    with TestClient(app) as tc:
        owner_hdrs = _reg_login(tc, "timer_owner")
        other_hdrs = _reg_login(tc, "timer_intruder")

        tc.post(
            "/api/v1/devices",
            headers=owner_hdrs,
            json={
                "device_uuid": "esp32c3-timer-dev",
                "name": "Timer Target",
                "capabilities": ["gpio", "display"],
            },
        )

        # Create countdown & interval timers
        c1 = tc.post(
            "/api/v2/timers",
            headers=owner_hdrs,
            json={
                "device_uuid": "esp32c3-timer-dev",
                "name": "30s Turn On LED",
                "schedule_type": "countdown",
                "delay_sec": 30,
                "action_type": "gpio_set",
                "action_payload": {"pin": 12, "op": "on"},
            },
        )
        assert c1.status_code == 201
        t1 = c1.json()
        assert t1["enabled"] is True
        assert t1["next_run_at"] is not None

        c2 = tc.post(
            "/api/v2/timers",
            headers=owner_hdrs,
            json={
                "device_uuid": "esp32c3-timer-dev",
                "name": "Periodic Display Refresh",
                "schedule_type": "interval",
                "interval_sec": 15,
                "action_type": "display_config",
                "action_payload": {"mode": "monitor", "backlight": 75},
            },
        )
        assert c2.status_code == 201
        t2 = c2.json()

        # Other user cannot edit, toggle, run, or delete owner's claimed device timer
        assert tc.put(f"/api/v2/timers/{t1['id']}", headers=other_hdrs, json={"name": "hijack"}).status_code == 403
        assert tc.post(f"/api/v2/timers/{t1['id']}/toggle", headers=other_hdrs, json={"enabled": False}).status_code == 403
        assert tc.post(f"/api/v2/timers/{t1['id']}/run", headers=other_hdrs).status_code == 403
        assert tc.delete(f"/api/v2/timers/{t1['id']}", headers=other_hdrs).status_code == 403

        # Toggle pause/resume + update
        assert tc.post(f"/api/v2/timers/{t1['id']}/toggle", headers=owner_hdrs, json={"enabled": False}).json()["next_run_at"] is None
        resumed = tc.post(f"/api/v2/timers/{t1['id']}/toggle", headers=owner_hdrs, json={"enabled": True}).json()
        assert resumed["enabled"] is True and resumed["next_run_at"] is not None

        upd = tc.put(f"/api/v2/timers/{t1['id']}", headers=owner_hdrs, json={"delay_sec": 5, "name": "5s Turn On LED"})
        assert upd.status_code == 200
        assert upd.json()["name"] == "5s Turn On LED"
        due_future = int(time.time()) + 60

        with tc.websocket_connect("/ws/device") as ws:
            ws.send_json({
                "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                "payload": {
                    "device_id": "esp32c3-timer-dev",
                    "name": "Timer Target",
                    "capabilities": ["gpio", "display"],
                },
            })
            assert ws.receive_json()["payload"]["status"] == "ok"

            stop_evt = threading.Event()

            def _responder():
                while not stop_evt.is_set():
                    try:
                        frame = ws.receive_json()
                    except Exception:
                        break
                    hdr = frame.get("header", {})
                    cmd = hdr.get("type")
                    mid = hdr.get("msg_id")
                    args = frame.get("payload") or {}
                    executed_commands.append((cmd, dict(args)))
                    ws.send_json({
                        "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                        "routing": {},
                        "payload": {"ok": True, "status": "ok", "exec_time_ms": 2},
                    })

            th = threading.Thread(target=_responder, daemon=True)
            th.start()
            try:
                # Immediate manual trigger via /run
                run_r = tc.post(f"/api/v2/timers/{t1['id']}/run", headers=owner_hdrs)
                assert run_r.status_code == 200
                assert run_r.json()["ok"] is True
                assert run_r.json()["timer"]["run_count"] == 1

                # Scheduler sweep due timers on the TestClient event loop
                with TestingSessionLocal() as db:
                    ran = tc.portal.call(lambda: run_due_timers(db, now_ts=due_future))
                    assert ran == 2
            finally:
                stop_evt.set()
                th.join(timeout=1)

        listed = {row["id"]: row for row in tc.get("/api/v2/timers", headers=owner_hdrs, params={"device_uuid": "esp32c3-timer-dev"}).json()}
        assert listed[t1["id"]]["enabled"] is False
        assert listed[t1["id"]]["next_run_at"] is None
        assert listed[t1["id"]]["run_count"] == 2
        assert listed[t1["id"]]["last_status"] == "ok"

        assert listed[t2["id"]]["enabled"] is True
        assert listed[t2["id"]]["next_run_at"] == due_future + 15
        assert listed[t2["id"]]["run_count"] == 1
        assert listed[t2["id"]]["last_status"] == "ok"

        assert tc.delete(f"/api/v2/timers/{t1['id']}", headers=owner_hdrs).status_code == 204
        assert tc.delete(f"/api/v2/timers/{t2['id']}", headers=owner_hdrs).status_code == 204

    assert ("gpio.set", {"pin": 12, "val": 1}) in executed_commands
    assert ("display.config", {"mode": "monitor", "backlight": 75}) in executed_commands
