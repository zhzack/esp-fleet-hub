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

    # Batch OTA URL generation points to /api/fleet/download
    ota = client.post(
        "/api/v2/unified/ota/batch",
        json={"file": "esp32s3_display_hid.bin", "device_ids": ["esp32s3-test01"]},
        headers=hdrs,
    )
    assert ota.status_code == 200
    assert "/api/fleet/download?file=esp32s3_display_hid.bin" in ota.json()["url"]

    dl = client.get("/api/fleet/download", params={"file": "esp32s3_display_hid.bin"})
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
