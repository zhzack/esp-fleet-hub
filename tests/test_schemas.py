"""tests/test_schemas.py - Pydantic v2 ConfigDict(from_attributes) 序列化与 WS 信封模型测试

覆盖本次 class Config -> model_config = ConfigDict 迁移的全部 schema：
DeviceOut / DeviceGroupOut / MacroOut / VehicleOut / ActionLogOut / SlotOut，
以及 ws_message.py 的 WsEnvelope 协议信封解析。
"""
import uuid
from datetime import datetime

import pytest
from pydantic import ValidationError

from app.api.v2.slots import SlotOut
from app.database.crud import action_log as crud_action_log
from app.database.crud import device_group as crud_group
from app.database.crud import macro as crud_macro
from app.database.crud import user as crud_user
from app.database.models.device import Device
from app.database.models.device_bond_slot import DeviceBondSlot
from app.database.models.vehicle import VehicleModel
from app.database.schemas.device import DeviceOut
from app.database.schemas.device_group import DeviceGroupOut
from app.database.schemas.macro import MacroOut
from app.database.schemas.vehicle import VehicleOut
from app.database.schemas.ws_message import ActionLogOut, WsEnvelope


def test_device_out_from_attributes(db):
    user = crud_user.create(db, "schema_dev_owner", "Password123", role="user")
    dev = Device(
        device_uuid="esp32_schema_001",
        owner_user_id=user.id,
        name="Schema Device",
        capabilities=["ble_keyboard", "ble_mouse"],
        status="online",
    )
    db.add(dev)
    db.commit()
    db.refresh(dev)

    out = DeviceOut.model_validate(dev)
    assert out.id == dev.id
    assert out.device_uuid == "esp32_schema_001"
    assert out.owner_user_id == user.id
    assert out.name == "Schema Device"
    assert out.capabilities == ["ble_keyboard", "ble_mouse"]
    assert out.status == "online"


def test_device_group_out_from_attributes(db):
    user = crud_user.create(db, "schema_group_owner", "Password123", role="user")
    group = crud_group.create_group(db, user.id, "Schema Group")

    out = DeviceGroupOut.model_validate(group)
    assert out.id == group.id
    assert out.owner_user_id == user.id
    assert out.name == "Schema Group"


def test_macro_out_from_attributes(db):
    user = crud_user.create(db, "schema_macro_owner", "Password123", role="user")
    m = crud_macro.create(
        db,
        user.id,
        "Schema Macro",
        steps=[{"type": "keyboard_tap", "key": "KEY_A"}],
    )

    out = MacroOut.model_validate(m)
    assert out.id == m.id
    assert out.user_id == user.id
    assert out.name == "Schema Macro"
    assert out.content_json["steps"] == [{"type": "keyboard_tap", "key": "KEY_A"}]
    assert out.content_json["config"]["execution_mode"] == "ble_targeted"


def test_vehicle_out_from_attributes(db):
    v = VehicleModel(
        model_name="Model S",
        vehicle_code="SCHEMA001",
        vin="VIN0000000000001",
        chip_platform="esp32s3",
        wheel_size=21,
        has_data_device=True,
        current_status="空闲",
    )
    db.add(v)
    db.commit()
    db.refresh(v)

    out = VehicleOut.model_validate(v)
    assert out.id == v.id
    assert out.model_name == "Model S"
    assert out.chip_platform == "esp32s3"
    assert out.wheel_size == 21
    assert out.has_data_device is True


def test_action_log_out_from_attributes(db):
    log = crud_action_log.create_pending(
        db,
        msg_id=str(uuid.uuid4()),
        user_id=1,
        device_id=10,
        action_type="mouse_move",
        payload={"dx": 10, "dy": 20},
    )
    crud_action_log.mark_acked(db, log.msg_id, exec_time_ms=42)

    out = ActionLogOut.model_validate(log)
    assert out.status == "acked"
    assert out.exec_time_ms == 42
    assert out.error_code is None
    assert out.error_message is None


def test_slot_out_from_row_and_validate(db):
    dev = Device(device_uuid="esp32_slot_schema", name="Slot Dev")
    db.add(dev)
    db.commit()
    db.refresh(dev)

    row = DeviceBondSlot(
        device_id=dev.id,
        slot_index=3,
        addr_type=0,
        addr="AA:BB:CC:DD:EE:FF",
        name="MacBook-Pro",
    )
    db.add(row)
    db.commit()
    db.refresh(row)

    # from_row 工厂方法（last_used_at 为 None 的分支）
    out = SlotOut.from_row(row)
    assert out.slot_index == 3
    assert out.addr == "AA:BB:CC:DD:EE:FF"
    assert out.name == "MacBook-Pro"
    assert out.last_used_at is None

    # from_attributes（model_config ConfigDict）路径：last_used_at=None 时可直接校验
    validated = SlotOut.model_validate(row)
    assert validated.addr == "AA:BB:CC:DD:EE:FF"
    assert validated.slot_index == 3

    # from_row 工厂方法（last_used_at 有值的 isoformat 分支；
    # schema 声明为 Optional[str]，datetime 需经 from_row 转换，这也是 API 采用它的原因）
    row.last_used_at = datetime(2026, 10, 20, 12, 0, 0)
    db.commit()
    db.refresh(row)
    out2 = SlotOut.from_row(row)
    assert out2.last_used_at == "2026-10-20T12:00:00"


def test_ws_envelope_defaults_and_parsing():
    env = WsEnvelope.model_validate(
        {
            "header": {"msg_id": "m-001", "type": "register", "device_id": "esp32_001"},
            "payload": {"device_id": "esp32_001", "capabilities": ["ble_keyboard"]},
        }
    )
    assert env.header.msg_id == "m-001"
    assert env.header.type == "register"
    assert env.header.device_id == "esp32_001"
    assert env.header.source == "web"  # 默认值
    assert env.header.seq == 0
    assert env.routing.broadcast is False  # 默认值
    assert env.routing.target_devices == []
    assert env.payload["capabilities"] == ["ble_keyboard"]

    # 非法消息类型被 Literal 拒绝
    with pytest.raises(ValidationError):
        WsEnvelope.model_validate(
            {"header": {"msg_id": "m-002", "type": "bogus_type"}, "payload": {}}
        )
