"""Integration tests for py_web <-> esp_unified_framework bridged APIs."""

import hashlib
import hmac
import sys
from pathlib import Path

import pytest


def _admin_headers(client):
    client.post(
        "/api/v1/auth/register",
        json={"username": "uadmin", "password": "secret123", "role": "admin"},
    )
    tok = client.post(
        "/api/v1/auth/login",
        json={"username": "uadmin", "password": "secret123"},
    ).json()["access_token"]
    return {"Authorization": f"Bearer {tok}"}


def test_unified_fleet_endpoints_and_publish(client, db, tmp_path, monkeypatch):
    from app.database.crud import device as crud_device
    from app.services import unified_bridge as ub

    ub.init_unified_hub()
    hub_db = ub.hub_db
    build_flavor = ub.build_flavor

    dist_dir = tmp_path / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)
    dummy_bin = dist_dir / "esp32s3_display_hid.bin"
    dummy_bin.write_bytes(b"\xE9" + b"\xAB" * 511)
    monkeypatch.setattr(build_flavor, "DIST_DIR", str(dist_dir))

    # Provision a unified device & challenge/register via /api/fleet/*
    secret = "0123456789abcdef0123456789abcdef"
    hub_db.provision_device(
        device_id="esp32s3-test01",
        mac="30:ED:A0:00:00:01",
        chip="ESP32-S3",
        flash_mb=16,
        device_secret=secret,
        hub_url="http://127.0.0.1:8000",
    )

    ch = client.get("/api/fleet/challenge", params={"device_id": "esp32s3-test01"})
    assert ch.status_code == 200
    nonce = ch.json()["nonce"]
    proof = hmac.new(secret.encode(), nonce.encode(), hashlib.sha256).hexdigest()

    reg = client.post(
        "/api/fleet/register",
        json={
            "device_id": "esp32s3-test01",
            "chip": "ESP32-S3",
            "ip": "192.168.68.180",
            "nonce": nonce,
            "proof": proof,
            "capabilities": {"usb_otg": {"enabled": True}, "storage": {"enabled": True}},
            "config": {"config_ver": 1},
        },
    )
    assert reg.status_code == 200
    assert reg.json()["device"]["status"] == "online"

    # Authenticated Vue endpoints
    hdrs = _admin_headers(client)
    devs = client.get("/api/v2/unified/devices", headers=hdrs)
    assert devs.status_code == 200
    assert any(d["device_id"] == "esp32s3-test01" for d in devs.json()["devices"])

    profs = client.get("/api/v2/unified/profiles", headers=hdrs)
    assert profs.status_code == 200
    assert len(profs.json()["profiles"]) >= 1

    fws = client.get("/api/v2/unified/firmwares", headers=hdrs)
    assert fws.status_code == 200
    assert fws.json()["firmwares"][0]["file"] == "esp32s3_display_hid.bin"

    # Publish into HID main Firmware repository
    from app.services import unified_bridge
    monkeypatch.setattr(unified_bridge, "HID_UPLOAD_DIR", tmp_path / "uploads")
    pub = client.post(
        "/api/v2/unified/firmwares/esp32s3_display_hid.bin/publish",
        json={"version": "1.2.0-s3"},
        headers=hdrs,
    )
    assert pub.status_code == 200
    assert pub.json()["version"] == "1.2.0-s3"

    # Batch OTA URL generation points to signed /api/fleet/download
    ota = client.post(
        "/api/v2/unified/ota/batch",
        json={"file": "esp32s3_display_hid.bin", "device_ids": ["esp32s3-test01"]},
        headers=hdrs,
    )
    assert ota.status_code == 200
    ota_url = ota.json()["url"]
    assert "/api/fleet/download?file=esp32s3_display_hid.bin" in ota_url
    assert "&sig=" in ota_url

    assert client.get("/api/fleet/download", params={"file": "esp32s3_display_hid.bin"}).status_code == 401
    sig_tok = ota_url.split("&sig=", 1)[1]
    dl = client.get("/api/fleet/download", params={"file": "esp32s3_display_hid.bin", "sig": sig_tok})
    assert dl.status_code == 200
    assert dl.content[:1] == b"\xE9"

    # RBAC checks: register regular users and enforce least privilege
    client.post("/api/v1/auth/register", json={"username": "u_norm1", "password": "secret123", "role": "user"})
    tok1 = client.post("/api/v1/auth/login", json={"username": "u_norm1", "password": "secret123"}).json()["access_token"]
    h_u1 = {"Authorization": f"Bearer {tok1}"}

    client.post("/api/v1/auth/register", json={"username": "u_norm2", "password": "secret123", "role": "user"})
    tok2 = client.post("/api/v1/auth/login", json={"username": "u_norm2", "password": "secret123"}).json()["access_token"]
    h_u2 = {"Authorization": f"Bearer {tok2}"}

    # Admin-only actions rejected with 403 for non-admin
    assert client.get("/api/v2/unified/ports", headers=h_u1).status_code == 403
    assert client.post("/api/v2/unified/build", json={"name": "x", "target": "esp32s3"}, headers=h_u1).status_code == 403
    assert client.post("/api/v2/unified/flash_serial", json={"port": "COM1"}, headers=h_u1).status_code == 403
    assert client.post("/api/v2/unified/ota/batch", json={"file": "esp32s3_display_hid.bin", "device_ids": ["esp32s3-test01"]}, headers=h_u1).status_code == 403
    assert client.post("/api/v2/unified/firmwares/esp32s3_display_hid.bin/publish", json={}, headers=h_u1).status_code == 403

    # Unclaimed device cannot be proxied/deleted before claiming, but user 1 can claim it
    assert client.get("/api/v2/unified/devices/esp32s3-test01/proxy", headers=h_u1).status_code == 403
    claim1 = client.post("/api/v2/unified/devices/esp32s3-test01/claim", json={"name": "MyS3", "claimed": True}, headers=h_u1)
    assert claim1.status_code == 200

    # User 2 cannot list, claim, push config to, or delete User 1's claimed device
    devs_u2 = client.get("/api/v2/unified/devices", headers=h_u2).json()["devices"]
    assert all(d["device_id"] != "esp32s3-test01" for d in devs_u2)
    assert client.post("/api/v2/unified/devices/esp32s3-test01/claim", json={"name": "Hijack"}, headers=h_u2).status_code == 403
    assert client.post("/api/v2/unified/devices/esp32s3-test01/config", json={"config": {"mdns_host": "x"}}, headers=h_u2).status_code == 403
    assert client.delete("/api/v2/unified/devices/esp32s3-test01", headers=h_u2).status_code == 403

    # Owner User 1 can manage & delete their own claimed device cleanly (syncing both tables)
    assert crud_device.get_by_uuid(db, "esp32s3-test01") is not None
    assert client.delete("/api/v2/unified/devices/esp32s3-test01", headers=h_u1).status_code == 200
    db.expire_all()
    assert crud_device.get_by_uuid(db, "esp32s3-test01") is None
    devs_adm = client.get("/api/v2/unified/devices", headers=hdrs).json()["devices"]
    assert all(d["device_id"] != "esp32s3-test01" for d in devs_adm)


def test_unified_ws_tunnel_proxy_and_batch_ota(tmp_path, monkeypatch):
    """Verify NAT-friendly WS fallback for config, command, proxy (incl. JPEG), and batch OTA tracker."""
    import base64
    import threading
    import uuid
    from fastapi.testclient import TestClient
    from app.main import app
    from app.services import unified_bridge as ub
    from app.services.device_registry import device_registry
    from app.services.ota_tracker import ota_tracker
    from tests.conftest import TestingSessionLocal, test_engine

    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)

    ub.init_unified_hub()
    dist_dir = tmp_path / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)
    fw_bin = dist_dir / "esp32s3_display_hid.bin"
    fw_bin.write_bytes(b"\xE9" + b"\xCC" * 1023)
    monkeypatch.setattr(ub.build_flavor, "DIST_DIR", str(dist_dir))

    # Register device with NO LAN IP (simulating NAT / Docker bridge without inbound HTTP)
    dev_id = "esp32s3-nat01"
    ub.hub_db.register_or_update_device({
        "device_id": dev_id,
        "chip": "ESP32-S3",
        "ip": "",
        "capabilities": {"usb_otg": {"enabled": True}, "camera": {"enabled": True}},
        "config": {"config_ver": 1},
    })

    try:
        with TestClient(app) as tc:
            hdrs = _admin_headers(tc)

            with tc.websocket_connect("/ws/device") as ws:
                ws.send_json({
                    "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                    "routing": {},
                    "payload": {"device_id": dev_id, "name": dev_id, "capabilities": ["usb_keyboard", "ota", "camera"]},
                })
                ack_reg = ws.receive_json()
                assert ack_reg["payload"]["status"] == "ok"

                def _dev_responder():
                    while True:
                        try:
                            msg = ws.receive_json()
                        except Exception:
                            break
                        mt = msg["header"]["type"]
                        mid = msg["header"]["msg_id"]
                        if mt == "config.set":
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {"ok": True, "status": "ok", "exec_time_ms": 2},
                            })
                        elif mt == "gpio.pins":
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {"ok": True, "status": "ok", "pins": [{"pin": 2, "val": 1}]},
                            })
                        elif mt == "camera.capture":
                            jpg = b"\xff\xd8\xff\xe0\x00\x10JFIF"
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {
                                    "ok": True,
                                    "status": "ok",
                                    "content_type": "image/jpeg",
                                    "image_b64": base64.b64encode(jpg).decode("ascii"),
                                },
                            })
                        elif mt == "ota.start":
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "ref_msg_id": mid, "type": "ack", "timestamp": 0},
                                "routing": {},
                                "payload": {"ok": True, "status": "ok"},
                            })
                            ws.send_json({
                                "header": {"msg_id": str(uuid.uuid4()), "device_id": dev_id, "type": "ota_progress", "timestamp": 0},
                                "routing": {},
                                "payload": {"status": "downloading", "progress": 60, "bytes_read": 600, "total_bytes": 1024},
                            })

                t = threading.Thread(target=_dev_responder, daemon=True)
                t.start()

                # 1. Config push falls over WebSocket even with empty ip=""
                cfg_res = tc.post(
                    f"/api/v2/unified/devices/{dev_id}/config",
                    json={"config": {"mdns_host": "nat-s3"}},
                    headers=hdrs,
                )
                assert cfg_res.status_code == 200
                assert cfg_res.json()["pushed_live"] is True
                assert cfg_res.json()["via"] == "websocket"

                # 2. Proxy GET falls back seamlessly to WS for JSON & binary JPEG capture
                g_res = tc.get(
                    f"/api/v2/unified/devices/{dev_id}/proxy",
                    params={"path": "/api/gpio/pins"},
                    headers=hdrs,
                )
                assert g_res.status_code == 200
                assert g_res.json()["via"] == "websocket"
                assert g_res.json()["pins"][0]["pin"] == 2

                cap_res = tc.get(
                    f"/api/v2/unified/devices/{dev_id}/proxy",
                    params={"path": "/capture"},
                    headers=hdrs,
                )
                assert cap_res.status_code == 200
                assert cap_res.headers["content-type"].startswith("image/jpeg")
                assert cap_res.content.startswith(b"\xff\xd8\xff")

                # 3. Batch OTA prefers WS ota.start and updates ota_tracker progress
                ota_res = tc.post(
                    "/api/v2/unified/ota/batch",
                    json={"file": "esp32s3_display_hid.bin", "device_ids": [dev_id]},
                    headers=hdrs,
                )
                assert ota_res.status_code == 200
                assert ota_res.json()["results"][0]["mode"] == "ws_ota_start"

                import time
                time.sleep(0.15)
                prog = tc.get(f"/api/v2/unified/ota/progress/{dev_id}", headers=hdrs)
                assert prog.status_code == 200
                assert prog.json()["progress"] == 60
                assert prog.json()["status"] == "downloading"
    finally:
        device_registry._live.clear()
        ota_tracker.clear(dev_id)


def test_multi_task_queue_external_env_and_public_url(client, tmp_path, monkeypatch):
    """Verify per-port flash isolation, multi-task logs, env compiler config, and HUB_PUBLIC_URL."""
    from app.core.config.config import settings
    from app.services import unified_bridge as ub

    wc = ub.web_compiler
    t1 = wc.create_task("flash", "COM5 <- s3.bin", status="flashing", initial_log="port5\n")
    t2 = wc.create_task("flash", "COM11 <- c3.bin", status="flashing", initial_log="port11\n")
    assert t1 != t2
    assert wc._get_port_lock("COM5") is not wc._get_port_lock("COM11")

    snap1 = wc.get_task_snapshot(t1)
    assert snap1["task_id"] == t1
    assert "port5" in snap1["log"]
    assert any(t["task_id"] == t2 for t in snap1["tasks"])

    monkeypatch.setenv("IDF_BUILD_CMD", "custom-idf")
    monkeypatch.setenv("IDF_BUILD_SSH_HOST", "ci@10.0.0.5")
    monkeypatch.setenv("IDF_REMOTE_PROJECT_DIR", "/srv/esp")
    ec = ub.build_flavor._build_env_config()
    assert ec["cmd"] == "custom-idf"
    assert ec["ssh_host"] == "ci@10.0.0.5"
    assert ec["remote_dir"] == "/srv/esp"

    hdrs = _admin_headers(client)
    res_t = client.get("/api/v2/unified/task", params={"task_id": t1}, headers=hdrs)
    assert res_t.status_code == 200
    assert res_t.json()["task_id"] == t1

    dist_dir = tmp_path / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)
    (dist_dir / "esp32s3_display_hid.bin").write_bytes(b"\xE9" + b"\x00" * 511)
    monkeypatch.setattr(ub.build_flavor, "DIST_DIR", str(dist_dir))
    monkeypatch.setattr(settings, "HUB_PUBLIC_URL", "https://hub.example.org:8443")

    ub.hub_db.register_or_update_device({
        "device_id": "esp32s3-pub01",
        "chip": "ESP32-S3",
        "ip": "",
        "config": {"config_ver": 1},
    })
    ota = client.post(
        "/api/v2/unified/ota/batch",
        json={"file": "esp32s3_display_hid.bin", "device_ids": ["esp32s3-pub01"]},
        headers=hdrs,
    )
    assert ota.status_code == 200
    assert ota.json()["url"].startswith("https://hub.example.org:8443/api/fleet/download?file=")
