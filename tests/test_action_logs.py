"""tests/test_action_logs.py - 动作日志筛选、分页与查询单元测试"""
import uuid
import pytest
from app.database.crud import action_log as crud_action_log
from app.database.crud import user as crud_user


def test_action_log_crud_and_filters(db):
    # 插入若干模拟日志
    log1 = crud_action_log.create_pending(
        db,
        msg_id=str(uuid.uuid4()),
        user_id=1,
        device_id=10,
        action_type="mouse_move",
        payload={"dx": 10, "dy": 20},
    )
    crud_action_log.mark_acked(db, log1.msg_id, exec_time_ms=15)

    log2 = crud_action_log.create_pending(
        db,
        msg_id=str(uuid.uuid4()),
        user_id=1,
        device_id=10,
        action_type="keyboard_tap",
        payload={"key": "KEY_ENTER"},
    )
    crud_action_log.mark_failed(db, log2.msg_id, "ERR_TIMEOUT", "Hardware timeout")

    log3 = crud_action_log.create_pending(
        db,
        msg_id=str(uuid.uuid4()),
        user_id=2,
        device_id=20,
        action_type="mouse_click",
        payload={"button": "left"},
    )

    # 1. 查询全部
    all_logs = crud_action_log.list_recent(db, limit=10)
    assert len(all_logs) == 3

    # 2. 按 status 过滤
    acked_logs = crud_action_log.list_recent(db, status="acked")
    assert len(acked_logs) == 1
    assert acked_logs[0].action_type == "mouse_move"
    assert acked_logs[0].exec_time_ms == 15

    failed_logs = crud_action_log.list_recent(db, status="failed")
    assert len(failed_logs) == 1
    assert failed_logs[0].error_code == "ERR_TIMEOUT"

    # 3. 按 action_type 过滤
    mouse_moves = crud_action_log.list_recent(db, action_type="mouse_move")
    assert len(mouse_moves) == 1

    # 4. 按 device_id 过滤
    dev10_logs = crud_action_log.list_recent(db, device_id=10)
    assert len(dev10_logs) == 2

    # 5. 分页 (limit, offset)
    paged = crud_action_log.list_recent(db, limit=1, offset=1)
    assert len(paged) == 1

    # 6. count_logs 计数
    assert crud_action_log.count_logs(db) == 3
    assert crud_action_log.count_logs(db, device_id=10) == 2
    assert crud_action_log.count_logs(db, status="failed") == 1


def test_action_log_api_endpoints(client, db):
    # 创建一个普通用户和一个管理员用户
    user = crud_user.create(db, "normal_user", "password123", role="user")
    admin = crud_user.create(db, "admin_user", "password123", role="admin")

    # 插入日志
    log1 = crud_action_log.create_pending(
        db,
        msg_id=str(uuid.uuid4()),
        user_id=user.id,
        device_id=1,
        action_type="mouse_move",
        payload={},
    )
    crud_action_log.mark_acked(db, log1.msg_id, 12)

    log2 = crud_action_log.create_pending(
        db,
        msg_id=str(uuid.uuid4()),
        user_id=admin.id,
        device_id=2,
        action_type="keyboard_tap",
        payload={},
    )

    # 普通用户登录
    u_res = client.post("/api/v1/auth/login", json={"username": "normal_user", "password": "password123"})
    u_token = u_res.json()["access_token"]
    u_headers = {"Authorization": f"Bearer {u_token}"}

    # 普通用户查看日志（仅能看到自己的）
    u_logs = client.get("/api/v2/logs", headers=u_headers).json()
    assert len(u_logs) == 1
    assert u_logs[0]["action_type"] == "mouse_move"

    # 管理员登录
    a_res = client.post("/api/v1/auth/login", json={"username": "admin_user", "password": "password123"})
    a_token = a_res.json()["access_token"]
    a_headers = {"Authorization": f"Bearer {a_token}"}

    # 管理员查看全部日志
    a_logs = client.get("/api/v2/logs", headers=a_headers).json()
    assert len(a_logs) == 2

    # 带查询参数过滤
    filtered = client.get(
        "/api/v2/logs?status=acked&action_type=mouse_move",
        headers=a_headers,
    ).json()
    assert len(filtered) == 1
    assert filtered[0]["status"] == "acked"
