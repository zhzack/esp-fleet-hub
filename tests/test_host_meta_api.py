"""tests/test_host_meta_api.py - 主机平台标记 API 测试

覆盖：标记/查询/更新平台（按蓝牙地址 upsert）、地址格式校验、
设备不存在 404、宏 variants 透传与平台变体归一化。
"""
import pytest


def _register_and_login(client, username):
    client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": "user"},
    )
    res = client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    )
    return {"Authorization": f"Bearer {res.json()['access_token']}"}


@pytest.fixture
def user_headers(client):
    return _register_and_login(client, "meta_alice")


@pytest.fixture
def device(client, user_headers, db):
    from app.database.models.device import Device

    d = Device(device_uuid="meta_dev_001", name="meta-dev")
    db.add(d)
    db.commit()
    return d


ADDR = "32:F4:7F:E5:11:D0"


def test_host_meta_upsert_and_list(client, user_headers, device):
    # 标记平台
    res = client.put(
        f"/api/v2/devices/{device.device_uuid}/host-meta/{ADDR}",
        headers=user_headers,
        json={"platform": "mac", "name": "MacBook"},
    )
    assert res.status_code == 200
    assert res.json()["platform"] == "mac"
    assert res.json()["addr"] == ADDR.upper()

    # 再标记第二台
    client.put(
        f"/api/v2/devices/{device.device_uuid}/host-meta/AA:BB:CC:DD:EE:01",
        headers=user_headers,
        json={"platform": "win"},
    )

    res = client.get(f"/api/v2/devices/{device.device_uuid}/host-meta", headers=user_headers)
    assert res.status_code == 200
    rows = {r["addr"]: r for r in res.json()}
    assert rows[ADDR.upper()]["platform"] == "mac"
    assert rows[ADDR.upper()]["name"] == "MacBook"
    assert rows["AA:BB:CC:DD:EE:01"]["platform"] == "win"

    # 更新（同地址 upsert 改平台）
    client.put(
        f"/api/v2/devices/{device.device_uuid}/host-meta/{ADDR}",
        headers=user_headers,
        json={"platform": "win"},
    )
    res = client.get(f"/api/v2/devices/{device.device_uuid}/host-meta", headers=user_headers)
    rows = {r["addr"]: r for r in res.json()}
    assert rows[ADDR.upper()]["platform"] == "win"
    assert len(res.json()) == 2  # 仍是两条（upsert 未新增）


def test_host_meta_bad_addr_and_unknown_device(client, user_headers, device):
    res = client.put(
        f"/api/v2/devices/{device.device_uuid}/host-meta/not-an-addr",
        headers=user_headers,
        json={"platform": "win"},
    )
    assert res.status_code == 400

    res = client.put(
        "/api/v2/devices/ghost_device/host-meta/AA:BB:CC:DD:EE:02",
        headers=user_headers,
        json={"platform": "win"},
    )
    assert res.status_code == 404

    res = client.put(
        f"/api/v2/devices/{device.device_uuid}/host-meta/AA:BB:CC:DD:EE:03",
        headers=user_headers,
        json={"platform": "android"},  # 非法平台枚举
    )
    assert res.status_code == 422


def test_macro_variants_passthrough(client, user_headers):
    """创建携带平台变体的宏 → 存储归一化保留合法变体、过滤非法键与空列表。"""
    res = client.post(
        "/api/v1/macros",
        headers=user_headers,
        json={
            "name": "复制粘贴",
            "steps": [{"type": "keyboard_tap", "key": "KEY_CTRL_C"}],
            "variants": {
                "mac": [{"type": "keyboard_tap", "key": "KEY_LEFTGUI"},
                        {"type": "keyboard_tap", "key": "KEY_C"}],
                "win": [],               # 空变体应被过滤（回退通用）
                "android": [{"type": "text", "content": "x"}],  # 非法平台键应被过滤
            },
        },
    )
    assert res.status_code == 201
    content = res.json()["content_json"]
    assert content["steps"][0]["key"] == "KEY_CTRL_C"
    assert list(content["variants"].keys()) == ["mac"]
    assert len(content["variants"]["mac"]) == 2

    # 更新：显式传 {} 清空变体
    res = client.put(
        f"/api/v1/macros/{res.json()['id']}",
        headers=user_headers,
        json={"variants": {}},
    )
    assert res.status_code == 200
    assert "variants" not in res.json()["content_json"]
    assert res.json()["content_json"]["steps"][0]["key"] == "KEY_CTRL_C"  # steps 未动
