"""device_bond_slot CRUD。"""
from datetime import datetime, timezone
from sqlalchemy.orm import Session

from app.database.models.device_bond_slot import DeviceBondSlot


def list_for_device(db: Session, device_id: int) -> list[DeviceBondSlot]:
    return (
        db.query(DeviceBondSlot)
        .filter(DeviceBondSlot.device_id == device_id)
        .order_by(DeviceBondSlot.slot_index)
        .all()
    )


def get(db: Session, device_id: int, slot_index: int) -> DeviceBondSlot | None:
    return (
        db.query(DeviceBondSlot)
        .filter(
            DeviceBondSlot.device_id == device_id,
            DeviceBondSlot.slot_index == slot_index,
        )
        .first()
    )


def upsert(
    db: Session,
    device_id: int,
    slot_index: int,
    *,
    addr_type: int,
    addr: str,
    name: str | None = None,
) -> DeviceBondSlot:
    row = get(db, device_id, slot_index)
    if row is None:
        row = DeviceBondSlot(
            device_id=device_id,
            slot_index=slot_index,
            addr_type=addr_type,
            addr=addr,
            name=name,
        )
        db.add(row)
    else:
        row.addr_type = addr_type
        row.addr = addr
        if name is not None:
            row.name = name
    db.commit()
    db.refresh(row)
    return row


def delete(db: Session, device_id: int, slot_index: int) -> bool:
    row = get(db, device_id, slot_index)
    if not row:
        return False
    db.delete(row)
    db.commit()
    return True


def touch(db: Session, device_id: int, slot_index: int) -> None:
    row = get(db, device_id, slot_index)
    if row:
        row.last_used_at = datetime.now(timezone.utc)
        db.commit()
