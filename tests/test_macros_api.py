"""tests/test_macros_api.py - 宏 CRUD、config 归一化与跨用户隔离测试"""
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
    return _register_and_login(client, "macro_alice")


@pytest.fixture
def other_headers(client):
    return _register_and_login(client, "macro_bob")


def test_macro_crud_lifecycle(client, user_headers, other_headers):
    # 1. 创建（steps + 默认 config 归一化进 content_json）
    res = client.post(
        "/api/v1/macros",
        headers=user_headers,
        json={
            "name": "打开记事本",
            "steps": [
                {"type": "keyboard_tap", "key": "KEY_LEFTMETA"},
                {"type": "delay", "delay_ms": 300},
                {"type": "text_stream", "content": "notepad"},
                {"type": "keyboard_tap", "key": "KEY_ENTER"},
            ],
        },
    )
    assert res.status_code == 201
    m = res.json()
    assert m["name"] == "打开记事本"
    content = m["content_json"]
    assert len(content["steps"]) == 4
    assert content["config"]["execution_mode"] == "ble_targeted"
    assert content["config"]["require_connected"] is True

    # 2. 列表 / 详情
    macros = client.get("/api/v1/macros", headers=user_headers).json()
    assert any(x["id"] == m["id"] for x in macros)
    assert client.get(f"/api/v1/macros/{m['id']}", headers=user_headers).status_code == 200

    # 3. 更新（改名 + 换 steps）
    upd = client.put(
        f"/api/v1/macros/{m['id']}",
        headers=user_headers,
        json={"name": "改名宏", "steps": [{"type": "keyboard_tap", "key": "KEY_A"}]},
    )
    assert upd.status_code == 200
    assert upd.json()["name"] == "改名宏"
    assert len(upd.json()["content_json"]["steps"]) == 1

    # 4. 跨用户隔离：他人读写删均 404（不泄漏存在性）
    assert client.get(f"/api/v1/macros/{m['id']}", headers=other_headers).status_code == 404
    assert (
        client.put(
            f"/api/v1/macros/{m['id']}", headers=other_headers, json={"name": "hack"}
        ).status_code
        == 404
    )
    assert (
        client.delete(f"/api/v1/macros/{m['id']}", headers=other_headers).status_code == 404
    )

    # 5. 删除 → 204，再查 404
    assert (
        client.delete(f"/api/v1/macros/{m['id']}", headers=user_headers).status_code == 204
    )
    assert client.get(f"/api/v1/macros/{m['id']}", headers=user_headers).status_code == 404


def test_macro_config_normalization(client, user_headers):
    # execution_mode=network_debug 时 model_validator 强制 require_connected=False
    res = client.post(
        "/api/v1/macros",
        headers=user_headers,
        json={
            "name": "调试宏",
            "steps": [{"type": "mouse_move", "x": 10, "y": 20}],
            "config": {"execution_mode": "network_debug", "require_connected": True},
        },
    )
    assert res.status_code == 201
    cfg = res.json()["content_json"]["config"]
    assert cfg["execution_mode"] == "network_debug"
    assert cfg["require_connected"] is False
