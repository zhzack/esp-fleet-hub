"""tests/test_user_api.py - 用户鉴权、多格式登录与密码修改单元测试"""
import pytest



def test_user_registration(client):
    # 正常注册
    res = client.post(
        "/api/v1/auth/register",
        json={"username": "alice", "password": "password123", "role": "user"},
    )
    assert res.status_code == 201
    data = res.json()
    assert data["username"] == "alice"
    assert data["role"] == "user"

    # 重复注册冲突检测
    dup_res = client.post(
        "/api/v1/auth/register",
        json={"username": "alice", "password": "new_password", "role": "user"},
    )
    assert dup_res.status_code == 409
    assert "用户名已存在" in dup_res.json()["detail"]


def test_login_json_and_form(client):
    # 先注册用户
    client.post(
        "/api/v1/auth/register",
        json={"username": "bob", "password": "SecretPassword123", "role": "user"},
    )

    # 1. JSON 格式登录
    res_json = client.post(
        "/api/v1/auth/login",
        json={"username": "bob", "password": "SecretPassword123"},
    )
    assert res_json.status_code == 200
    token_data = res_json.json()
    assert "access_token" in token_data
    assert token_data["token_type"] == "bearer"

    # 2. 表单 Form-data 格式登录
    res_form = client.post(
        "/api/v1/auth/login",
        data={"username": "bob", "password": "SecretPassword123"},
    )
    assert res_form.status_code == 200
    assert "access_token" in res_form.json()

    # 3. 错误密码
    res_fail = client.post(
        "/api/v1/auth/login",
        json={"username": "bob", "password": "WrongPassword"},
    )
    assert res_fail.status_code == 401
    assert "用户名或密码错误" in res_fail.json()["detail"]

    # 4. 空参数
    res_empty = client.post(
        "/api/v1/auth/login",
        json={"username": "", "password": ""},
    )
    assert res_empty.status_code == 400


def test_user_me_and_password_change(client):
    # 注册用户
    client.post(
        "/api/v1/auth/register",
        json={"username": "charlie", "password": "OldPassword123", "role": "admin"},
    )

    # 登录获取 Token
    login_res = client.post(
        "/api/v1/auth/login",
        json={"username": "charlie", "password": "OldPassword123"},
    )
    token = login_res.json()["access_token"]
    headers = {"Authorization": f"Bearer {token}"}

    # 获取个人信息 /me
    me_res = client.get("/api/v1/auth/me", headers=headers)
    assert me_res.status_code == 200
    assert me_res.json()["username"] == "charlie"
    assert me_res.json()["role"] == "admin"

    # 尝试使用错误原密码修改
    bad_change = client.post(
        "/api/v1/auth/change-password",
        headers=headers,
        json={"old_password": "WrongOldPassword", "new_password": "NewPassword456"},
    )
    assert bad_change.status_code == 400
    assert "原密码不正确" in bad_change.json()["detail"]

    # 正确修改密码
    ok_change = client.post(
        "/api/v1/auth/change-password",
        headers=headers,
        json={"old_password": "OldPassword123", "new_password": "NewPassword456"},
    )
    assert ok_change.status_code == 200
    assert "密码修改成功" in ok_change.json()["message"]

    # 旧密码不再可用
    old_login = client.post(
        "/api/v1/auth/login",
        json={"username": "charlie", "password": "OldPassword123"},
    )
    assert old_login.status_code == 401

    # 新密码成功登录
    new_login = client.post(
        "/api/v1/auth/login",
        json={"username": "charlie", "password": "NewPassword456"},
    )
    assert new_login.status_code == 200
