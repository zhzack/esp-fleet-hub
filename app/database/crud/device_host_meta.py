"""DeviceHostMeta CRUD —— 主机平台标记。"""
from sqlalchemy.orm import Session

from app.database.models.device import Device
from app.database.models.device_host_meta import DeviceHostMeta


def list_for_device(db: Session, device_id: int) -> list[DeviceHostMeta]:
    return (
        db.query(DeviceHostMeta)
        .filter(DeviceHostMeta.device_id == device_id)
        .order_by(DeviceHostMeta.addr)
        .all()
    )


def upsert(
    db: Session,
    device_id: int,
    addr: str,
    *,
    platform: str = "",
    name: str | None = None,
    addr_type: int = 0,
) -> DeviceHostMeta:
    addr = addr.strip().upper()
    row = (
        db.query(DeviceHostMeta)
        .filter(DeviceHostMeta.device_id == device_id, DeviceHostMeta.addr == addr)
        .first()
    )
    if row is None:
        row = DeviceHostMeta(device_id=device_id, addr=addr, addr_type=addr_type)
        db.add(row)
    row.platform = platform
    if name is not None:
        row.name = name
    if addr_type:
        row.addr_type = addr_type
    db.commit()
    db.refresh(row)
    return row
