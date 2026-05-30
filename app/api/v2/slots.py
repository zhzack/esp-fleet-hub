"""V2 设备槽位 / 多目标发送 API。"""
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field
from sqlalchemy.orm import Session

from app.core.security import get_current_user
from app.database.crud import device as crud_device
from app.database.crud import device_bond_slot as crud_slot
from app.database.models.user import User
from app.database.session import get_db
from app.services.multi_target import Target, send_to_targets

router = APIRouter()


def _check_owner(db: Session, user: User, device_uuid: str):
    dev = crud_device.get_by_uuid(db, device_uuid)
    if not dev:
        raise HTTPException(404, "device not found")
    if user.role != "admin" and dev.owner_user_id not in (None, user.id):
        raise HTTPException(403, "not your device")
    return dev


# ---------- schemas ----------

class SlotOut(BaseModel):
    slot_index: int
    addr_type: int
    addr: str
    name: Optional[str] = None
    last_used_at: Optional[str] = None

    class Config:
        from_attributes = True

    @classmethod
    def from_row(cls, row):
        return cls(
            slot_index=row.slot_index,
            addr_type=row.addr_type,
            addr=row.addr,
            name=row.name,
            last_used_at=row.last_used_at.isoformat() if row.last_used_at else None,
        )


class SlotIn(BaseModel):
    addr_type: int
    addr: str
    name: Optional[str] = None


class TargetIn(BaseModel):
    addr_type: int
    addr: str
    name: Optional[str] = None


class SendIn(BaseModel):
    action: dict
    slots: Optional[list[int]] = None
    targets: Optional[list[TargetIn]] = None
    require_connected: bool = True
    switch_timeout_s: float = 5.0
    action_timeout_ms: int = 4000


# ---------- slot CRUD ----------

@router.get("/{device_uuid}/slots")
async def list_slots(device_uuid: str,
                     db: Session = Depends(get_db),
                     user: User = Depends(get_current_user)):
    dev = _check_owner(db, user, device_uuid)
    rows = crud_slot.list_for_device(db, dev.id)
    return [SlotOut.from_row(r) for r in rows]


@router.put("/{device_uuid}/slots/{slot_index}")
async def put_slot(device_uuid: str, slot_index: int, body: SlotIn,
                   db: Session = Depends(get_db),
                   user: User = Depends(get_current_user)):
    if slot_index < 1 or slot_index > 9:
        raise HTTPException(400, "slot_index out of range (1..9)")
    dev = _check_owner(db, user, device_uuid)
    row = crud_slot.upsert(db, dev.id, slot_index,
                           addr_type=body.addr_type, addr=body.addr.upper(),
                           name=body.name)
    return SlotOut.from_row(row)


@router.delete("/{device_uuid}/slots/{slot_index}")
async def delete_slot(device_uuid: str, slot_index: int,
                      db: Session = Depends(get_db),
                      user: User = Depends(get_current_user)):
    dev = _check_owner(db, user, device_uuid)
    if not crud_slot.delete(db, dev.id, slot_index):
        raise HTTPException(404, "slot not found")
    return {"ok": True}


# ---------- send 编排 ----------

@router.post("/{device_uuid}/send")
async def send(device_uuid: str, body: SendIn,
               db: Session = Depends(get_db),
               user: User = Depends(get_current_user)):
    dev = _check_owner(db, user, device_uuid)

    # 解析 targets
    targets: list[Target] = []
    if body.slots:
        rows = {r.slot_index: r for r in crud_slot.list_for_device(db, dev.id)}
        for idx in body.slots:
            r = rows.get(idx)
            if not r:
                raise HTTPException(400, f"slot {idx} not assigned")
            targets.append(Target(addr_type=r.addr_type, addr=r.addr,
                                  slot_index=r.slot_index, name=r.name))
    if body.targets:
        for t in body.targets:
            targets.append(Target(addr_type=t.addr_type, addr=t.addr.upper(),
                                  slot_index=None, name=t.name))
    if not targets:
        raise HTTPException(400, "no targets (slots or targets) specified")

    results = await send_to_targets(
        db,
        user_id=user.id,
        device_uuid=device_uuid,
        targets=targets,
        action=body.action,
        require_connected=body.require_connected,
        switch_timeout_s=body.switch_timeout_s,
        action_timeout_ms=body.action_timeout_ms,
    )

    # 标记 slot 最近使用
    for r in results:
        if r.slot_index and r.status == "acked":
            crud_slot.touch(db, dev.id, r.slot_index)

    return {"results": [r.__dict__ for r in results]}
