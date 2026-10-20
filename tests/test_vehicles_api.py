"""tests/test_vehicles_api.py - 车辆列表与 chip_platform 过滤测试"""
import pytest

from app.database.models.vehicle import VehicleModel


@pytest.fixture(autouse=True)
def seed_vehicles(db):
    db.add_all(
        [
            VehicleModel(
                model_name="Model A",
                vehicle_code="VA001",
                vin="VIN000000000000A",
                chip_platform="esp32s3",
                wheel_size=18,
                has_data_device=True,
                # current_status 不传：验证 ORM 列默认值
            ),
            VehicleModel(
                model_name="Model B",
                vehicle_code="VB002",
                vin="VIN000000000000B",
                chip_platform="stm32",
                wheel_size=19,
                has_data_device=False,
                current_status="空闲",
            ),
        ]
    )
    db.commit()


def test_list_vehicles(client):
    res = client.get("/api/v2/vehicles")
    assert res.status_code == 200
    items = res.json()
    assert len(items) == 2

    codes = {v["vehicle_code"] for v in items}
    assert codes == {"VA001", "VB002"}

    # VehicleOut from_attributes 完整透出字段
    a = next(v for v in items if v["vehicle_code"] == "VA001")
    assert a["model_name"] == "Model A"
    assert a["chip_platform"] == "esp32s3"
    assert a["wheel_size"] == 18
    assert a["has_data_device"] is True
    # 未显式提供时 ORM 列默认值生效，响应校验（str）不再报错
    assert a["current_status"] == "空闲"


def test_filter_vehicles_by_chip(client):
    res = client.get("/api/v2/vehicles", params={"chip": "esp32s3"})
    assert res.status_code == 200
    items = res.json()
    assert len(items) == 1
    assert items[0]["vehicle_code"] == "VA001"

    # 无匹配返回空列表
    assert client.get("/api/v2/vehicles", params={"chip": "nrf52"}).json() == []
