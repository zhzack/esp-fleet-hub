"""tests/test_firmware_api.py - 固件管理与 OTA 升级 API 单元测试"""
import hashlib
import io
import pytest


@pytest.fixture
def admin_token(client):
    client.post(
        "/api/v1/auth/register",
        json={"username": "fw_admin", "password": "AdminPassword123", "role": "admin"},
    )
    res = client.post(
        "/api/v1/auth/login",
        json={"username": "fw_admin", "password": "AdminPassword123"},
    )
    return res.json()["access_token"]


@pytest.fixture
def user_token(client):
    client.post(
        "/api/v1/auth/register",
        json={"username": "fw_user", "password": "UserPassword123", "role": "user"},
    )
    res = client.post(
        "/api/v1/auth/login",
        json={"username": "fw_user", "password": "UserPassword123"},
    )
    return res.json()["access_token"]


@pytest.fixture
def uploaded_fw(client, admin_token):
    """上传一个 4KB 模拟固件，返回 (id, version, bytes)。"""
    fake_bin = b"\xAA\x55" * 2048  # 4KB
    res = client.post(
        "/api/v2/firmwares/upload",
        headers={"Authorization": f"Bearer {admin_token}"},
        data={"version": "v1.4.0", "description": "ota push test", "target_chip": "esp32s3"},
        files={"file": ("ble_hid_v1.4.0.bin", io.BytesIO(fake_bin), "application/octet-stream")},
    )
    assert res.status_code == 200
    return res.json()["id"], res.json()["version"], fake_bin


def test_firmware_upload_and_lifecycle(client, admin_token):
    headers = {"Authorization": f"Bearer {admin_token}"}

    # 1. 上传一个模拟的固件二进制文件
    fake_bin = b"\x00\x01\x02\x03\x04\x05" * 1024  # 6KB 模拟 bin
    file_tuple = ("ble_hid_v1.3.0.bin", io.BytesIO(fake_bin), "application/octet-stream")

    upload_res = client.post(
        "/api/v2/firmwares/upload",
        headers=headers,
        data={"version": "v1.3.0", "description": "OTA test firmware", "target_chip": "esp32s3"},
        files={"file": file_tuple},
    )
    assert upload_res.status_code == 200
    fw_data = upload_res.json()
    fw_id = fw_data["id"]
    assert fw_data["version"] == "v1.3.0"
    assert fw_data["file_size"] == len(fake_bin)
    assert "sha256" in fw_data
    assert "download_url" in fw_data

    # 2. 查询固件列表
    list_res = client.get("/api/v2/firmwares", headers=headers)
    assert list_res.status_code == 200
    items = list_res.json()
    assert len(items) >= 1
    assert any(item["id"] == fw_id for item in items)

    # 3. 详情查询
    detail_res = client.get(f"/api/v2/firmwares/{fw_id}", headers=headers)
    assert detail_res.status_code == 200
    assert detail_res.json()["version"] == "v1.3.0"

    # 4. 下载固件验证
    download_res = client.get(f"/api/v2/firmwares/{fw_id}/download")
    assert download_res.status_code == 200
    assert download_res.content == fake_bin
    assert download_res.headers.get("X-Firmware-Version") == "v1.3.0"

    # 5. 删除固件
    del_res = client.delete(f"/api/v2/firmwares/{fw_id}", headers=headers)
    assert del_res.status_code == 200
    assert del_res.json()["status"] == "ok"

    # 再次查询应 404
    assert client.get(f"/api/v2/firmwares/{fw_id}", headers=headers).status_code == 404


def test_upload_sha256_matches_file_hash(client, admin_token):
    """上传时服务端计算的 sha256 必须与文件真实哈希一致（设备端校验的信任根）。"""
    fake_bin = b"\x12\x34\x56" * 1000
    res = client.post(
        "/api/v2/firmwares/upload",
        headers={"Authorization": f"Bearer {admin_token}"},
        data={"version": "v1.4.1", "target_chip": "esp32s3"},
        files={"file": ("f.bin", io.BytesIO(fake_bin), "application/octet-stream")},
    )
    assert res.status_code == 200
    assert res.json()["sha256"] == hashlib.sha256(fake_bin).hexdigest()


def test_ota_push_payload_carries_url_version_sha256(client, admin_token, uploaded_fw, db, monkeypatch):
    """push 下发给设备的 ota.start 信封必须携带 url/version/sha256 三元组，
    且 sha256 与固件文件真实哈希一致——固件端据此做完整性校验。"""
    from app.database.models.device import Device

    fw_id, fw_version, fw_bytes = uploaded_fw
    db.add(Device(device_uuid="ota_push_dev", name="ota-push-dev"))
    db.commit()

    captured = {}

    async def fake_deliver(db_, device_uuid, cmd_type, payload, user_id, timeout_ms):
        captured.update(cmd_type=cmd_type, payload=payload, device_uuid=device_uuid)
        return {"status": "queued", "msg_id": "m1"}

    monkeypatch.setattr("app.api.v2.firmwares.dispatcher.deliver_command", fake_deliver)

    res = client.post(
        f"/api/v2/firmwares/{fw_id}/push",
        headers={"Authorization": f"Bearer {admin_token}"},
        json={"device_uuids": ["ota_push_dev"]},
    )
    assert res.status_code == 200
    body = res.json()
    assert body["status"] == "dispatched"
    assert body["firmware_version"] == fw_version

    # 下发信封内容
    assert captured["cmd_type"] == "ota.start"
    assert captured["device_uuid"] == "ota_push_dev"
    pl = captured["payload"]
    assert pl["version"] == fw_version
    assert pl["sha256"] == hashlib.sha256(fw_bytes).hexdigest()
    assert str(fw_id) in pl["url"] or fw_version in pl["url"]


def test_ota_push_requires_admin(client, user_token, uploaded_fw):
    """非 admin 不允许推送 OTA。"""
    fw_id, _, _ = uploaded_fw
    res = client.post(
        f"/api/v2/firmwares/{fw_id}/push",
        headers={"Authorization": f"Bearer {user_token}"},
        json={"device_uuids": ["any"]},
    )
    assert res.status_code == 403


def test_ota_push_unknown_device_reports_failure(client, admin_token, uploaded_fw):
    """目标设备不存在时逐设备返回 failed，不中断其他结果。"""
    fw_id, _, _ = uploaded_fw
    res = client.post(
        f"/api/v2/firmwares/{fw_id}/push",
        headers={"Authorization": f"Bearer {admin_token}"},
        json={"device_uuids": ["ghost_dev"]},
    )
    assert res.status_code == 200
    results = res.json()["results"]
    assert results[0]["status"] == "failed"
    assert "not found" in results[0]["error"]
