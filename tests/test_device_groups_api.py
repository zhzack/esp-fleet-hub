"""tests/test_device_groups_api.py - 设备分组 CRUD、成员管理与越权访问测试"""
import pytest


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


@pytest.fixture
def user_headers(client):
    return _register_and_login(client, "grp_alice")


@pytest.fixture
def other_headers(client):
    return _register_and_login(client, "grp_bob")


def test_group_crud_lifecycle(client, user_headers):
    # 创建
    res = client.post(
        "/api/v1/device-groups", headers=user_headers, json={"name": "Lab Devices"}
    )
    assert res.status_code == 201
    group = res.json()
    assert group["name"] == "Lab Devices"

    gid = group["id"]

    # 列表包含
    groups = client.get("/api/v1/device-groups", headers=user_headers).json()
    assert any(g["id"] == gid for g in groups)

    # 详情 + 404
    assert (
        client.get(f"/api/v1/device-groups/{gid}", headers=user_headers).status_code == 200
    )
    assert (
        client.get("/api/v1/device-groups/99999", headers=user_headers).status_code == 404
    )

    # 更新
    upd = client.put(
        f"/api/v1/device-groups/{gid}", headers=user_headers, json={"name": "Lab v2"}
    )
    assert upd.status_code == 200
    assert upd.json()["name"] == "Lab v2"

    # 删除 → 204，再查 404
    assert (
        client.delete(f"/api/v1/device-groups/{gid}", headers=user_headers).status_code
        == 204
    )
    assert (
        client.get(f"/api/v1/device-groups/{gid}", headers=user_headers).status_code == 404
    )


def test_group_membership_and_access_control(client, user_headers, other_headers):
    group = client.post(
        "/api/v1/device-groups", headers=user_headers, json={"name": "Cluster A"}
    ).json()
    gid = group["id"]

    d1 = client.post(
        "/api/v1/devices",
        headers=user_headers,
        json={"device_uuid": "grp_dev_1"},
    ).json()
    d2 = client.post(
        "/api/v1/devices",
        headers=user_headers,
        json={"device_uuid": "grp_dev_2"},
    ).json()

    # 添加成员（单次请求内含重复 id，验证幂等去重不触发唯一约束）
    res = client.post(
        f"/api/v1/device-groups/{gid}/members",
        headers=user_headers,
        json={"device_ids": [d1["id"], d2["id"], d1["id"]]},
    )
    assert res.status_code == 204

    # 跨请求重复添加同一成员也是幂等的
    res = client.post(
        f"/api/v1/device-groups/{gid}/members",
        headers=user_headers,
        json={"device_ids": [d1["id"]]},
    )
    assert res.status_code == 204

    members = client.get(
        f"/api/v1/device-groups/{gid}/devices", headers=user_headers
    ).json()
    assert sorted(members["devices"]) == sorted([d1["id"], d2["id"]])

    # 移除一个成员
    assert (
        client.delete(
            f"/api/v1/device-groups/{gid}/members/{d1['id']}", headers=user_headers
        ).status_code
        == 204
    )
    members = client.get(
        f"/api/v1/device-groups/{gid}/devices", headers=user_headers
    ).json()
    assert members["devices"] == [d2["id"]]

    # 其他用户访问 → 403（详情 / 成员列表 / 添加成员）
    assert (
        client.get(f"/api/v1/device-groups/{gid}", headers=other_headers).status_code == 403
    )
    assert (
        client.get(f"/api/v1/device-groups/{gid}/devices", headers=other_headers).status_code
        == 403
    )
    assert (
        client.post(
            f"/api/v1/device-groups/{gid}/members",
            headers=other_headers,
            json={"device_ids": [d1["id"]]},
        ).status_code
        == 403
    )
