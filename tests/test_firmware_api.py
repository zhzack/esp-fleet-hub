"""tests/test_firmware_api.py - 固件管理与 OTA 升级 API 单元测试"""
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
