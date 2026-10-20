"""tests/test_devices_api.py - 设备 CRUD、归属过滤、认领与 BLE 槽位 API 测试"""
import pytest

from app.database.crud import device as crud_device


def _register_and_login(client, username, role="user"):
    client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": role},
    )
    res = client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    )
    return {"Authorization": f"Bearer {res.json()['access_token']}"}


def _me(client, headers):
    return client.get("/api/v1/auth/me", headers=headers).json()


@pytest.fixture
def user_headers(client):
    return _register_and_login(client, "dev_alice")


@pytest.fixture
def other_headers(client):
    return _register_and_login(client, "dev_bob")


@pytest.fixture
def admin_headers(client):
    return _register_and_login(client, "dev_admin", role="admin")


def test_device_crud_and_ownership(client, db, user_headers, other_headers, admin_headers):
    # 1. 创建设备
    res = client.post(
        "/api/v1/devices",
        headers=user_headers,
        json={
            "device_uuid": "esp32_dev_001",
            "name": "Dev One",
            "capabilities": ["usb_keyboard"],
        },
    )
    assert res.status_code == 201
    dev = res.json()
    assert dev["device_uuid"] == "esp32_dev_001"
    assert dev["owner_user_id"] == _me(client, user_headers)["id"]
    assert dev["status"] == "offline"
    assert dev["capabilities"] == ["usb_keyboard"]

    # 2. 重复 UUID → 409
    dup = client.post(
        "/api/v1/devices",
        headers=user_headers,
        json={"device_uuid": "esp32_dev_001"},
    )
    assert dup.status_code == 409

    # 3. 其他用户创建自己的设备
    res2 = client.post(
        "/api/v1/devices",
        headers=other_headers,
        json={"device_uuid": "esp32_dev_002", "name": "Dev Two"},
    )
    assert res2.status_code == 201

    # 4. 模拟硬件自注册的未认领设备
    crud_device.create(db, "esp32_dev_unclaimed", None)

    # 5. 列表归属过滤：普通用户看自己的 + 未认领的
    mine = client.get("/api/v1/devices", headers=user_headers).json()
    mine_uuids = {d["device_uuid"] for d in mine}
    assert "esp32_dev_001" in mine_uuids
    assert "esp32_dev_unclaimed" in mine_uuids
    assert "esp32_dev_002" not in mine_uuids

    # 管理员看全部
    all_devs = client.get("/api/v1/devices", headers=admin_headers).json()
    all_uuids = {d["device_uuid"] for d in all_devs}
    assert {"esp32_dev_001", "esp32_dev_002", "esp32_dev_unclaimed"} <= all_uuids

    # 6. 详情：自己的 200，他人的 403，不存在的 404
    assert client.get(f"/api/v1/devices/{dev['id']}", headers=user_headers).status_code == 200
    assert (
        client.get(f"/api/v1/devices/{res2.json()['id']}", headers=user_headers).status_code
        == 403
    )
    assert client.get("/api/v1/devices/99999", headers=user_headers).status_code == 404

    # 7. 更新自己的设备（改名 + 改能力）
    upd = client.put(
        f"/api/v1/devices/{dev['id']}",
        headers=user_headers,
        json={"name": "Dev One Renamed", "capabilities": ["ble_keyboard", "ble_mouse"]},
    )
    assert upd.status_code == 200
    assert upd.json()["name"] == "Dev One Renamed"
    assert upd.json()["capabilities"] == ["ble_keyboard", "ble_mouse"]

    # 8. 普通用户不能转让所有权
    assert (
        client.put(
            f"/api/v1/devices/{dev['id']}",
            headers=user_headers,
            json={"owner_user_id": 123},
        ).status_code
        == 403
    )


def test_device_claim_flow(client, db, user_headers, other_headers):
    unclaimed = crud_device.create(db, "esp32_claim_target", None)

    # 其他用户认领成功
    res = client.post(f"/api/v1/devices/{unclaimed.id}/claim", headers=other_headers)
    assert res.status_code == 200
    assert res.json()["owner_user_id"] == _me(client, other_headers)["id"]

    # 已被认领 → 400
    res2 = client.post(f"/api/v1/devices/{unclaimed.id}/claim", headers=user_headers)
    assert res2.status_code == 400

    # 不存在 → 400
    assert client.post("/api/v1/devices/99999/claim", headers=user_headers).status_code == 400


def test_slot_crud_and_validation(client, user_headers):
    dev = client.post(
        "/api/v1/devices",
        headers=user_headers,
        json={"device_uuid": "esp32_slot_001", "name": "Slot Host"},
    ).json()
    base = "/api/v2/devices/esp32_slot_001/slots"

    # 1. 绑定槽位：addr 自动大写
    res = client.put(
        f"{base}/1",
        headers=user_headers,
        json={"addr_type": 0, "addr": "aa:bb:cc:dd:ee:ff", "name": "MacBook"},
    )
    assert res.status_code == 200
    slot = res.json()
    assert slot["slot_index"] == 1
    assert slot["addr"] == "AA:BB:CC:DD:EE:FF"
    assert slot["name"] == "MacBook"
    assert slot["last_used_at"] is None

    # 2. 槽位范围校验（仅 1..9 合法）
    assert (
        client.put(
            f"{base}/0",
            headers=user_headers,
            json={"addr_type": 0, "addr": "AA:BB:CC:DD:EE:FF"},
        ).status_code
        == 400
    )
    assert (
        client.put(
            f"{base}/10",
            headers=user_headers,
            json={"addr_type": 0, "addr": "AA:BB:CC:DD:EE:FF"},
        ).status_code
        == 400
    )

    # 3. 列表（多槽位 + 按 slot_index 排序）
    client.put(
        f"{base}/2",
        headers=user_headers,
        json={"addr_type": 1, "addr": "11:22:33:44:55:66"},
    )
    slots = client.get(base, headers=user_headers).json()
    assert [s["slot_index"] for s in slots] == [1, 2]

    # 4. upsert：覆盖已有槽位
    res = client.put(
        f"{base}/1",
        headers=user_headers,
        json={"addr_type": 0, "addr": "FF:FF:FF:FF:FF:FF", "name": "Renamed"},
    )
    assert res.status_code == 200
    assert res.json()["addr"] == "FF:FF:FF:FF:FF:FF"
    assert res.json()["name"] == "Renamed"

    # 5. 删除 + 重复删除 404
    assert client.delete(f"{base}/1", headers=user_headers).status_code == 200
    assert client.delete(f"{base}/1", headers=user_headers).status_code == 404

    # 6. 未知设备 → 404
    assert (
        client.get("/api/v2/devices/unknown_dev/slots", headers=user_headers).status_code
        == 404
    )
